/*******************************************************************************************
 *   brush editor — Dear ImGui + ImGuizmo scene editor over libbrush.
 *
 *   Authoring happens in the FINAL renderer: the viewport shows the real HDR
 *   pipeline (PCSS shadows, bloom, tone map) while you drag lights around.
 *   Edits write world.def files (b_scene); the running game hot-reloads them.
 *
 *   Navigation is TRACKPAD-FIRST (Blender-style):
 *     two-finger scroll        orbit around the focus point
 *     shift + scroll           pan
 *     cmd/ctrl + scroll        zoom (dolly, distance-proportional)
 *     right-drag               fly-look + WASD/QE (mouse users / precise moves)
 *     F                        frame the selected entity
 *     W / E / R                gizmo: move / rotate / scale
 *     cmd+S save · cmd+D duplicate · backspace/delete remove · F5 play/stop
 ********************************************************************************************/

#include "brush.h"
#include "rlImGui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include <raymath.h>
#include <rlgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#if defined(__APPLE__)
// macos_window.mm — Godot-style seamless titlebar (content under the
// titlebar, traffic lights floating over the menu bar row).
extern "C" void EditorMacSeamlessTitlebar(void *nsWindow);
extern "C" void EditorMacDragWindow(void *nsWindow);
extern "C" void EditorMacInstallMenu(void);
extern "C" int EditorMacPollMenuAction(void);
#endif

// ---------------------------------------------------------------------------
// Editor camera: orbit-centric (focus + yaw/pitch/distance), with a fly mode
// on right-drag. Orbit/pan/zoom are all distance-proportional so the feel is
// constant whether you're framing a crate or the whole arena.
// ---------------------------------------------------------------------------
struct EditorCamera {
    Camera3D cam;
    Vector3 focus;      // orbit pivot
    float dist;         // boom length
    float yaw, pitch;   // radians
    float flySpeed;

    void Init() {
        focus = (Vector3){ 0.0f, 1.0f, -10.0f };
        dist = 26.0f;
        yaw = 0.0f;             // looking -Z (into the gym)
        pitch = -0.45f;
        flySpeed = 10.0f;
        cam.up = (Vector3){ 0, 1, 0 };
        cam.fovy = 60.0f;
        cam.projection = CAMERA_PERSPECTIVE;
        Sync();
    }

    Vector3 Forward() const {
        return (Vector3){ cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw) };
    }
    Vector3 Right() const {
        return (Vector3){ sinf(yaw - PI / 2.0f), 0.0f, cosf(yaw - PI / 2.0f) };
    }

    void Sync() {
        Vector3 f = Forward();
        cam.position = Vector3Subtract(focus, Vector3Scale(f, dist));
        cam.target = focus;
    }

    void Frame(Vector3 center, float radius) {
        focus = center;
        dist = fmaxf(radius * 2.8f, 2.0f);
        Sync();
    }

    void Update(float dt, bool flying, bool viewportHovered) {
        // --- Trackpad gestures (both scroll axes) ------------------------
        if (viewportHovered && !flying) {
            Vector2 wheel = GetMouseWheelMoveV();
            if (wheel.x != 0.0f || wheel.y != 0.0f) {
                bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                bool cmd = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
                           IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (cmd) {
                    // Zoom: exponential dolly — a swipe covers a big range but
                    // never overshoots through the focus.
                    dist *= powf(0.94f, wheel.y);
                    dist = Clamp(dist, 0.8f, 400.0f);
                } else if (shift) {
                    // Pan: screen-space, proportional to distance.
                    Vector3 up = Vector3CrossProduct(Right(), Forward());
                    float k = dist * 0.02f;
                    focus = Vector3Add(focus, Vector3Scale(Right(), -wheel.x * k));
                    focus = Vector3Add(focus, Vector3Scale(up, -wheel.y * k));
                } else {
                    // Orbit (Blender trackpad feel).
                    yaw -= wheel.x * 0.05f;
                    pitch += wheel.y * 0.05f;
                    pitch = Clamp(pitch, -1.45f, 1.45f);
                }
            }
        }

        // --- Fly mode (right-drag): mouse-look + WASD/QE ------------------
        if (flying) {
            if (!IsCursorHidden()) {
                DisableCursor();
                GetMouseDelta(); // swallow the snap delta
            } else {
                Vector2 d = GetMouseDelta();
                yaw -= d.x * 0.003f;
                pitch -= d.y * 0.003f;
                pitch = Clamp(pitch, -1.45f, 1.45f);
            }

            float sp = flySpeed * (IsKeyDown(KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
            Vector3 f = Forward(), r = Right();
            Vector3 move = { 0, 0, 0 };
            if (IsKeyDown(KEY_W)) move = Vector3Add(move, f);
            if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, f);
            if (IsKeyDown(KEY_D)) move = Vector3Add(move, r);
            if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, r);
            if (IsKeyDown(KEY_E)) move.y += 1.0f;
            if (IsKeyDown(KEY_Q)) move.y -= 1.0f;
            // Fly moves the whole rig: the focus rides in front of the camera.
            focus = Vector3Add(focus, Vector3Scale(move, sp * dt));
            // Fly zoom on the scroll wheel too (speed adjust, like UE).
            float w = GetMouseWheelMove();
            if (w != 0.0f) flySpeed = Clamp(flySpeed * powf(1.12f, w), 1.0f, 80.0f);
        } else if (IsCursorHidden()) {
            EnableCursor();
        }

        Sync();
    }
};

// ---------------------------------------------------------------------------
// Editor state
// ---------------------------------------------------------------------------
enum EntityType { ENTITY_NONE, ENTITY_BLOCK, ENTITY_LIGHT, ENTITY_SPAWN };

static BrushPhysics g_phys;
static BrushScene g_scene;
static BrushWorld *g_world = NULL;
static Texture2D g_groundTex;
static Model g_unitCube;
static Model g_spawnMarker;

static JPH_BodyID g_blockBodies[BRUSH_SCENE_MAX_BLOCKS];
static int g_blockBodyCount = 0;

static BrushTimeOfDay g_tod;
static EditorCamera g_camera;

static EntityType g_selectedType = ENTITY_NONE;
static int g_selectedIdx = -1;

static ImGuizmo::OPERATION g_gizmoOp = ImGuizmo::TRANSLATE;

// --- Terrain sculpting ---
enum EditorMode { MODE_SELECT, MODE_SCULPT };
static EditorMode g_mode = MODE_SELECT;
static BrushSculptOp g_sculptOp = BRUSH_SCULPT_ADD;
static float g_brushRadius = 6.0f;
static float g_brushStrength = 1.0f;
static bool g_sculptStroking = false;
static float g_flattenY = 0.0f;
static bool g_cursorOnGround = false;
static Vector3 g_cursorPos = {0, 0, 0};
static char g_terrainPath[512] = "assets/gym.terrain";

#define MAX_UNDO 32
static unsigned char *g_undoBlobs[MAX_UNDO];
static int g_undoSizes[MAX_UNDO];
static int g_undoCount = 0;


static bool g_dirty = false;
static bool g_surfaceSnap = true;
static bool g_quit = false;
static char g_scenePath[512] = "assets/gym.def";

// --- In-editor log ----------------------------------------------------------
#define MAX_LOG_LINES 128
static char g_logLines[MAX_LOG_LINES][256];
static int g_logLineCount = 0;

static void AddEditorLog(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_logLineCount == MAX_LOG_LINES) {
        memmove(g_logLines[0], g_logLines[1], sizeof(g_logLines[0]) * (MAX_LOG_LINES - 1));
        g_logLineCount--;
    }
    snprintf(g_logLines[g_logLineCount++], 256, "%s", buf);
}

static void PushSculptUndo() {
    int size = 0;
    // Whole-overlay snapshot (editor scale keeps this small; tiles are sparse).
    unsigned char *blob = BrushWorldSculptSnapshot(
        g_world, (Vector2){-32000, -32000}, (Vector2){32000, 32000}, &size);
    if (blob == NULL) return;
    if (g_undoCount == MAX_UNDO) { // drop the oldest
        MemFree(g_undoBlobs[0]);
        memmove(g_undoBlobs, g_undoBlobs + 1, sizeof(g_undoBlobs[0]) * (MAX_UNDO - 1));
        memmove(g_undoSizes, g_undoSizes + 1, sizeof(g_undoSizes[0]) * (MAX_UNDO - 1));
        g_undoCount--;
    }
    g_undoBlobs[g_undoCount] = blob;
    g_undoSizes[g_undoCount] = size;
    g_undoCount++;
}

static void PopSculptUndo() {
    if (g_undoCount == 0) return;
    g_undoCount--;
    BrushWorldSculptRestore(g_world, g_undoBlobs[g_undoCount], g_undoSizes[g_undoCount]);
    MemFree(g_undoBlobs[g_undoCount]);
    g_dirty = true;
    AddEditorLog("Undo sculpt");
}

// --- Game process (Play button) ----------------------------------------------
static pid_t g_gamePid = -1;

static bool GameRunning() {
    if (g_gamePid <= 0) return false;
    int status;
    if (waitpid(g_gamePid, &status, WNOHANG) == 0) return true;
    g_gamePid = -1;
    return false;
}

static void SaveScene() {
    g_scene.timeHours = g_tod.timeHours;
    if (BrushSceneSave(&g_scene, g_scenePath)) {
        g_dirty = false;
        AddEditorLog("Saved %s", g_scenePath);
    } else {
        AddEditorLog("ERROR: could not save %s", g_scenePath);
    }
    if (g_world && BrushWorldSculptAny(g_world)) {
        if (BrushWorldSculptSave(g_world, g_terrainPath))
            AddEditorLog("Saved %s", g_terrainPath);
    }
}

static void PlayGame() {
    SaveScene(); // the game loads (and hot-reloads) the same file
    if (GameRunning()) { AddEditorLog("Game already running."); return; }
    g_gamePid = fork();
    if (g_gamePid == 0) {
        char *args[] = { (char *)"./build/sandbox", NULL };
        execvp(args[0], args);
        perror("execvp failed");
        _exit(1);
    }
    AddEditorLog(g_gamePid > 0 ? "Launched game." : "Failed to launch game.");
    if (g_gamePid < 0) g_gamePid = -1;
}

static void StopGame() {
    if (GameRunning()) {
        kill(g_gamePid, SIGTERM); // graceful: the game runs its shutdown path
        AddEditorLog("Stopped game.");
    }
    g_gamePid = -1;
}

// --- Scene ops ---------------------------------------------------------------
static void RebuildAllColliders() {
    for (int i = 0; i < g_blockBodyCount; i++)
        if (g_blockBodies[i] != BRUSH_BODY_INVALID)
            BrushPhysicsRemoveBody(&g_phys, g_blockBodies[i]);
    g_blockBodyCount = 0;
    for (int i = 0; i < g_scene.blockCount; i++) {
        BrushSceneBlock *k = &g_scene.blocks[i];
        g_blockBodies[g_blockBodyCount++] =
            BrushPhysicsAddStaticBox(&g_phys, k->pos, k->size, k->rot, i, "block");
    }
}

static void ReloadScene() {
    if (BrushSceneLoad(&g_scene, g_scenePath)) {
        g_dirty = false;
        g_tod.timeHours = g_scene.timeHours >= 0.0f ? g_scene.timeHours : 12.0f;
        RebuildAllColliders();
        g_selectedType = ENTITY_NONE;
        g_selectedIdx = -1;
        AddEditorLog("Reloaded %s", g_scenePath);
    }
}


static Vector3 SpawnPointInFront() {
    return Vector3Add(g_camera.cam.position,
                      Vector3Scale(g_camera.Forward(), fminf(g_camera.dist, 8.0f)));
}

static void AddBlockEntity() {
    if (g_scene.blockCount >= BRUSH_SCENE_MAX_BLOCKS) return;
    g_scene.blocks[g_scene.blockCount] =
        (BrushSceneBlock){ SpawnPointInFront(), {2, 2, 2}, {0, 0, 0}, (Color){200, 200, 200, 255} };
    g_selectedType = ENTITY_BLOCK;
    g_selectedIdx = g_scene.blockCount++;
    RebuildAllColliders();
    g_dirty = true;
    AddEditorLog("Added block %d", g_selectedIdx);
}

static void AddLightEntity() {
    if (g_scene.lightCount >= BRUSH_SCENE_MAX_LIGHTS) return;
    g_scene.lights[g_scene.lightCount] =
        (BrushSceneLight){ .light = { SpawnPointInFront(), {1.8f, 1.2f, 0.6f}, 6.0f }, .flicker = false };
    g_selectedType = ENTITY_LIGHT;
    g_selectedIdx = g_scene.lightCount++;
    g_dirty = true;
    AddEditorLog("Added light %d", g_selectedIdx);
}

static void DeleteSelected() {
    if (g_selectedType == ENTITY_BLOCK && g_selectedIdx >= 0) {
        for (int i = g_selectedIdx; i < g_scene.blockCount - 1; i++)
            g_scene.blocks[i] = g_scene.blocks[i + 1];
        g_scene.blockCount--;
        RebuildAllColliders();
        AddEditorLog("Deleted block");
    } else if (g_selectedType == ENTITY_LIGHT && g_selectedIdx >= 0) {
        for (int i = g_selectedIdx; i < g_scene.lightCount - 1; i++)
            g_scene.lights[i] = g_scene.lights[i + 1];
        g_scene.lightCount--;
        AddEditorLog("Deleted light");
    } else {
        return;
    }
    g_selectedType = ENTITY_NONE;
    g_selectedIdx = -1;
    g_dirty = true;
}

static void DuplicateSelected() {
    if (g_selectedType == ENTITY_BLOCK && g_selectedIdx >= 0 &&
        g_scene.blockCount < BRUSH_SCENE_MAX_BLOCKS) {
        BrushSceneBlock copy = g_scene.blocks[g_selectedIdx];
        copy.pos.x += copy.size.x * 0.5f + 0.5f;
        g_scene.blocks[g_scene.blockCount] = copy;
        g_selectedIdx = g_scene.blockCount++;
        RebuildAllColliders();
        g_dirty = true;
        AddEditorLog("Duplicated block -> %d", g_selectedIdx);
    } else if (g_selectedType == ENTITY_LIGHT && g_selectedIdx >= 0 &&
               g_scene.lightCount < BRUSH_SCENE_MAX_LIGHTS) {
        BrushSceneLight copy = g_scene.lights[g_selectedIdx];
        copy.light.position.x += 1.0f;
        g_scene.lights[g_scene.lightCount] = copy;
        g_selectedIdx = g_scene.lightCount++;
        g_dirty = true;
        AddEditorLog("Duplicated light -> %d", g_selectedIdx);
    }
}

// Frame the current selection (F).
static void FrameSelected() {
    if (g_selectedType == ENTITY_BLOCK && g_selectedIdx >= 0) {
        BrushSceneBlock *b = &g_scene.blocks[g_selectedIdx];
        float r = fmaxf(fmaxf(b->size.x, b->size.y), b->size.z) * 0.75f;
        g_camera.Frame(b->pos, r);
    } else if (g_selectedType == ENTITY_LIGHT && g_selectedIdx >= 0) {
        g_camera.Frame(g_scene.lights[g_selectedIdx].light.position, 1.5f);
    } else if (g_selectedType == ENTITY_SPAWN) {
        g_camera.Frame(g_scene.spawn, 1.5f);
    } else {
        g_camera.Frame((Vector3){ 0, 1, -14 }, 14.0f); // whole gym
    }
}

static void ColorToFloat3(Color c, float out[3]) {
    out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f;
}
static Color Float3ToColor(const float v[3]) {
    return (Color){ (unsigned char)(Clamp(v[0], 0, 1) * 255.0f),
                    (unsigned char)(Clamp(v[1], 0, 1) * 255.0f),
                    (unsigned char)(Clamp(v[2], 0, 1) * 255.0f), 255 };
}

// ---------------------------------------------------------------------------
// UI style: a restrained dark theme — near-black panels, soft rounding, one
// blue accent. Loads a real system font (crisp at retina) when available.
// ---------------------------------------------------------------------------
static void ApplyEditorStyle() {
    ImGuiIO &io = ImGui::GetIO();
    // UI font at 17 logical points, rasterized at the monitor's DPI scale so
    // it stays crisp on retina. FontGlobalScale brings the oversized atlas
    // back to logical size. (Fonts only take effect because this runs before
    // rlImGuiEndInitImGui, which bakes the atlas.)
    float dpi = fmaxf(GetWindowScaleDPI().y, 1.0f);
    const char *fontCandidates[] = {
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char *path : fontCandidates) {
        if (!FileExists(path)) continue;
        ImFont *f = io.Fonts->AddFontFromFileTTF(path, 15.0f * dpi);
        if (f) {
            io.FontDefault = f;
            io.FontGlobalScale = 1.0f / dpi;
        }
        break;
    }

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 16.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 10.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4 *c = s.Colors;
    const ImVec4 bg0(0.086f, 0.090f, 0.106f, 1.00f);  // deepest (title/menu)
    const ImVec4 bg1(0.110f, 0.115f, 0.133f, 1.00f);  // panels
    const ImVec4 bg2(0.150f, 0.157f, 0.180f, 1.00f);  // widgets
    const ImVec4 bg3(0.196f, 0.204f, 0.235f, 1.00f);  // hovered
    const ImVec4 acc(0.310f, 0.550f, 1.000f, 1.00f);  // brush blue
    const ImVec4 accDim(0.310f, 0.550f, 1.000f, 0.55f);
    const ImVec4 text(0.847f, 0.855f, 0.875f, 1.00f);
    const ImVec4 textDim(0.500f, 0.510f, 0.540f, 1.00f);

    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = bg1;
    c[ImGuiCol_PopupBg] = bg0;
    c[ImGuiCol_Border] = ImVec4(0, 0, 0, 0.35f);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive] = bg3;
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = bg1;
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = accDim;
    c[ImGuiCol_ScrollbarGrabActive] = acc;
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_SliderGrab] = accDim;
    c[ImGuiCol_SliderGrabActive] = acc;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive] = accDim;
    c[ImGuiCol_Header] = bg2;
    c[ImGuiCol_HeaderHovered] = bg3;
    c[ImGuiCol_HeaderActive] = accDim;
    c[ImGuiCol_Separator] = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_SeparatorHovered] = accDim;
    c[ImGuiCol_SeparatorActive] = acc;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = accDim;
    c[ImGuiCol_ResizeGripActive] = acc;
    c[ImGuiCol_Tab] = bg0;
    c[ImGuiCol_TabHovered] = bg3;
    c[ImGuiCol_TabActive] = bg1;
    c[ImGuiCol_TabUnfocused] = bg0;
    c[ImGuiCol_TabUnfocusedActive] = bg1;
    c[ImGuiCol_DockingPreview] = accDim;
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_TextSelectedBg] = accDim;
    c[ImGuiCol_NavHighlight] = acc;
    c[ImGuiCol_DragDropTarget] = acc;
}

// Toolbar toggle button with an active accent.
static bool ToolButton(const char *label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.310f, 0.550f, 1.000f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.310f, 0.550f, 1.000f, 0.60f));
    }
    bool pressed = ImGui::Button(label);
    if (active) ImGui::PopStyleColor(2);
    return pressed;
}

// ---------------------------------------------------------------------------
int main() {
    BrushConsoleInit();
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(1600, 900, "brush editor");
#if defined(__APPLE__)
    EditorMacSeamlessTitlebar(GetWindowHandle());
    EditorMacInstallMenu(); // File/Edit/View live in the macOS top bar
#endif
    SetWindowState(FLAG_WINDOW_MAXIMIZED); // fill whatever screen this is
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); // ESC deselects, never quits

    BrushRenderInit(GetScreenWidth(), GetScreenHeight(), 1.0f);
    BrushRenderSetEditorMode(true);
    BrushPhysicsInit(&g_phys);

    if (!BrushSceneLoad(&g_scene, g_scenePath)) {
        g_scene.spawn = (Vector3){ 0.0f, 0.5f, 8.0f };
        g_scene.timeHours = 12.0f;
        BrushSceneSave(&g_scene, g_scenePath);
        AddEditorLog("Created new scene %s", g_scenePath);
    } else {
        AddEditorLog("Loaded %s (%d blocks, %d lights)", g_scenePath,
                     g_scene.blockCount, g_scene.lightCount);
    }

    BrushTodInit(&g_tod);
    g_tod.timeHours = g_scene.timeHours >= 0.0f ? g_scene.timeHours : 12.0f;

    Image checker = GenImageChecked(1024, 1024, 32, 32, RAYWHITE, (Color){196, 199, 206, 255});
    g_groundTex = LoadTextureFromImage(checker);
    UnloadImage(checker);
    GenTextureMipmaps(&g_groundTex);
    SetTextureFilter(g_groundTex, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(g_groundTex, TEXTURE_WRAP_REPEAT);

    g_world = BrushWorldCreate(
        (BrushWorldConfig){ .seed = 1337, .heightFn = NULL, .chunkSize = 64.0f,
                            .loadRadius = 3, .physics = &g_phys,
                            .groundTex = g_groundTex, .texMetresPerTile = 64.0f },
        (Vector3){ g_scene.spawn.x, 0, g_scene.spawn.z });

    BrushWorldSculptLoad(g_world, g_terrainPath);

    // Harness: scripted sculpt + save (headless end-to-end check of the
    // editor -> .terrain -> game pipeline).
    if (getenv("BRUSH_TEST_SCULPT") != NULL) {
        for (int i = 0; i < 40; i++)
            BrushWorldSculpt(g_world, BRUSH_SCULPT_ADD, (Vector3){0, 0, 14},
                             9.0f, 0.09f, 0.0f);
        BrushWorldSculptSave(g_world, g_terrainPath);
    }

    g_unitCube = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
    g_unitCube.materials[0].shader = BrushGetLitShader();

    g_spawnMarker = LoadModel("assets/character/mannequin.glb");
    for (int i = 0; i < g_spawnMarker.materialCount; i++)
        g_spawnMarker.materials[i].shader = BrushGetLitShader();

    RebuildAllColliders();
    g_camera.Init();

    rlImGuiBeginInitImGui();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Own layout file (imgui.ini may hold layouts from older window names).
    ImGui::GetIO().IniFilename = "editor_layout.ini";
    ApplyEditorStyle(); // loads the UI font — must run before EndInitImGui
    rlImGuiEndInitImGui(); // bakes the font atlas


    bool viewportHovered = false;

    while (!g_quit && !WindowShouldClose()) {
        float dt = GetFrameTime();

        if (IsWindowResized())
            BrushRenderResize(GetScreenWidth(), GetScreenHeight());

        // --- Scene update -------------------------------------------------
        BrushTodUpdate(&g_tod, dt);
        BrushRenderApplyTimeOfDay(&g_tod);
        BrushWorldUpdate(g_world, g_camera.cam.position);
        // Nothing is dynamic here, but Jolt's broadphase does its node
        // maintenance inside Update — without stepping, editing churn
        // eventually aborts with "QuadTree: Out of nodes!".
        BrushPhysicsStep(&g_phys, dt);

        static bool isFlying = false;
        if (viewportHovered && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) isFlying = true;
        if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) isFlying = false;
        g_camera.Update(dt, isFlying, viewportHovered);

        // --- Submit scene to the layered renderer --------------------------
        BrushWorldSubmit(g_world, g_camera.cam);

        for (int i = 0; i < g_scene.blockCount; i++) {
            BrushSceneBlock *k = &g_scene.blocks[i];
            Matrix xf = BrushBlockGetModelMatrix(k); // includes rotation
            BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &g_unitCube, xf, k->color);
            BrushRenderSubmit(BRUSH_LAYER_SHADOW, &g_unitCube, xf, k->color);
        }

        float tt = (float)GetTime();
        for (int i = 0; i < g_scene.lightCount; i++) {
            BrushSceneLight *l = &g_scene.lights[i];
            float fl = 1.0f;
            if (l->flicker)
                fl = 0.85f + 0.11f * sinf(tt * 9.0f + i * 2.1f) +
                     0.06f * sinf(tt * 23.7f + i * 4.3f);
            BrushPointLight pl = l->light;
            pl.color = (Vector3){ pl.color.x * fl, pl.color.y * fl, pl.color.z * fl };
            BrushRenderSubmitPointLight(pl);

            Matrix mk = MatrixMultiply(MatrixScale(0.25f, 0.25f, 0.25f),
                                       MatrixTranslate(l->light.position.x,
                                                       l->light.position.y,
                                                       l->light.position.z));
            BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &g_unitCube, mk, (Color){255, 190, 90, 255});
        }

        Matrix spawnXf = MatrixTranslate(g_scene.spawn.x, g_scene.spawn.y, g_scene.spawn.z);
        BrushRenderSubmit(BRUSH_LAYER_OPAQUE, &g_spawnMarker, spawnXf, (Color){0, 255, 120, 255});

        // --- Render -------------------------------------------------------
        BeginDrawing();
        ClearBackground((Color){ 22, 23, 27, 255 });

        BrushRenderExecute(g_camera.cam); // editorMode: composites, no present

        BrushPost *pp = BrushRenderGetPost();

        // Sculpt brush cursor: a ring hugging the terrain under the mouse.
        if (pp && g_mode == MODE_SCULPT && g_cursorOnGround) {
            RenderTexture2D *target = pp->smaaEnabled ? &pp->presentAA : &pp->present;
            BeginTextureMode(*target);
            BeginMode3D(g_camera.cam);
            Color ringCol = (g_sculptOp == BRUSH_SCULPT_ADD)    ? GREEN
                            : (g_sculptOp == BRUSH_SCULPT_SMOOTH) ? SKYBLUE
                                                                  : GOLD;
            const int SEGS = 48;
            Vector3 prev = {0};
            for (int i = 0; i <= SEGS; i++) {
                float a = (float)i / SEGS * 2.0f * PI;
                Vector3 pt = { g_cursorPos.x + cosf(a) * g_brushRadius, 0,
                               g_cursorPos.z + sinf(a) * g_brushRadius };
                pt.y = BrushWorldGroundHeight(g_world, pt.x, pt.z) + 0.08f;
                if (i > 0) DrawLine3D(prev, pt, ringCol);
                prev = pt;
            }
            DrawLine3D((Vector3){ g_cursorPos.x, g_cursorPos.y, g_cursorPos.z },
                       (Vector3){ g_cursorPos.x, g_cursorPos.y + 1.0f, g_cursorPos.z },
                       Fade(ringCol, 0.6f));
            EndMode3D();
            EndTextureMode();
        }

        // Selection outline drawn onto the final LDR target (respects rotation).
        if (pp && g_mode == MODE_SELECT && g_selectedType != ENTITY_NONE) {
            RenderTexture2D *target = pp->smaaEnabled ? &pp->presentAA : &pp->present;
            BeginTextureMode(*target);
            BeginMode3D(g_camera.cam);
            if (g_selectedType == ENTITY_BLOCK && g_selectedIdx >= 0 &&
                g_selectedIdx < g_scene.blockCount) {
                BrushSceneBlock *b = &g_scene.blocks[g_selectedIdx];
                rlPushMatrix();
                Matrix xf = BrushBlockGetModelMatrix(b);
                rlMultMatrixf(MatrixToFloat(xf));
                DrawCubeWires((Vector3){0, 0, 0}, 1.02f, 1.02f, 1.02f, GREEN);
                rlPopMatrix();
            } else if (g_selectedType == ENTITY_LIGHT && g_selectedIdx >= 0 &&
                       g_selectedIdx < g_scene.lightCount) {
                BrushSceneLight *l = &g_scene.lights[g_selectedIdx];
                DrawCubeWires(l->light.position, 0.35f, 0.35f, 0.35f, GOLD);
                DrawSphereWires(l->light.position, l->light.radius, 12, 12,
                                Fade(GOLD, 0.25f));
            } else if (g_selectedType == ENTITY_SPAWN) {
                DrawCubeWires((Vector3){ g_scene.spawn.x, g_scene.spawn.y + 0.9f,
                                         g_scene.spawn.z }, 0.9f, 1.9f, 0.9f, SKYBLUE);
            }
            EndMode3D();
            EndTextureMode();
        }

        // --- ImGui frame ----------------------------------------------------
        rlImGuiBegin();
        ImGuizmo::BeginFrame();

        ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // Default layout only when none exists yet — imgui.ini keeps the
        // user's arrangement across sessions.
        ImGuiDockNode *rootNode = ImGui::DockBuilderGetNode(dockspaceId);
        if (rootNode == NULL || rootNode->IsLeafNode()) {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_None);
            ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);
            ImGuiID dockLeft, dockCenter, dockRight, dockView, dockBottom, dockViewFinal;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.16f, &dockLeft, &dockCenter);
            ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.24f, &dockRight, &dockView);
            ImGui::DockBuilderSplitNode(dockView, ImGuiDir_Down, 0.22f, &dockBottom, &dockViewFinal);
            ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
            ImGui::DockBuilderDockWindow("Viewport", dockViewFinal);
            ImGui::DockBuilderDockWindow("Inspector", dockRight);
            ImGui::DockBuilderDockWindow("Environment", dockRight);
            ImGui::DockBuilderDockWindow("Console", dockBottom);
            ImGui::DockBuilderFinish(dockspaceId);
        }

        // === Menu bar ========================================================
#if defined(__APPLE__)
        // The menus live in the NATIVE macOS top bar (installed at startup);
        // poll and dispatch their actions here.
        switch (EditorMacPollMenuAction()) {
        case 1: SaveScene(); break;
        case 2: ReloadScene(); break;
        case 3: g_quit = true; break;
        case 4: AddBlockEntity(); break;
        case 5: AddLightEntity(); break;
        case 6: DuplicateSelected(); break;
        case 7: DeleteSelected(); break;
        case 8: FrameSelected(); break;
        case 9: g_selectedType = ENTITY_NONE; g_selectedIdx = -1; FrameSelected(); break;
        case 10: GameRunning() ? StopGame() : PlayGame(); break;
        case 11: PopSculptUndo(); break;
        case 100: AddEditorLog("brush editor — github.com/don-ciccio/brush"); break;
        default: break;
        }

        // Drag the window from the (otherwise empty) top strip, like a
        // titlebar — the dock tab row sits under the traffic lights.
        if (GetMousePosition().y < 30.0f &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive())
            EditorMacDragWindow(GetWindowHandle());
#else
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save", "Ctrl+S")) SaveScene();
                if (ImGui::MenuItem("Reload from Disk")) ReloadScene();
                ImGui::Separator();
                if (ImGui::MenuItem("Quit Editor", "Ctrl+Q")) g_quit = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Add Block", "B")) AddBlockEntity();
                if (ImGui::MenuItem("Add Light", "L")) AddLightEntity();
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, g_selectedType != ENTITY_NONE))
                    DuplicateSelected();
                if (ImGui::MenuItem("Delete", "Backspace", false, g_selectedType != ENTITY_NONE))
                    DeleteSelected();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Frame Selection", "F")) FrameSelected();
                if (ImGui::MenuItem("Frame Scene", "Shift+F")) {
                    g_selectedType = ENTITY_NONE; FrameSelected();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
#endif

        // === Hierarchy =======================================================
        ImGui::Begin("Hierarchy");
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool sel = (g_selectedType == ENTITY_SPAWN);
            if (ImGui::Selectable("  Spawn Point", sel)) {
                g_selectedType = ENTITY_SPAWN; g_selectedIdx = 0;
            }
        }
        char blocksHdr[32];
        snprintf(blocksHdr, sizeof(blocksHdr), "Blocks (%d)###blocks", g_scene.blockCount);
        if (ImGui::CollapsingHeader(blocksHdr, ImGuiTreeNodeFlags_DefaultOpen))
        for (int i = 0; i < g_scene.blockCount; i++) {
            char label[96];
            BrushSceneBlock *b = &g_scene.blocks[i];
            snprintf(label, sizeof(label), "  Block %d  (%.1f x %.1f x %.1f)##b%d",
                     i, b->size.x, b->size.y, b->size.z, i);
            bool sel = (g_selectedType == ENTITY_BLOCK && g_selectedIdx == i);
            if (ImGui::Selectable(label, sel)) { g_selectedType = ENTITY_BLOCK; g_selectedIdx = i; }
            if (ImGui::BeginPopupContextItem()) {
                g_selectedType = ENTITY_BLOCK; g_selectedIdx = i;
                if (ImGui::MenuItem("Frame")) FrameSelected();
                if (ImGui::MenuItem("Duplicate")) DuplicateSelected();
                if (ImGui::MenuItem("Delete")) DeleteSelected();
                ImGui::EndPopup();
            }
        }
        char lightsHdr[32];
        snprintf(lightsHdr, sizeof(lightsHdr), "Lights (%d)###lights", g_scene.lightCount);
        if (ImGui::CollapsingHeader(lightsHdr, ImGuiTreeNodeFlags_DefaultOpen))
        for (int i = 0; i < g_scene.lightCount; i++) {
            char label[64];
            snprintf(label, sizeof(label), "  Light %d%s##l%d", i,
                     g_scene.lights[i].flicker ? "  (flicker)" : "", i);
            bool sel = (g_selectedType == ENTITY_LIGHT && g_selectedIdx == i);
            if (ImGui::Selectable(label, sel)) { g_selectedType = ENTITY_LIGHT; g_selectedIdx = i; }
            if (ImGui::BeginPopupContextItem()) {
                g_selectedType = ENTITY_LIGHT; g_selectedIdx = i;
                if (ImGui::MenuItem("Frame")) FrameSelected();
                if (ImGui::MenuItem("Duplicate")) DuplicateSelected();
                if (ImGui::MenuItem("Delete")) DeleteSelected();
                ImGui::EndPopup();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("+ Block", ImVec2(w, 0))) AddBlockEntity();
        ImGui::SameLine();
        if (ImGui::Button("+ Light", ImVec2(w, 0))) AddLightEntity();
        ImGui::End();

        // === Inspector =======================================================
        ImGui::Begin("Inspector");
        if (g_selectedType == ENTITY_NONE) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::Spacing();
            ImGui::TextWrapped("Click an entity in the viewport or the hierarchy.");
        } else if (g_selectedType == ENTITY_SPAWN) {
            ImGui::SeparatorText("Spawn Point");
            float sp[3] = { g_scene.spawn.x, g_scene.spawn.y, g_scene.spawn.z };
            if (ImGui::DragFloat3("Position", sp, 0.1f)) {
                g_scene.spawn = (Vector3){ sp[0], sp[1], sp[2] };
                g_dirty = true;
            }
        } else if (g_selectedType == ENTITY_BLOCK && g_selectedIdx >= 0 &&
                   g_selectedIdx < g_scene.blockCount) {
            BrushSceneBlock *b = &g_scene.blocks[g_selectedIdx];
            ImGui::SeparatorText(TextFormat("Block %d", g_selectedIdx));
            float pos[3] = { b->pos.x, b->pos.y, b->pos.z };
            float rot[3] = { b->rot.x, b->rot.y, b->rot.z };
            float size[3] = { b->size.x, b->size.y, b->size.z };
            float col[3]; ColorToFloat3(b->color, col);
            bool changed = false;
            if (ImGui::DragFloat3("Position", pos, 0.1f)) {
                b->pos = (Vector3){ pos[0], pos[1], pos[2] }; changed = true;
            }
            if (ImGui::DragFloat3("Rotation", rot, 1.0f, -360.0f, 360.0f, "%.1f deg")) {
                b->rot = (Vector3){ rot[0], rot[1], rot[2] }; changed = true;
            }
            if (ImGui::DragFloat3("Size", size, 0.1f, 0.1f, 200.0f)) {
                b->size = (Vector3){ size[0], size[1], size[2] }; changed = true;
            }
            // Rebuild colliders when the edit ENDS (slider released / typing
            // done), not per drag tick — per-tick churn floods Jolt.
            static bool inspectorStale = false;
            if (changed) { g_dirty = true; inspectorStale = true; }
            if (inspectorStale && !ImGui::IsAnyItemActive()) {
                RebuildAllColliders();
                inspectorStale = false;
            }
            if (ImGui::ColorEdit3("Color", col)) {
                b->color = Float3ToColor(col); g_dirty = true;
            }
        } else if (g_selectedType == ENTITY_LIGHT && g_selectedIdx >= 0 &&
                   g_selectedIdx < g_scene.lightCount) {
            BrushSceneLight *l = &g_scene.lights[g_selectedIdx];
            ImGui::SeparatorText(TextFormat("Point Light %d", g_selectedIdx));
            float pos[3] = { l->light.position.x, l->light.position.y, l->light.position.z };
            float col[3] = { l->light.color.x, l->light.color.y, l->light.color.z };
            if (ImGui::DragFloat3("Position", pos, 0.1f)) {
                l->light.position = (Vector3){ pos[0], pos[1], pos[2] }; g_dirty = true;
            }
            if (ImGui::ColorEdit3("Color (HDR)", col,
                                  ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                { l->light.color = (Vector3){ col[0], col[1], col[2] }; g_dirty = true; }
            if (ImGui::DragFloat("Radius", &l->light.radius, 0.1f, 0.1f, 100.0f, "%.1f m"))
                g_dirty = true;
            if (ImGui::Checkbox("Flicker", &l->flicker)) g_dirty = true;
        }
        ImGui::End();

        // === Environment =====================================================
        ImGui::Begin("Environment");
        ImGui::SeparatorText("Time of Day");
        if (ImGui::SliderFloat("Hour", &g_tod.timeHours, 0.0f, 24.0f, "%.2f")) g_dirty = true;
        ImGui::DragFloat("Speed", &g_tod.timeScale, 0.01f, -10.0f, 10.0f);
        ImGui::SeparatorText("Post Processing");
        if (pp) {
            ImGui::Checkbox("SSAO", &pp->ssaoEnabled);
            ImGui::Checkbox("Depth of Field", &pp->dofEnabled);
            ImGui::Checkbox("God Rays", &pp->godRaysEnabled);
            ImGui::Checkbox("Volumetric Fog", &pp->volFogEnabled);
            ImGui::Checkbox("SMAA", &pp->smaaEnabled);
            ImGui::Spacing();
            ImGui::DragFloat("Exposure", &pp->exposure, 0.05f, 0.1f, 5.0f);
            ImGui::DragFloat("Bloom", &pp->bloomIntensity, 0.05f, 0.0f, 10.0f);
            ImGui::DragFloat("Bloom Threshold", &pp->bloomThreshold, 0.05f, 0.0f, 5.0f);
        } else {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "HDR pipeline offline");
        }
        ImGui::End();

        // === Console =========================================================
        ImGui::Begin("Console");
        if (ImGui::SmallButton("Clear")) g_logLineCount = 0;
        ImGui::Separator();
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None);
        for (int i = 0; i < g_logLineCount; i++) ImGui::TextUnformatted(g_logLines[i]);
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();

        // === Viewport ========================================================
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Viewport");
        viewportHovered = ImGui::IsWindowHovered();

        ImVec2 panelPos = ImGui::GetCursorScreenPos();
        ImVec2 panelSize = ImGui::GetContentRegionAvail();
        if (pp && panelSize.x > 8 && panelSize.y > 8) {
            // Letterbox: the scene renders at a fixed aspect — fit it into the
            // panel WITHOUT stretching so the gizmo, picking rays, and image
            // all share one geometry. (A stretched image can never align with
            // a gizmo drawn at a different aspect.)
            float renderAspect = (float)pp->outW / (float)pp->outH;
            ImVec2 imgSize = panelSize;
            if (panelSize.x / panelSize.y > renderAspect)
                imgSize.x = panelSize.y * renderAspect;
            else
                imgSize.y = panelSize.x / renderAspect;
            ImVec2 imgPos = ImVec2(panelPos.x + (panelSize.x - imgSize.x) * 0.5f,
                                   panelPos.y + (panelSize.y - imgSize.y) * 0.5f);
            ImGui::SetCursorScreenPos(imgPos);

            Texture2D finalTex = BrushPostGetFinalTexture(pp);
            ImGui::Image((ImTextureID)(intptr_t)finalTex.id, imgSize,
                         ImVec2(0, 1), ImVec2(1, 0)); // render textures are Y-flipped

            // Map a panel mouse position into a full-window ray through the image.
            auto mouseToRay = [&](Vector2 mouse, Ray *out) -> bool {
                float nx = (mouse.x - imgPos.x) / imgSize.x;
                float ny = (mouse.y - imgPos.y) / imgSize.y;
                if (nx < 0 || nx > 1 || ny < 0 || ny > 1) return false;
                Vector2 screen = { nx * (float)GetScreenWidth(),
                                   ny * (float)GetScreenHeight() };
                *out = GetMouseRay(screen, g_camera.cam);
                return true;
            };

            // --- Gizmo (select mode only) -------------------------------------
            if (g_mode == MODE_SELECT && g_selectedType != ENTITY_NONE) {
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(imgPos.x, imgPos.y, imgSize.x, imgSize.y);

                Matrix viewMat = GetCameraMatrix(g_camera.cam);
                Matrix projMat = MatrixPerspective(g_camera.cam.fovy * DEG2RAD,
                                                   renderAspect, 0.01f, 1000.0f);
                Matrix modelMat = MatrixIdentity();
                if (g_selectedType == ENTITY_BLOCK)
                    modelMat = BrushBlockGetModelMatrix(&g_scene.blocks[g_selectedIdx]);
                else if (g_selectedType == ENTITY_LIGHT)
                    modelMat = MatrixTranslate(g_scene.lights[g_selectedIdx].light.position.x,
                                               g_scene.lights[g_selectedIdx].light.position.y,
                                               g_scene.lights[g_selectedIdx].light.position.z);
                else if (g_selectedType == ENTITY_SPAWN)
                    modelMat = MatrixTranslate(g_scene.spawn.x, g_scene.spawn.y, g_scene.spawn.z);

                Matrix viewT = MatrixTranspose(viewMat);
                Matrix projT = MatrixTranspose(projMat);
                Matrix modelT = MatrixTranspose(modelMat);

                ImGuizmo::Manipulate((float *)&viewT, (float *)&projT,
                                     g_selectedType == ENTITY_BLOCK ? g_gizmoOp
                                                                    : ImGuizmo::TRANSLATE,
                                     ImGuizmo::WORLD, (float *)&modelT);

                // Collider policy during drags: the selected block's body is
                // removed ONCE at drag start (so snap rays see through it) and
                // everything rebuilds ONCE at drag end. Rebuilding per frame
                // churned hundreds of Jolt bodies per second.
                static bool wasUsingGizmo = false;
                bool usingGizmo = ImGuizmo::IsUsing();
                if (usingGizmo && !wasUsingGizmo && g_selectedType == ENTITY_BLOCK &&
                    g_blockBodies[g_selectedIdx] != BRUSH_BODY_INVALID) {
                    BrushPhysicsRemoveBody(&g_phys, g_blockBodies[g_selectedIdx]);
                    g_blockBodies[g_selectedIdx] = BRUSH_BODY_INVALID;
                }
                if (!usingGizmo && wasUsingGizmo) RebuildAllColliders();
                wasUsingGizmo = usingGizmo;

                if (usingGizmo) {
                    float trs[3], rot[3], scl[3];
                    ImGuizmo::DecomposeMatrixToComponents((float *)&modelT, trs, rot, scl);
                    Vector3 newPos = { trs[0], trs[1], trs[2] };
                    Vector3 newRot = { rot[0], rot[1], rot[2] };
                    Vector3 newScale = { scl[0], scl[1], scl[2] };

                    // Surface snap: translate mode drops the entity onto the
                    // surface under the cursor (selected body already removed).
                    if (g_surfaceSnap && g_gizmoOp == ImGuizmo::TRANSLATE) {
                        Ray ray;
                        Vector3 hitPt, hitNorm;
                        if (mouseToRay(GetMousePosition(), &ray) &&
                            BrushPhysicsRaycastEx(&g_phys, ray.position, ray.direction,
                                                  1000.0f, &hitPt, &hitNorm, NULL)) {
                            if (g_selectedType == ENTITY_BLOCK) {
                                BrushSceneBlock *b = &g_scene.blocks[g_selectedIdx];
                                float half = (fabsf(hitNorm.x) * b->size.x +
                                              fabsf(hitNorm.y) * b->size.y +
                                              fabsf(hitNorm.z) * b->size.z) * 0.5f;
                                newPos = Vector3Add(hitPt, Vector3Scale(hitNorm, half));
                            } else if (g_selectedType == ENTITY_LIGHT) {
                                newPos = Vector3Add(hitPt, Vector3Scale(hitNorm, 0.2f));
                            } else {
                                newPos = hitPt;
                            }
                        }
                    }

                    if (g_selectedType == ENTITY_BLOCK) {
                        BrushSceneBlock *b = &g_scene.blocks[g_selectedIdx];
                        b->pos = newPos;
                        if (g_gizmoOp == ImGuizmo::ROTATE) b->rot = newRot;
                        if (g_gizmoOp == ImGuizmo::SCALE) b->size = newScale;
                    } else if (g_selectedType == ENTITY_LIGHT) {
                        g_scene.lights[g_selectedIdx].light.position = newPos;
                    } else if (g_selectedType == ENTITY_SPAWN) {
                        g_scene.spawn = newPos;
                    }
                    g_dirty = true;
                }
            }

            // --- Sculpting ------------------------------------------------------
            g_cursorOnGround = false;
            if (g_mode == MODE_SCULPT && viewportHovered) {
                Ray ray;
                if (mouseToRay(GetMousePosition(), &ray)) {
                    Vector3 hitPt, hitNorm;
                    if (BrushPhysicsRaycastEx(&g_phys, ray.position, ray.direction,
                                              2000.0f, &hitPt, &hitNorm, NULL)) {
                        g_cursorOnGround = true;
                        g_cursorPos = hitPt;
                    }
                }
                if (g_cursorOnGround) {
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        PushSculptUndo();     // stroke-level undo record
                        g_sculptStroking = true;
                        g_flattenY = g_cursorPos.y; // flatten to press height
                    }
                    if (g_sculptStroking && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                        float dtStroke = GetFrameTime();
                        bool lower = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                        float strength;
                        if (g_sculptOp == BRUSH_SCULPT_ADD)
                            strength = (lower ? -1.0f : 1.0f) * g_brushStrength * 6.0f * dtStroke;
                        else
                            strength = fminf(g_brushStrength * 8.0f * dtStroke, 1.0f);
                        BrushWorldSculpt(g_world, g_sculptOp, g_cursorPos,
                                         g_brushRadius, strength, g_flattenY);
                        g_dirty = true;
                    }
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) g_sculptStroking = false;
                // Radius on [ ] keys (scroll stays camera navigation).
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
                    g_brushRadius = fmaxf(g_brushRadius - 1.0f, 1.0f);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
                    g_brushRadius = fminf(g_brushRadius + 1.0f, 60.0f);
            }

            // --- Picking (select mode only) -------------------------------------
            if (g_mode == MODE_SELECT && viewportHovered &&
                IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
                Ray ray;
                if (mouseToRay(GetMousePosition(), &ray)) {
                    float closest = 1e9f;
                    EntityType hitType = ENTITY_NONE;
                    int hitIdx = -1;

                    JPH_BodyID hitBody = BRUSH_BODY_INVALID;
                    Vector3 hitPt;
                    if (BrushPhysicsRaycastEx(&g_phys, ray.position, ray.direction,
                                              1000.0f, &hitPt, NULL, &hitBody) &&
                        hitBody != BRUSH_BODY_INVALID) {
                        for (int i = 0; i < g_blockBodyCount; i++) {
                            if (g_blockBodies[i] == hitBody) {
                                closest = Vector3Distance(ray.position, hitPt);
                                hitType = ENTITY_BLOCK;
                                hitIdx = i;
                                break;
                            }
                        }
                    }
                    for (int i = 0; i < g_scene.lightCount; i++) {
                        Vector3 p = g_scene.lights[i].light.position;
                        BoundingBox box = { { p.x - 0.2f, p.y - 0.2f, p.z - 0.2f },
                                            { p.x + 0.2f, p.y + 0.2f, p.z + 0.2f } };
                        RayCollision col = GetRayCollisionBox(ray, box);
                        if (col.hit && col.distance < closest) {
                            closest = col.distance; hitType = ENTITY_LIGHT; hitIdx = i;
                        }
                    }
                    BoundingBox spawnBox = {
                        { g_scene.spawn.x - 0.4f, g_scene.spawn.y, g_scene.spawn.z - 0.4f },
                        { g_scene.spawn.x + 0.4f, g_scene.spawn.y + 1.8f, g_scene.spawn.z + 0.4f }
                    };
                    RayCollision sc = GetRayCollisionBox(ray, spawnBox);
                    if (sc.hit && sc.distance < closest) {
                        closest = sc.distance; hitType = ENTITY_SPAWN; hitIdx = 0;
                    }

                    g_selectedType = hitType;
                    g_selectedIdx = hitIdx;
                }
            }

            // --- Floating toolbar (top-left of the image) ----------------------
            ImGui::SetCursorScreenPos(ImVec2(imgPos.x + 10, imgPos.y + 10));
            ImGui::BeginGroup();
            if (ToolButton(" Select ", g_mode == MODE_SELECT)) g_mode = MODE_SELECT;
            ImGui::SameLine();
            if (ToolButton(" Sculpt ", g_mode == MODE_SCULPT)) g_mode = MODE_SCULPT;
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (g_mode == MODE_SELECT) {
                if (ToolButton(" Move (W) ", g_gizmoOp == ImGuizmo::TRANSLATE))
                    g_gizmoOp = ImGuizmo::TRANSLATE;
                ImGui::SameLine();
                if (ToolButton(" Rotate (E) ", g_gizmoOp == ImGuizmo::ROTATE))
                    g_gizmoOp = ImGuizmo::ROTATE;
                ImGui::SameLine();
                if (ToolButton(" Scale (R) ", g_gizmoOp == ImGuizmo::SCALE))
                    g_gizmoOp = ImGuizmo::SCALE;
                ImGui::SameLine();
                ImGui::Checkbox("Snap", &g_surfaceSnap);
            } else {
                if (ToolButton(" Raise ", g_sculptOp == BRUSH_SCULPT_ADD))
                    g_sculptOp = BRUSH_SCULPT_ADD;
                ImGui::SameLine();
                if (ToolButton(" Smooth ", g_sculptOp == BRUSH_SCULPT_SMOOTH))
                    g_sculptOp = BRUSH_SCULPT_SMOOTH;
                ImGui::SameLine();
                if (ToolButton(" Flatten ", g_sculptOp == BRUSH_SCULPT_FLATTEN))
                    g_sculptOp = BRUSH_SCULPT_FLATTEN;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                ImGui::SliderFloat("Radius", &g_brushRadius, 1.0f, 60.0f, "%.0f m");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                ImGui::SliderFloat("Strength", &g_brushStrength, 0.1f, 4.0f, "%.1f");
            }
            ImGui::EndGroup();

            // Play/Stop + save state, top-right of the viewport.
            {
                float clusterW = ImGui::CalcTextSize("  Stop  ").x +
                                 ImGui::CalcTextSize("* unsaved").x +
                                 ImGui::GetStyle().ItemSpacing.x * 2 +
                                 ImGui::GetStyle().FramePadding.x * 4;
                ImGui::SetCursorScreenPos(
                    ImVec2(imgPos.x + imgSize.x - clusterW - 10, imgPos.y + 10));
                ImGui::BeginGroup();
                bool running = GameRunning();
                if (running) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
                    if (ImGui::Button("  Stop  ")) StopGame();
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
                    if (ImGui::Button("  Play  ")) PlayGame();
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();
                if (g_dirty)
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1), "* unsaved");
                else
                    ImGui::TextDisabled("saved");
                ImGui::EndGroup();
            }

            // --- Nav hint bar (bottom of the image) ----------------------------
            const char *hints =
                (g_mode == MODE_SCULPT)
                    ? "drag sculpt   |   shift+drag lower   |   [ ] radius   |   "
                      "scroll orbit   |   cmd+Z undo   |   1 select  2 sculpt"
                    : "scroll orbit   |   shift+scroll pan   |   cmd+scroll zoom   |   "
                      "right-drag fly (WASD)   |   F frame   |   B block   L light";
            ImVec2 hintSize = ImGui::CalcTextSize(hints);
            ImGui::SetCursorScreenPos(ImVec2(imgPos.x + (imgSize.x - hintSize.x) * 0.5f,
                                             imgPos.y + imgSize.y - hintSize.y - 8));
            ImGui::TextColored(ImVec4(1, 1, 1, 0.45f), "%s", hints);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // === Shortcuts =======================================================
        ImGuiIO &io = ImGui::GetIO();
        bool typing = io.WantTextInput;
        bool cmd = io.KeySuper || io.KeyCtrl;

        if (!typing) {
            if (ImGui::IsKeyPressed(ImGuiKey_F5)) { GameRunning() ? StopGame() : PlayGame(); }
            if (cmd && ImGui::IsKeyPressed(ImGuiKey_S)) SaveScene();
            if (cmd && ImGui::IsKeyPressed(ImGuiKey_Z)) PopSculptUndo();
            if (cmd && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
            if (cmd && ImGui::IsKeyPressed(ImGuiKey_Q)) g_quit = true;

            if (viewportHovered) {
                // Gizmo modes only when not flying (W is fly-forward there).
                if (!isFlying) {
                    if (ImGui::IsKeyPressed(ImGuiKey_W)) g_gizmoOp = ImGuizmo::TRANSLATE;
                    if (ImGui::IsKeyPressed(ImGuiKey_E)) g_gizmoOp = ImGuizmo::ROTATE;
                    if (ImGui::IsKeyPressed(ImGuiKey_R)) g_gizmoOp = ImGuizmo::SCALE;
                    if (ImGui::IsKeyPressed(ImGuiKey_B)) AddBlockEntity();
                    if (ImGui::IsKeyPressed(ImGuiKey_L)) AddLightEntity();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_1)) g_mode = MODE_SELECT;
                if (ImGui::IsKeyPressed(ImGuiKey_2)) g_mode = MODE_SCULPT;
                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    if (io.KeyShift) { g_selectedType = ENTITY_NONE; g_selectedIdx = -1; }
                    FrameSelected();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                    ImGui::IsKeyPressed(ImGuiKey_Backspace))
                    DeleteSelected();
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    g_selectedType = ENTITY_NONE; g_selectedIdx = -1;
                }
            }
        }

        rlImGuiEnd();
        EndDrawing();

        // Screenshot harness (same envs as the game): automated visual checks.
        static int frameCount = 0;
        frameCount++;
        if (getenv("BRUSH_AUTO_SCREENSHOT") != NULL) {
            const char *sf = getenv("BRUSH_AUTO_FRAMES");
            int shotFrame = sf ? atoi(sf) : 180;
            if (frameCount == (shotFrame > 0 ? shotFrame : 180)) {
                TakeScreenshot("screenshot.png");
                g_quit = true;
            }
        }
    }

    StopGame();
    rlImGuiShutdown();
    UnloadTexture(g_groundTex);
    UnloadModel(g_unitCube);
    UnloadModel(g_spawnMarker);
    BrushWorldDestroy(g_world);
    BrushPhysicsCleanup(&g_phys);
    BrushRenderShutdown();
    CloseWindow();
    return 0;
}
