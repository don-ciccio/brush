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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#if defined(__APPLE__)
// macos_window.mm — Godot-style seamless titlebar (content under the
// titlebar, traffic lights floating over the menu bar row).
extern "C" void EditorMacSeamlessTitlebar(void *nsWindow);
extern "C" void EditorMacDragWindow(void *nsWindow);
extern "C" void EditorMacInstallMenu(void);
extern "C" int EditorMacOpenImageDialog(char *out, int cap);
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
enum EntityType { ENTITY_NONE, ENTITY_BLOCK, ENTITY_LIGHT, ENTITY_SPAWN, ENTITY_MODEL, ENTITY_ROAD };

static BrushPhysics g_phys;
static BrushScene g_scene;
static BrushWorld *g_world = NULL;
static BrushFoliage *g_foliage = NULL;
static Texture2D g_groundTex;
static Model g_unitCube;
static Model g_spawnMarker;

static JPH_BodyID g_blockBodies[BRUSH_SCENE_MAX_BLOCKS];
// One placed model = up to a few trimesh bodies (one per mesh). Kept per
// instance so picking maps hits back and gizmo drags can remove just the
// selected instance's bodies.
#define MODEL_BODIES_PER_INSTANCE 4
static JPH_BodyID g_modelBodies[BRUSH_SCENE_MAX_MODELS][MODEL_BODIES_PER_INSTANCE];
static int g_modelBodyN[BRUSH_SCENE_MAX_MODELS];
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
static bool g_paintActive = false; // sculpt-mode Paint tool (terrain layers)
static int g_paintLayer = 1;       // painted layer (0 is the base coat)
static bool g_roadActive = false;  // sculpt-mode Road spline tool
static int g_selectedRoadIdx = -1;
static int g_selectedNodeIdx = -1;
static bool g_roadResyncPending = false; // a road changed; re-bake when idle
static bool g_foliageResyncPending = false; // placement changed; rebuild + re-scatter
static bool g_foliageRebuildPending = false; // draw-only change; rebuild layers, no rebake
static int g_selectedFoliage = -1;          // active foliage layer in the panel

static bool g_useSlopeFilter = false;
static float g_constrainSlopeDegMin = 0.0f;
static float g_constrainSlopeDegMax = 30.0f;
static bool g_useHeightFilter = false;
static float g_constrainHeightMin = -10.0f;
static float g_constrainHeightMax = 100.0f;
static int g_constrainLayerIdx = -1; // -1 = any

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

// Which terrain-layer slot (0..3) a road's material occupies, or -1 if the
// material isn't assigned to a slot (road then carves shape only, no paint).
static int RoadLayerSlot(const char *material) {
    if (!material || !material[0]) return -1;
    for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++)
        if (strcmp(g_scene.terrainLayers[i], material) == 0) return i;
    return -1;
}

// Initialise a fresh road with sane defaults. Material defaults to the first
// configured terrain layer (so it paints out of the box); "" until one is set.
static void NewRoad(BrushSceneRoad *r) {
    memset(r, 0, sizeof(*r));
    r->width = 8.0f;
    r->fade = 4.0f;
    r->paintFade = 4.0f; // texture feathers by default; set 0 for paving
    for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++)
        if (g_scene.terrainLayers[i][0]) {
            strncpy(r->material, g_scene.terrainLayers[i], sizeof(r->material) - 1);
            break;
        }
}

// Push the scene's roads into the live world (resolving each material to a
// splat slot). Call after loading a scene and whenever a road changes — the
// world re-bakes the affected chunks, so edits update the terrain live.
static void SyncRoadsToWorld() {
    if (!g_world) return;
    BrushWorldRoad wr[BRUSH_WORLD_MAX_ROADS];
    int n = 0;
    for (int i = 0; i < g_scene.roadCount && n < BRUSH_WORLD_MAX_ROADS; i++) {
        const BrushSceneRoad *r = &g_scene.roads[i];
        if (r->pointCount < 2) continue; // half-drawn road: nothing to bake yet
        BrushWorldRoad *o = &wr[n++];
        int pc = r->pointCount > BRUSH_ROAD_MAX_POINTS ? BRUSH_ROAD_MAX_POINTS
                                                       : r->pointCount;
        for (int k = 0; k < pc; k++) o->points[k] = r->points[k];
        o->pointCount = pc;
        o->width = r->width;
        o->fade = r->fade;
        o->paintFade = r->paintFade;
        o->layerSlot = RoadLayerSlot(r->material); // -1 = shape only
    }
    BrushWorldSetRoads(g_world, wr, n);
}

// (Re)build the foliage system's layers from the scene's foliage entries. The
// scene stays foliage-system-agnostic, so translate here (as the sandbox does).
static void BuildFoliageLayers() {
    if (!g_foliage) return;
    BrushFoliageClearLayers(g_foliage);
    for (int i = 0; i < g_scene.foliageCount; i++) {
        const BrushSceneFoliageLayer *fl = &g_scene.foliage[i];
        BrushFoliageLayerConfig c = {};
        c.density = fl->density;
        c.drawDistance = fl->drawDistance;
        c.lodDistance = fl->lodDistance;
        c.scale = fl->scale;
        c.scaleJitter = fl->scaleJitter;
        c.heightOffset = fl->heightOffset;
        c.maxSlopeDeg = fl->maxSlopeDeg;
        c.windStrength = fl->windStrength;
        c.farKeepRatio = fl->farKeepRatio;
        c.tint = fl->tint;
        c.macroLow = fl->macroLow;
        c.macroHigh = fl->macroHigh;
        c.albedo = fl->albedoTex;
        c.growLayer = -1;
        c.avoidLayer = -1;
        if (fl->modelRes.meshCount > 0) c.mesh = fl->modelRes.meshes[0];
        BrushFoliageAddLayer(g_foliage, &c);
    }
}

// Re-resolve assets, rebuild the layers, and re-scatter every chunk. Called
// (deferred to idle) whenever a foliage layer edit changes placement/mesh.
static void SyncFoliageToWorld() {
    if (!g_world || !g_foliage) return;
    BrushSceneResolveMaterials(&g_scene); // pick up model/albedo path changes
    BuildFoliageLayers();
    BrushWorldRebakeAll(g_world);
}

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
    BrushSceneCaptureRenderSettings(&g_scene); // scene carries its look
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
    // Player binary lives in the ENGINE tree; the child inherits our cwd,
    // which is the open project's root — so it runs THIS project.
    static char playerPath[600];
    snprintf(playerPath, sizeof(playerPath), "%s",
             BrushEnginePath("build/sandbox"));
    g_gamePid = fork();
    if (g_gamePid == 0) {
        char *args[] = { playerPath, NULL };
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

// --- Texture browser (material path pickers) ----------------------------------
#define MAX_TEX_FILES 128
static char g_texFiles[MAX_TEX_FILES][128];
static int g_texFileCount = 0;

static void RescanTextureDir() {
    g_texFileCount = 0;
    FilePathList fl = LoadDirectoryFiles("assets/textures");
    for (unsigned int i = 0; i < fl.count && g_texFileCount < MAX_TEX_FILES; i++) {
        if (!IsFileExtension(fl.paths[i], ".png") &&
            !IsFileExtension(fl.paths[i], ".jpg")) continue;
        snprintf(g_texFiles[g_texFileCount++], sizeof(g_texFiles[0]), "%s",
                 fl.paths[i]);
    }
    UnloadDirectoryFiles(fl);
    // Alphabetical, so *_color / *_normal pairs sit together.
    qsort(g_texFiles, (size_t)g_texFileCount, sizeof(g_texFiles[0]),
          [](const void *a, const void *b) {
              return strcmp((const char *)a, (const char *)b);
          });
}

// Combo over assets/textures. Writes the project-relative path into `path`;
// returns true when it changed (caller re-resolves the scene materials).
static bool TexPathCombo(const char *label, char *path, int cap) {
    bool changed = false;
    char preview[128];
    snprintf(preview, sizeof(preview), "%s",
             path[0] ? GetFileName(path) : "(none)");
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable("(none)", path[0] == '\0') && path[0] != '\0') {
            path[0] = '\0';
            changed = true;
        }
        for (int i = 0; i < g_texFileCount; i++) {
            bool sel = (strcmp(path, g_texFiles[i]) == 0);
            if (ImGui::Selectable(GetFileName(g_texFiles[i]), sel) && !sel) {
                snprintf(path, (size_t)cap, "%s", g_texFiles[i]);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// --- Assets panel file list (everything under assets/) -----------------------
#define MAX_ASSET_FILES 512
static char g_assetFiles[MAX_ASSET_FILES][192];
static int g_assetFileCount = 0;

static Texture2D g_assetPreview = { 0 };
static char g_assetPreviewPath[192] = "";

static void RescanAssetFiles() {
    g_assetFileCount = 0;
    FilePathList fl = LoadDirectoryFilesEx("assets", NULL, true);
    for (unsigned int i = 0; i < fl.count && g_assetFileCount < MAX_ASSET_FILES; i++) {
        const char *name = GetFileName(fl.paths[i]);
        if (name == NULL || name[0] == '.') continue;      // .DS_Store etc.
        if (IsFileExtension(fl.paths[i], ".import")) continue; // sidecars ride along
        snprintf(g_assetFiles[g_assetFileCount++], sizeof(g_assetFiles[0]),
                 "%s", fl.paths[i]);
    }
    UnloadDirectoryFiles(fl);
    qsort(g_assetFiles, (size_t)g_assetFileCount, sizeof(g_assetFiles[0]),
          [](const void *a, const void *b) {
              return strcmp((const char *)a, (const char *)b);
          });
}

static bool IsImageAsset(const char *path) {
    return IsFileExtension(path, ".png") || IsFileExtension(path, ".jpg") ||
           IsFileExtension(path, ".jpeg") || IsFileExtension(path, ".tga") ||
           IsFileExtension(path, ".bmp");
}

static bool IsModelAsset(const char *path) {
    return IsFileExtension(path, ".glb") || IsFileExtension(path, ".gltf") ||
           IsFileExtension(path, ".obj");
}

static void DrawFolderContents(const char* parentFolder, int& selAsset, BrushTexImportParams& impParams) {
    char parentPath[256];
    if (parentFolder[0] == '\0') {
        snprintf(parentPath, sizeof(parentPath), "assets/");
    } else {
        snprintf(parentPath, sizeof(parentPath), "assets/%s/", parentFolder);
    }
    int parentLen = strlen(parentPath);
    char lastSubfolder[128] = "";

    for (int i = 0; i < g_assetFileCount; i++) {
        const char* path = g_assetFiles[i];
        if (strncmp(path, parentPath, parentLen) != 0) continue;

        const char* sub = path + parentLen;
        const char* slash = strchr(sub, '/');

        if (slash != NULL) {
            char subfolderName[128];
            int subfolderLen = slash - sub;
            if (subfolderLen >= (int)sizeof(subfolderName)) subfolderLen = sizeof(subfolderName) - 1;
            strncpy(subfolderName, sub, subfolderLen);
            subfolderName[subfolderLen] = '\0';

            if (strcmp(subfolderName, lastSubfolder) == 0) continue;
            snprintf(lastSubfolder, sizeof(lastSubfolder), "%s", subfolderName);

            char nextFolder[256];
            if (parentFolder[0] == '\0') {
                snprintf(nextFolder, sizeof(nextFolder), "%s", subfolderName);
            } else {
                snprintf(nextFolder, sizeof(nextFolder), "%s/%s", parentFolder, subfolderName);
            }

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
            ImGui::PushID(subfolderName);
            if (ImGui::TreeNodeEx(subfolderName, flags)) {
                DrawFolderContents(nextFolder, selAsset, impParams);
                ImGui::TreePop();
            }
            ImGui::PopID();
        } else {
            bool isSelected = (selAsset == i);
            if (ImGui::Selectable(sub, isSelected)) {
                selAsset = i;
                if (g_assetPreviewPath[0] != '\0') {
                    BrushAssetsReleaseTexture(g_assetPreview);
                    g_assetPreview = (Texture2D){ 0 };
                    g_assetPreviewPath[0] = '\0';
                }
                if (IsImageAsset(g_assetFiles[i])) {
                    g_assetPreview = BrushAssetsTexture(g_assetFiles[i]);
                    snprintf(g_assetPreviewPath, sizeof(g_assetPreviewPath),
                             "%s", g_assetFiles[i]);
                    BrushAssetsGetImportParams(g_assetFiles[i], &impParams);
                }
            }
            // Model files drag into the viewport to place an instance.
            if (IsModelAsset(path) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("BRUSH_MODEL", path,
                                          strlen(path) + 1);
                ImGui::TextUnformatted(GetFileName(path));
                ImGui::EndDragDropSource();
            }
        }
    }
}

// Copy image files into the project's assets/textures (creating it on
// demand — the Empty template starts without one) and rescan the pickers.
// This is how textures enter a project: drag & drop onto the editor, or
// the Import button's native dialog.
static int ImportTextureFiles(const char **paths, int count) {
    int imported = 0;
    for (int i = 0; i < count; i++) {
        bool isImage = IsImageAsset(paths[i]);
        bool isModel = IsModelAsset(paths[i]);
        if (!isImage && !isModel) {
            AddEditorLog("Skipped (not an image or model): %s",
                         GetFileName(paths[i]));
            continue;
        }
        mkdir("assets", 0755);
        mkdir(isModel ? "assets/models" : "assets/textures", 0755);
        char dst[512];
        snprintf(dst, sizeof(dst), "%s/%s",
                 isModel ? "assets/models" : "assets/textures",
                 GetFileName(paths[i]));
        int size = 0;
        unsigned char *data = LoadFileData(paths[i], &size);
        if (data == NULL) {
            AddEditorLog("ERROR: cannot read %s", paths[i]);
            continue;
        }
        bool ok = SaveFileData(dst, data, size);
        UnloadFileData(data);
        if (ok) {
            imported++;
            AddEditorLog("Imported %s", dst);
        }
    }
    if (imported > 0) {
        RescanTextureDir();
        RescanAssetFiles();
    }
    return imported;
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
    for (int i = 0; i < BRUSH_SCENE_MAX_MODELS; i++) {
        for (int j = 0; j < g_modelBodyN[i]; j++)
            BrushPhysicsRemoveBody(&g_phys, g_modelBodies[i][j]);
        g_modelBodyN[i] = 0;
    }
    for (int i = 0; i < g_scene.modelCount; i++) {
        BrushSceneModelInstance *mi = &g_scene.models[i];
        if (mi->model.meshCount == 0) continue;
        // Shared cooked shape per mesh, placed at this instance's transform.
        int n = 0;
        for (int m = 0; m < mi->model.meshCount && n < MODEL_BODIES_PER_INSTANCE; m++) {
            JPH_Shape *base = BrushAssetsModelShape(mi->path, m);
            if (base == NULL) continue;
            JPH_BodyID id = BrushPhysicsAddStaticShapeAt(
                &g_phys, base, mi->pos, mi->rot, mi->scale, i, "model");
            if (id != BRUSH_BODY_INVALID) g_modelBodies[i][n++] = id;
        }
        g_modelBodyN[i] = n;
    }
}

// "0: grass" / "2: (empty)" — layer slots are meaningless without their
// material names in front of the user.
static const char *TerrainSlotLabel(int slot) {
    return TextFormat("%d: %s", slot,
                      g_scene.terrainLayers[slot][0]
                          ? g_scene.terrainLayers[slot]
                          : "(empty)");
}

// Combo over the layer slots for the auto-mask pickers: shows material
// names, greys out empty slots (an unset slot can never render).
static bool TerrainSlotCombo(const char *label, int *slot) {
    bool changed = false;
    const char *preview = (*slot < 0) ? "Off" : TerrainSlotLabel(*slot);
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable("Off", *slot < 0) && *slot >= 0) {
            *slot = -1;
            changed = true;
        }
        for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++) {
            bool empty = (g_scene.terrainLayers[i][0] == '\0');
            if (ImGui::Selectable(TerrainSlotLabel(i), *slot == i,
                                  empty ? ImGuiSelectableFlags_Disabled : 0) &&
                *slot != i) {
                *slot = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Push the scene's terrain_layer slots into the world (load/reload/re-import).
static void ApplyTerrainLayers() {
    if (g_world == NULL) return;
    BrushTerrainLayer layers[BRUSH_TERRAIN_LAYERS];
    int n = BrushSceneTerrainLayers(&g_scene, layers);
    BrushWorldSetLayers(g_world, layers, n);
    BrushWorldSetAutoSlope(g_world, g_scene.autoSlopeLayer,
                           g_scene.autoSlopeStart, g_scene.autoSlopeEnd);
    BrushWorldSetLayerHeights(g_world, g_scene.layerHeightOn,
                              g_scene.layerHeightStart, g_scene.layerHeightFull);
}

static void ReloadScene() {
    if (BrushSceneLoad(&g_scene, g_scenePath)) {
        g_dirty = false;
        g_tod.timeHours = g_scene.timeHours >= 0.0f ? g_scene.timeHours : 12.0f;
        RebuildAllColliders();
        BrushSceneApplyRenderSettings(&g_scene);
        ApplyTerrainLayers();
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

// Place a model instance (Assets-panel drag or OS drop). Resolves through
// the registry immediately so it draws this frame.
static void AddModelEntityAt(const char *path, Vector3 pos) {
    if (g_scene.modelCount >= BRUSH_SCENE_MAX_MODELS) return;
    BrushSceneModelInstance *mi = &g_scene.models[g_scene.modelCount];
    memset(mi, 0, sizeof(*mi));
    snprintf(mi->path, sizeof(mi->path), "%s", path);
    mi->pos = pos;
    mi->scale = (Vector3){ 1, 1, 1 };
    mi->model = BrushAssetsModel(mi->path);
    g_selectedType = ENTITY_MODEL;
    g_selectedIdx = g_scene.modelCount++;
    RebuildAllColliders();
    g_dirty = true;
    AddEditorLog("Placed %s", GetFileName(path));
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
    } else if (g_selectedType == ENTITY_MODEL && g_selectedIdx >= 0) {
        BrushAssetsReleaseModel(g_scene.models[g_selectedIdx].path);
        for (int i = g_selectedIdx; i < g_scene.modelCount - 1; i++)
            g_scene.models[i] = g_scene.models[i + 1];
        g_scene.modelCount--;
        RebuildAllColliders();
        AddEditorLog("Deleted model");
    } else if (g_selectedType == ENTITY_ROAD && g_selectedIdx >= 0) {
        for (int i = g_selectedIdx; i < g_scene.roadCount - 1; i++)
            g_scene.roads[i] = g_scene.roads[i + 1];
        g_scene.roadCount--;
        g_selectedRoadIdx = -1;
        g_selectedNodeIdx = -1;
        g_roadResyncPending = true; // rebake to erase the deleted road
        AddEditorLog("Deleted road");
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
    } else if (g_selectedType == ENTITY_MODEL && g_selectedIdx >= 0 &&
               g_scene.modelCount < BRUSH_SCENE_MAX_MODELS) {
        BrushSceneModelInstance copy = g_scene.models[g_selectedIdx];
        copy.pos.x += 1.5f;
        copy.model = BrushAssetsModel(copy.path); // its own registry ref
        g_scene.models[g_scene.modelCount] = copy;
        g_selectedIdx = g_scene.modelCount++;
        RebuildAllColliders();
        g_dirty = true;
        AddEditorLog("Duplicated model -> %d", g_selectedIdx);
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
    } else if (g_selectedType == ENTITY_MODEL && g_selectedIdx >= 0) {
        g_camera.Frame(g_scene.models[g_selectedIdx].pos, 2.5f);
    } else if (g_selectedType == ENTITY_ROAD && g_selectedIdx >= 0 &&
               g_scene.roads[g_selectedIdx].pointCount > 0) {
        g_camera.Frame(g_scene.roads[g_selectedIdx].points[0], 10.0f);
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
// --- Project management --------------------------------------------------------
// The editor is a two-state app (Godot-style): PROJECT MANAGER -> EDITOR.
// A project is a folder with project.def (see b_project.h); opening one
// chdir()s into it so every project-relative path in the codebase keeps
// working. Engine files resolve via BrushEnginePath. Known projects persist
// in ~/.brush/projects, most recent first.

static BrushProject g_project;
static bool g_projectOpen = false;

#define MAX_RECENT_PROJECTS 16
static char g_recent[MAX_RECENT_PROJECTS][512];
static int g_recentCount = 0;
static BrushProject g_recentProjects[MAX_RECENT_PROJECTS];
static bool g_recentExists[MAX_RECENT_PROJECTS];

static void ResolveRecentProjectMetadata() {
    for (int i = 0; i < g_recentCount; i++) {
        char pd[560];
        snprintf(pd, sizeof(pd), "%s/project.def", g_recent[i]);
        g_recentExists[i] = FileExists(pd);
        if (g_recentExists[i]) {
            BrushProjectLoad(&g_recentProjects[i], g_recent[i]);
        } else {
            memset(&g_recentProjects[i], 0, sizeof(BrushProject));
        }
    }
}

static const char *BrushUserDir() {
    static char dir[512] = "";
    if (dir[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(dir, sizeof(dir), "%s/.brush", home ? home : ".");
        mkdir(dir, 0755); // idempotent
    }
    return dir;
}

static void LoadRecents() {
    char path[560];
    snprintf(path, sizeof(path), "%s/projects", BrushUserDir());
    g_recentCount = 0;
    FILE *f = fopen(path, "r");
    if (!f) return; // clean slate: the first project is created, not seeded
    char line[512];
    while (fgets(line, sizeof(line), f) && g_recentCount < MAX_RECENT_PROJECTS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] != '\0')
            snprintf(g_recent[g_recentCount++], sizeof(g_recent[0]), "%s", line);
    }
    fclose(f);
    ResolveRecentProjectMetadata();
}

static void SaveRecents() {
    char path[560];
    snprintf(path, sizeof(path), "%s/projects", BrushUserDir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_recentCount; i++) fprintf(f, "%s\n", g_recent[i]);
    fclose(f);
}

static void AddRecent(const char *dir) {
    char abs[512];
    if (realpath(dir, abs) == NULL) snprintf(abs, sizeof(abs), "%s", dir);
    for (int i = 0; i < g_recentCount; i++) {
        if (strcmp(g_recent[i], abs) != 0) continue;
        for (int j = i; j > 0; j--) strcpy(g_recent[j], g_recent[j - 1]);
        strcpy(g_recent[0], abs);
        SaveRecents();
        return;
    }
    if (g_recentCount < MAX_RECENT_PROJECTS) g_recentCount++;
    for (int j = g_recentCount - 1; j > 0; j--)
        strcpy(g_recent[j], g_recent[j - 1]);
    snprintf(g_recent[0], sizeof(g_recent[0]), "%s", abs);
    SaveRecents();
    ResolveRecentProjectMetadata();
}

// mkdir -p
static void MakeDirs(const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static bool CopyFileRaw(const char *src, const char *dst) {
    int size = 0;
    unsigned char *data = LoadFileData(src, &size);
    if (data == NULL) return false;
    bool ok = SaveFileData(dst, data, size);
    UnloadFileData(data);
    return ok;
}

// Copy every file under srcDir (recursive) into dstDir, preserving layout.
static void CopyTree(const char *srcDir, const char *dstDir) {
    char src[512];
    snprintf(src, sizeof(src), "%s", srcDir); // srcDir may be in the
                                              // BrushEnginePath ring buffer
    FilePathList fl = LoadDirectoryFilesEx(src, NULL, true);
    for (unsigned int i = 0; i < fl.count; i++) {
        const char *rel = fl.paths[i] + strlen(src);
        while (*rel == '/') rel++;
        char dst[1024];
        snprintf(dst, sizeof(dst), "%s/%s", dstDir, rel);
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s", dst);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; MakeDirs(parent); }
        CopyFileRaw(fl.paths[i], dst);
    }
    UnloadDirectoryFiles(fl);
}

enum { TEMPLATE_GYM = 0, TEMPLATE_EMPTY };

// Create <parent>/<name> from a template. v0 templates source the engine
// repo's own assets (the gym IS the template). Returns the project dir.
static bool CreateProject(const char *parent, const char *name, int tmpl,
                          char *outDir, int outCap) {
    snprintf(outDir, (size_t)outCap, "%s/%s", parent, name);
    char probe[600];
    snprintf(probe, sizeof(probe), "%s/project.def", outDir);
    if (FileExists(probe)) {
        AddEditorLog("ERROR: %s already exists", probe);
        return false;
    }
    MakeDirs(outDir);
    char assets[600];
    // assets/textures exists from birth so texture drops/imports always
    // have a home (the Empty template ships no textures of its own).
    snprintf(assets, sizeof(assets), "%s/assets/textures", outDir);
    MakeDirs(assets);

    BrushProject p = {0};
    snprintf(p.name, sizeof(p.name), "%s", name);
    snprintf(p.scene, sizeof(p.scene),
             tmpl == TEMPLATE_GYM ? "assets/gym.def" : "assets/main.def");
    if (!BrushProjectSave(&p, outDir)) return false;

    // Both templates need the character (the player binary loads it).
    char dst[600];
    snprintf(dst, sizeof(dst), "%s/assets/character", outDir);
    CopyTree(BrushEnginePath("assets/character"), dst);
    if (tmpl == TEMPLATE_GYM) {
        snprintf(dst, sizeof(dst), "%s/assets/textures", outDir);
        CopyTree(BrushEnginePath("assets/textures"), dst);
        snprintf(dst, sizeof(dst), "%s/assets/models", outDir);
        CopyTree(BrushEnginePath("assets/models"), dst);
        snprintf(dst, sizeof(dst), "%s/assets/gym.def", outDir);
        CopyFileRaw(BrushEnginePath("assets/gym.def"), dst);
        snprintf(dst, sizeof(dst), "%s/assets/gym.terrain", outDir);
        CopyFileRaw(BrushEnginePath("assets/gym.terrain"), dst);
    }
    AddEditorLog("Created project %s", outDir);
    return true;
}

// Enter a project and bring up everything that depends on it. The
// project-independent systems (window, renderer, ImGui) are already up.
static bool OpenProjectAt(const char *dir) {
    if (chdir(dir) != 0) {
        AddEditorLog("ERROR: cannot enter %s", dir);
        return false;
    }
    bool engineDev = false;
    if (!BrushProjectLoad(&g_project, ".")) {
        if (FileExists("engine/shaders/lit.fs")) {
            // The ENGINE repo itself (harnesses, engine development): run
            // the gym in place, but never write a project.def into the
            // engine tree or list it as a project — the gym is engine-dev
            // fixture + template payload, not a user project.
            engineDev = true;
            snprintf(g_project.name, sizeof(g_project.name), "Engine Dev");
            snprintf(g_project.scene, sizeof(g_project.scene),
                     "assets/gym.def");
        } else {
            // Opening a bare folder (CLI): adopt it with defaults so the
            // next save makes it a real project.
            const char *base = GetFileName(GetWorkingDirectory());
            if (base && base[0]) snprintf(g_project.name, sizeof(g_project.name), "%s", base);
            BrushProjectSave(&g_project, ".");
            AddEditorLog("Created project.def in %s", dir);
        }
    }
    snprintf(g_scenePath, sizeof(g_scenePath), "%s", g_project.scene);
    snprintf(g_terrainPath, sizeof(g_terrainPath), "%s", g_scenePath);
    char *dot = strrchr(g_terrainPath, '.');
    if (dot != NULL && dot != g_terrainPath) *dot = '\0';
    strncat(g_terrainPath, ".terrain",
            sizeof(g_terrainPath) - strlen(g_terrainPath) - 1);

    BrushPhysicsInit(&g_phys);

    if (!BrushSceneLoad(&g_scene, g_scenePath)) {
        g_scene.spawn = (Vector3){ 0.0f, 0.5f, 8.0f };
        g_scene.timeHours = 12.0f;
        BrushSceneSave(&g_scene, g_scenePath);
        AddEditorLog("Created new scene %s", g_scenePath);
    } else {
        AddEditorLog("Loaded %s (%d blocks, %d lights, %d materials)",
                     g_scenePath, g_scene.blockCount, g_scene.lightCount,
                     g_scene.materialCount);
    }
    BrushSceneApplyRenderSettings(&g_scene);

    BrushTodInit(&g_tod);
    g_tod.timeHours = g_scene.timeHours >= 0.0f ? g_scene.timeHours : 12.0f;

    // Resolve splat layers into the config so the initial ring bakes textured
    // (a post-create SetLayers would dirty every chunk and pop the terrain
    // texture in ~1s later). ApplyTerrainLayers below then only sets the
    // shader-side auto-slope/heights; its SetLayers call is a no-op.
    BrushWorldConfig wcfg = { .seed = 1337, .heightFn = NULL, .chunkSize = 64.0f,
                              .loadRadius = 3, .physics = &g_phys,
                              .groundTex = g_groundTex, .texMetresPerTile = 64.0f };
    wcfg.layerCount = BrushSceneTerrainLayers(&g_scene, wcfg.layers);

    // Live instanced foliage: build from the scene, install worker hooks before
    // create so the initial ring scatters, attach the draw after.
    g_foliage = BrushFoliageCreate();
    if (g_scene.foliageCount == 0) {
        BrushSceneFoliageLayer *fl = &g_scene.foliage[g_scene.foliageCount++];
        memset(fl, 0, sizeof(*fl));
        snprintf(fl->name, sizeof(fl->name), "grass");
        fl->density = 0.5f; fl->drawDistance = 68.0f; fl->lodDistance = 26.0f;
        fl->scale = 1.0f; fl->scaleJitter = 0.35f; fl->heightOffset = -0.04f;
        fl->maxSlopeDeg = 42.0f; fl->windStrength = 1.0f; fl->farKeepRatio = 0.4f;
        fl->tint = (Vector3){1, 1, 1};
    }
    BuildFoliageLayers();
    BrushFoliageInstallHooks(g_foliage, &wcfg);

    g_world = BrushWorldCreate(
        wcfg, (Vector3){ g_scene.spawn.x, 0, g_scene.spawn.z });
    BrushFoliageAttach(g_foliage, g_world);

    BrushWorldSculptLoad(g_world, g_terrainPath);
    ApplyTerrainLayers();
    SyncRoadsToWorld(); // live-bake the scene's roads on load

    // Harness: scripted sculpt + save (headless end-to-end check of the
    // editor -> .terrain -> game pipeline).
    if (getenv("BRUSH_TEST_SCULPT") != NULL) {
        for (int i = 0; i < 40; i++)
            BrushWorldSculpt(g_world, BRUSH_SCULPT_ADD, (Vector3){0, 0, 14},
                             9.0f, 0.09f, 0.0f);
        BrushWorldSculptSave(g_world, g_terrainPath);
    }

    g_spawnMarker = LoadModel("assets/character/mannequin.glb");
    for (int i = 0; i < g_spawnMarker.materialCount; i++)
        g_spawnMarker.materials[i].shader = BrushGetLitShader();

    RebuildAllColliders();
    g_camera.Init();
    RescanTextureDir(); // material path pickers browse assets/textures
    RescanAssetFiles(); // Assets panel file list

    SetWindowTitle(g_project.name);
    if (!engineDev) AddRecent(dir);
    g_projectOpen = true;
    return true;
}

int main(int argc, char **argv) {
    BrushConsoleInit();
    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(1600, 900, "Brush");
#if defined(__APPLE__)
    EditorMacSeamlessTitlebar(GetWindowHandle());
    EditorMacInstallMenu(); // File/Edit/View live in the macOS top bar
#endif
    SetWindowState(FLAG_WINDOW_MAXIMIZED); // fill whatever screen this is
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); // ESC deselects, never quits

    BrushRenderInit(GetScreenWidth(), GetScreenHeight(), 1.0f);
    BrushRenderSetEditorMode(true);

    // Project-independent GPU objects.
    Image checker = GenImageChecked(1024, 1024, 32, 32, RAYWHITE, (Color){196, 199, 206, 255});
    g_groundTex = LoadTextureFromImage(checker);
    UnloadImage(checker);
    GenTextureMipmaps(&g_groundTex);
    SetTextureFilter(g_groundTex, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(g_groundTex, TEXTURE_WRAP_REPEAT);

    g_unitCube = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
    g_unitCube.materials[0].shader = BrushGetLitShader();

    rlImGuiBeginInitImGui();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Editor-global layout, outside any project tree. Static: ImGui keeps
    // the POINTER, not a copy.
    static char iniPath[560];
    snprintf(iniPath, sizeof(iniPath), "%s/editor_layout.ini", BrushUserDir());
    ImGui::GetIO().IniFilename = iniPath;
    ApplyEditorStyle(); // loads the UI font — must run before EndInitImGui
    rlImGuiEndInitImGui(); // bakes the font atlas

    LoadRecents();

    // Harness: boot straight into a tool for headless UI screenshots.
    const char *bm = getenv("BRUSH_EDITOR_TOOL");
    if (bm != NULL && strcmp(bm, "paint") == 0) {
        g_mode = MODE_SCULPT;
        g_paintActive = true;
    } else if (bm != NULL && strcmp(bm, "sculpt") == 0) {
        g_mode = MODE_SCULPT;
    }

    // CLI/env project selection skips the manager (also how the headless
    // harnesses run: BRUSH_PROJECT=. from the repo root).
    const char *bootDir = getenv("BRUSH_PROJECT");
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--project") == 0) bootDir = argv[i + 1];
    if (bootDir != NULL && bootDir[0] != '\0') OpenProjectAt(bootDir);

    // Harness: BRUSH_TEST_CREATE=<parent>/<name> exercises template
    // creation + open headlessly (BRUSH_TEST_TEMPLATE=empty for the empty
    // template; default gym).
    const char *tc = getenv("BRUSH_TEST_CREATE");
    if (tc != NULL && !g_projectOpen) {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", tc);
        char *slash = strrchr(parent, '/');
        if (slash != NULL && slash[1] != '\0') {
            *slash = '\0';
            const char *te = getenv("BRUSH_TEST_TEMPLATE");
            int tmpl = (te && strcmp(te, "empty") == 0) ? TEMPLATE_EMPTY
                                                        : TEMPLATE_GYM;
            char dir[600];
            if (CreateProject(parent, slash + 1, tmpl, dir, sizeof(dir)))
                OpenProjectAt(dir);
        }
    }

    // === Project Manager =================================================
    static char newName[64] = "My Game";
    static char newParent[512] = "";
    if (newParent[0] == '\0') {
        const char *home = getenv("HOME");
        snprintf(newParent, sizeof(newParent), "%s/Projects", home ? home : ".");
    }
    static int newTemplate = TEMPLATE_GYM;
    int selRecent = -1;

    bool pmQuit = false;
    while (!g_projectOpen && !pmQuit && !WindowShouldClose()) {
#if defined(__APPLE__)
        // The native menu posts action tags; without polling here Cmd+Q
        // (tag 3) does nothing on the manager screen.
        if (EditorMacPollMenuAction() == 3) pmQuit = true;
#endif
        BeginDrawing();
        ClearBackground((Color){ 22, 23, 27, 255 });
        rlImGuiBegin();

        ImGuiViewport *vp = ImGui::GetMainViewport();
        float w = fminf(720.0f, vp->WorkSize.x - 80.0f);
        float h = fminf(640.0f, vp->WorkSize.y - 80.0f);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f,
                                       vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::Begin("##projectmanager", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("Brush");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Select a project or create a new one.");
        ImGui::Spacing();

        ImGui::SeparatorText("Projects");
        ImGui::BeginChild("##recents", ImVec2(0, h * 0.42f));
        for (int i = 0; i < g_recentCount; i++) {
            bool exists = g_recentExists[i];
            char label[640];
            snprintf(label, sizeof(label), "%s##rec%d",
                     exists ? g_recentProjects[i].name : "(missing)", i);
            if (ImGui::Selectable(label, selRecent == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selRecent = i;
                if (exists && ImGui::IsMouseDoubleClicked(0))
                    OpenProjectAt(g_recent[i]);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_recent[i]);
        }
        if (g_recentCount == 0)
            ImGui::TextDisabled("No projects yet — create one below.");
        ImGui::EndChild();
        if (ImGui::Button("Open") && selRecent >= 0)
            OpenProjectAt(g_recent[selRecent]);
        ImGui::SameLine();
        if (ImGui::Button("Remove from list") && selRecent >= 0) {
            for (int j = selRecent; j < g_recentCount - 1; j++)
                strcpy(g_recent[j], g_recent[j + 1]);
            g_recentCount--;
            selRecent = -1;
            SaveRecents();
            ResolveRecentProjectMetadata();
        }

        ImGui::Spacing();
        ImGui::SeparatorText("New Project");
        ImGui::InputText("Name", newName, sizeof(newName));
        ImGui::InputText("Location", newParent, sizeof(newParent));
        ImGui::Combo("Template", &newTemplate, "Sandbox Gym\0Empty\0");
        if (ImGui::Button("Create & Open", ImVec2(-1, 0)) && newName[0]) {
            char dir[600];
            if (CreateProject(newParent, newName, newTemplate, dir,
                              sizeof(dir)))
                OpenProjectAt(dir);
        }

        ImGui::End();
        rlImGuiEnd();
        EndDrawing();

        // Screenshot harness for the manager screen itself.
        static int pmFrames = 0;
        pmFrames++;
        if (getenv("BRUSH_AUTO_SCREENSHOT") != NULL && pmFrames == 30) {
            TakeScreenshot("screenshot.png");
            break;
        }
    }

    if (!g_projectOpen) { // closed from the manager
        rlImGuiShutdown();
        CloseWindow();
        return 0;
    }

    bool viewportHovered = false;

    while (!g_quit && !WindowShouldClose()) {
        float dt = GetFrameTime();

        if (IsWindowResized())
            BrushRenderResize(GetScreenWidth(), GetScreenHeight());

        // --- Scene update -------------------------------------------------
        // Drag & drop: images dropped anywhere on the window import into
        // the project's assets/textures.
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            ImportTextureFiles((const char **)dropped.paths,
                               (int)dropped.count);
            UnloadDroppedFiles(dropped);
        }
        // Harness: BRUSH_TEST_IMPORT=<abs path> imports that image once,
        // exercising the same path as drag & drop.
        static bool testImportDone = false;
        const char *ti = getenv("BRUSH_TEST_IMPORT");
        if (ti != NULL && !testImportDone) {
            testImportDone = true;
            ImportTextureFiles(&ti, 1);
        }
        // Texture import cache: land background re-imports of edited
        // source images and refresh the material table's handles.
        if (BrushAssetsUpdate()) {
            BrushSceneResolveMaterials(&g_scene);
            ApplyTerrainLayers();
            // The Assets preview may hold the swapped-out texture: refresh
            // (release tolerates the stale id via the registry's prevId).
            if (g_assetPreviewPath[0] != '\0') {
                BrushAssetsReleaseTexture(g_assetPreview);
                g_assetPreview = BrushAssetsTexture(g_assetPreviewPath);
            }
            AddEditorLog("Re-imported changed textures");
        }
        BrushTodUpdate(&g_tod, dt);
        BrushRenderApplyTimeOfDay(&g_tod);
        BrushWorldUpdate(g_world, g_camera.cam.position);
        BrushFoliageUpdate(g_foliage, (float)GetTime(), 1.0f);
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

        // Untextured blocks wear the terrain's checker (world-tiled) — the
        // blockout grid look; the block color tints it.
        BrushMaterialProps checker = {};
        checker.albedo = g_groundTex;
        checker.triplanar = true;
        checker.texScale = 64.0f; // = terrain texMetresPerTile
        checker.specStrength = -1.0f;
        for (int i = 0; i < g_scene.blockCount; i++) {
            BrushSceneBlock *k = &g_scene.blocks[i];
            Matrix xf = BrushBlockGetModelMatrix(k); // includes rotation
            BrushMaterialProps props;
            bool hasMat = BrushSceneBlockProps(&g_scene, k, &props);
            BrushRenderSubmitEx(BRUSH_LAYER_OPAQUE, &g_unitCube, xf, k->color,
                                hasMat ? &props : &checker);
            BrushRenderSubmit(BRUSH_LAYER_SHADOW, &g_unitCube, xf, k->color);
        }

        // Placed model props (shared registry models, per-instance matrix).
        // Frustum-cull the opaque submit (off-screen isn't drawn); no distance
        // cull in the editor, so far placed props stay visible when framed.
        BrushFrustum efrust = BrushRenderMakeFrustum(g_camera.cam);
        for (int i = 0; i < g_scene.modelCount; i++) {
            BrushSceneModelInstance *mi = &g_scene.models[i];
            if (mi->model.meshCount == 0) continue;
            Matrix mxf = BrushModelInstanceMatrix(mi);
            Vector3 c; float r;
            BrushBoundingSphere(BrushAssetsModelAABB(mi->path), mxf, &c, &r);
            if (BrushFrustumContainsSphere(&efrust, c, r)) {
                BrushMaterialProps mprops;
                bool hasMMat = BrushSceneModelProps(&g_scene, mi, &mprops) ||
                               BrushSceneModelEmbeddedProps(mi, &mprops);
                BrushRenderSubmitEx(BRUSH_LAYER_OPAQUE, &mi->model, mxf, WHITE,
                                    hasMMat ? &mprops : NULL);
            }
            BrushRenderSubmit(BRUSH_LAYER_SHADOW, &mi->model, mxf, WHITE);
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
            Color ringCol = g_paintActive                       ? MAGENTA
                            : (g_sculptOp == BRUSH_SCULPT_ADD)  ? GREEN
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

        // Road spline preview
        if (pp && g_mode == MODE_SCULPT && g_roadActive && g_selectedRoadIdx >= 0 &&
            g_selectedRoadIdx < g_scene.roadCount) {
            RenderTexture2D *target = pp->smaaEnabled ? &pp->presentAA : &pp->present;
            BeginTextureMode(*target);
            BeginMode3D(g_camera.cam);

            BrushSceneRoad *road = &g_scene.roads[g_selectedRoadIdx];
            int ptCount = road->pointCount;
            
            // Draw node connection segments
            for (int i = 0; i < ptCount - 1; i++) {
                DrawLine3D(road->points[i], road->points[i+1], Fade(SKYBLUE, 0.4f));
            }

            // Draw node spheres
            for (int i = 0; i < ptCount; i++) {
                Color c = (i == g_selectedNodeIdx) ? ORANGE : SKYBLUE;
                DrawSphere(road->points[i], 0.35f, Fade(c, 0.8f));
                DrawSphereWires(road->points[i], 0.36f, 8, 8, c);
            }

            // Draw the road corridor ribbon. The SAME polyline the stamp uses
            // (BrushWorldRoadPolyline), and the ribbon rides the spline Y — i.e.
            // the surface the stamp will carve to — so the preview matches the
            // result even after a node is lifted off the ground.
            int polyN = 0;
            Vector3 *poly = BrushWorldRoadPolyline(g_world, road->points,
                                                   road->pointCount, &polyN);
            if (poly != NULL) {
                Vector3 prevC = poly[0], prevL = poly[0], prevR = poly[0];
                bool first = true;
                float halfW = road->width * 0.5f;

                for (int i = 0; i < polyN; i++) {
                    Vector3 pt = poly[i];
                    pt.y += 0.05f; // lift off the surface to avoid z-fighting

                    Vector3 dir = {0, 0, 0};
                    if (i + 1 < polyN)      dir = Vector3Subtract(poly[i+1], poly[i]);
                    else if (i > 0)         dir = Vector3Subtract(poly[i], poly[i-1]);
                    dir.y = 0.0f;
                    dir = Vector3Normalize(dir);
                    Vector3 right = { dir.z, 0, -dir.x };

                    // Corridor core is flattened to roadY across the full width,
                    // so both shoulders sit at the spline Y too.
                    Vector3 leftPt = Vector3Add(pt, Vector3Scale(right, halfW));
                    Vector3 rightPt = Vector3Subtract(pt, Vector3Scale(right, halfW));

                    if (!first) {
                        DrawLine3D(prevC, pt, SKYBLUE);
                        DrawLine3D(prevL, leftPt, Fade(GOLD, 0.6f));
                        DrawLine3D(prevR, rightPt, Fade(GOLD, 0.6f));
                    }
                    prevC = pt;
                    prevL = leftPt;
                    prevR = rightPt;
                    first = false;
                }
                MemFree(poly);
            }

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
            } else if (g_selectedType == ENTITY_MODEL && g_selectedIdx >= 0 &&
                       g_selectedIdx < g_scene.modelCount) {
                BrushSceneModelInstance *mi = &g_scene.models[g_selectedIdx];
                if (mi->model.meshCount > 0) {
                    BoundingBox bb = GetModelBoundingBox(mi->model);
                    Vector3 c = Vector3Scale(Vector3Add(bb.min, bb.max), 0.5f);
                    Vector3 sz = Vector3Subtract(bb.max, bb.min);
                    rlPushMatrix();
                    Matrix mxf = BrushModelInstanceMatrix(mi);
                    rlMultMatrixf(MatrixToFloat(mxf));
                    DrawCubeWires(c, sz.x * 1.02f, sz.y * 1.02f, sz.z * 1.02f,
                                  GREEN);
                    rlPopMatrix();
                }
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

        // --- Top strip: a slim bar the macOS traffic lights float over.
        // Hosts Play/Stop + save state and doubles as the window drag area;
        // the dockspace starts below it so no tab ever hides under the
        // window buttons.
        const float TOPBAR_H = 38.0f;
        ImGuiViewport *mainVp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainVp->Pos);
        ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, TOPBAR_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.086f, 0.090f, 0.106f, 1.0f));
        ImGui::Begin("##topbar", NULL,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        {
#if defined(__APPLE__)
            ImGui::SetCursorPosX(84.0f); // clear the traffic lights
#endif
            ImGui::SetCursorPosY((TOPBAR_H - ImGui::GetFrameHeight()) * 0.5f);
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

            // Right side: scene path + fps.
            char status[128];
            snprintf(status, sizeof(status), "%s   %d fps", g_scenePath, GetFPS());
            float sw = ImGui::CalcTextSize(status).x;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sw - 8);
            ImGui::TextDisabled("%s", status);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Dockspace fills everything below the top strip.
        ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x, mainVp->Pos.y + TOPBAR_H));
        ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, mainVp->Size.y - TOPBAR_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##dockhost", NULL,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoBackground);
        ImGuiID dockspaceId = ImGui::GetID("BrushDockspace");
        ImGui::DockSpace(dockspaceId);
        ImGui::End();
        ImGui::PopStyleVar(2);

        // Default layout only when none exists yet — imgui.ini keeps the
        // user's arrangement across sessions.
        ImGuiDockNode *rootNode = ImGui::DockBuilderGetNode(dockspaceId);
        if (rootNode == NULL || rootNode->IsLeafNode()) {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_None);
            ImGui::DockBuilderSetNodeSize(
                dockspaceId, ImVec2(mainVp->Size.x, mainVp->Size.y - TOPBAR_H));
            ImGuiID dockLeft, dockCenter, dockRight, dockView, dockBottom, dockViewFinal;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.16f, &dockLeft, &dockCenter);
            ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.24f, &dockRight, &dockView);
            ImGui::DockBuilderSplitNode(dockView, ImGuiDir_Down, 0.22f, &dockBottom, &dockViewFinal);
            ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
            ImGui::DockBuilderDockWindow("Viewport", dockViewFinal);
            ImGui::DockBuilderDockWindow("Inspector", dockRight);
            ImGui::DockBuilderDockWindow("Materials", dockRight);
            ImGui::DockBuilderDockWindow("Environment", dockRight);
            ImGui::DockBuilderDockWindow("Console", dockBottom);
            ImGui::DockBuilderDockWindow("Assets", dockBottom);
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
        if (GetMousePosition().y < 38.0f &&
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
        char modelsHdr[32];
        snprintf(modelsHdr, sizeof(modelsHdr), "Models (%d)###models", g_scene.modelCount);
        if (g_scene.modelCount > 0 &&
            ImGui::CollapsingHeader(modelsHdr, ImGuiTreeNodeFlags_DefaultOpen))
        for (int i = 0; i < g_scene.modelCount; i++) {
            char label[160];
            snprintf(label, sizeof(label), "  %s##m%d",
                     GetFileName(g_scene.models[i].path), i);
            bool sel = (g_selectedType == ENTITY_MODEL && g_selectedIdx == i);
            if (ImGui::Selectable(label, sel)) { g_selectedType = ENTITY_MODEL; g_selectedIdx = i; }
            if (ImGui::BeginPopupContextItem()) {
                g_selectedType = ENTITY_MODEL; g_selectedIdx = i;
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
        char roadsHdr[32];
        snprintf(roadsHdr, sizeof(roadsHdr), "Roads (%d)###roads", g_scene.roadCount);
        if (ImGui::CollapsingHeader(roadsHdr, ImGuiTreeNodeFlags_DefaultOpen))
        for (int i = 0; i < g_scene.roadCount; i++) {
            char label[64];
            snprintf(label, sizeof(label), "  Road %d  (%d pts)##r%d", i,
                     g_scene.roads[i].pointCount, i);
            bool sel = (g_selectedType == ENTITY_ROAD && g_selectedIdx == i);
            // Selecting a road enters the Road tool so its nodes are editable.
            if (ImGui::Selectable(label, sel)) {
                g_selectedType = ENTITY_ROAD; g_selectedIdx = i;
                g_selectedRoadIdx = i; g_selectedNodeIdx = -1;
                g_mode = MODE_SCULPT; g_roadActive = true; g_paintActive = false;
            }
            if (ImGui::BeginPopupContextItem()) {
                g_selectedType = ENTITY_ROAD; g_selectedIdx = i;
                g_selectedRoadIdx = i; g_selectedNodeIdx = -1;
                if (ImGui::MenuItem("Frame")) FrameSelected();
                if (ImGui::MenuItem("Delete")) DeleteSelected();
                ImGui::EndPopup();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float w = (ImGui::GetContentRegionAvail().x -
                   ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        if (ImGui::Button("+ Block", ImVec2(w, 0))) AddBlockEntity();
        ImGui::SameLine();
        if (ImGui::Button("+ Light", ImVec2(w, 0))) AddLightEntity();
        ImGui::SameLine();
        // Models are file-backed: the button is the quick-add path (a picker
        // over the project's model files, placed in front of the camera);
        // dragging from the Assets panel into the viewport stays the
        // precise-placement flow, like Unity/Unreal/Godot.
        if (ImGui::Button("+ Model", ImVec2(w, 0)))
            ImGui::OpenPopup("##addmodel");
        if (ImGui::BeginPopup("##addmodel")) {
            int shown = 0;
            for (int i = 0; i < g_assetFileCount; i++) {
                if (!IsModelAsset(g_assetFiles[i])) continue;
                shown++;
                if (ImGui::Selectable(GetFileName(g_assetFiles[i]))) {
                    AddModelEntityAt(g_assetFiles[i], SpawnPointInFront());
                    ImGui::CloseCurrentPopup();
                }
            }
            if (shown == 0)
                ImGui::TextDisabled("No models — drop .glb files\nonto the window to import.");
            ImGui::EndPopup();
        }
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
            // Material assignment (materials themselves live in the
            // Materials panel; the color above tints the texture).
            const char *matPreview = b->material[0] ? b->material : "(none)";
            if (ImGui::BeginCombo("Material", matPreview)) {
                if (ImGui::Selectable("(none)", b->material[0] == '\0') &&
                    b->material[0] != '\0') {
                    b->material[0] = '\0';
                    g_dirty = true;
                }
                for (int i = 0; i < g_scene.materialCount; i++) {
                    const char *mn = g_scene.materials[i].name;
                    bool sel = (strcmp(b->material, mn) == 0);
                    if (ImGui::Selectable(mn, sel) && !sel) {
                        snprintf(b->material, sizeof(b->material), "%s", mn);
                        g_dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
        } else if (g_selectedType == ENTITY_MODEL && g_selectedIdx >= 0 &&
                   g_selectedIdx < g_scene.modelCount) {
            BrushSceneModelInstance *mi = &g_scene.models[g_selectedIdx];
            ImGui::SeparatorText(TextFormat("Model %d", g_selectedIdx));
            ImGui::TextDisabled("%s", mi->path);
            float mpos[3] = { mi->pos.x, mi->pos.y, mi->pos.z };
            float mrot[3] = { mi->rot.x, mi->rot.y, mi->rot.z };
            float mscl[3] = { mi->scale.x, mi->scale.y, mi->scale.z };
            bool mchanged = false;
            if (ImGui::DragFloat3("Position", mpos, 0.1f)) {
                mi->pos = (Vector3){ mpos[0], mpos[1], mpos[2] }; mchanged = true;
            }
            if (ImGui::DragFloat3("Rotation", mrot, 1.0f, -360.0f, 360.0f, "%.1f deg")) {
                mi->rot = (Vector3){ mrot[0], mrot[1], mrot[2] }; mchanged = true;
            }
            if (ImGui::DragFloat3("Scale", mscl, 0.05f, 0.01f, 100.0f)) {
                mi->scale = (Vector3){ mscl[0], mscl[1], mscl[2] }; mchanged = true;
            }
            // Swap the mesh file: ResolveMaterials handles ref bookkeeping.
            if (ImGui::BeginCombo("Mesh", GetFileName(mi->path))) {
                for (int i = 0; i < g_assetFileCount; i++) {
                    if (!IsModelAsset(g_assetFiles[i])) continue;
                    bool sel = (strcmp(mi->path, g_assetFiles[i]) == 0);
                    if (ImGui::Selectable(GetFileName(g_assetFiles[i]), sel) && !sel) {
                        snprintf(mi->path, sizeof(mi->path), "%s", g_assetFiles[i]);
                        BrushSceneResolveMaterials(&g_scene);
                        mchanged = true;
                    }
                }
                ImGui::EndCombo();
            }
            // Optional material (same library as blocks): triplanar wrap.
            const char *mmatPreview = mi->material[0] ? mi->material : "(model's own)";
            if (ImGui::BeginCombo("Material", mmatPreview)) {
                if (ImGui::Selectable("(model's own)", mi->material[0] == '\0') &&
                    mi->material[0] != '\0') {
                    mi->material[0] = '\0';
                    g_dirty = true;
                }
                for (int i = 0; i < g_scene.materialCount; i++) {
                    const char *mn = g_scene.materials[i].name;
                    bool sel = (strcmp(mi->material, mn) == 0);
                    if (ImGui::Selectable(mn, sel) && !sel) {
                        snprintf(mi->material, sizeof(mi->material), "%s", mn);
                        g_dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            static bool modelInspStale = false;
            if (mchanged) { g_dirty = true; modelInspStale = true; }
            if (modelInspStale && !ImGui::IsAnyItemActive()) {
                RebuildAllColliders();
                modelInspStale = false;
            }
            if (mi->model.meshCount == 0)
                ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "mesh MISSING");
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
        ImGuiID inspectorDockId = ImGui::GetWindowDockID();
        ImGui::End();

        // === Materials =======================================================
        // Scene-owned material library: triplanar texture sets blocks
        // reference by name (assigned in the Inspector).
        if (inspectorDockId != 0)
            ImGui::SetNextWindowDockID(inspectorDockId, ImGuiCond_FirstUseEver);
        ImGui::Begin("Materials");
        {
            static int selMat = -1;
            static char nameBuf[BRUSH_SCENE_NAME_MAX] = "";
            if (selMat >= g_scene.materialCount) selMat = -1;

            for (int i = 0; i < g_scene.materialCount; i++) {
                if (ImGui::Selectable(
                        TextFormat("%s##mat%d", g_scene.materials[i].name, i),
                        selMat == i)) {
                    selMat = i;
                    snprintf(nameBuf, sizeof(nameBuf), "%s",
                             g_scene.materials[i].name);
                }
            }
            float bw = (ImGui::GetContentRegionAvail().x -
                        ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("+ Material", ImVec2(bw, 0)) &&
                g_scene.materialCount < BRUSH_SCENE_MAX_MATERIALS) {
                BrushSceneMaterial *m =
                    &g_scene.materials[g_scene.materialCount];
                memset(m, 0, sizeof(*m));
                snprintf(m->name, sizeof(m->name), "mat_%d",
                         g_scene.materialCount);
                m->tile = 2.0f;
                m->spec = 0.25f;
                m->normalDepth = 1.0f;
                m->heightScale = 0.05f;
                m->aoStrength = 1.0f;
                selMat = g_scene.materialCount++;
                snprintf(nameBuf, sizeof(nameBuf), "%s", m->name);
                g_dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete", ImVec2(bw, 0)) && selMat >= 0) {
                // Clear references so blocks fall back to plain color.
                for (int i = 0; i < g_scene.blockCount; i++)
                    if (strcmp(g_scene.blocks[i].material,
                               g_scene.materials[selMat].name) == 0)
                        g_scene.blocks[i].material[0] = '\0';
                BrushAssetsReleaseTexture(g_scene.materials[selMat].albedoTex);
                BrushAssetsReleaseTexture(g_scene.materials[selMat].normalTex);
                BrushAssetsReleaseTexture(g_scene.materials[selMat].displacementTex);
                BrushAssetsReleaseTexture(g_scene.materials[selMat].aoTex);
                for (int i = selMat; i < g_scene.materialCount - 1; i++)
                    g_scene.materials[i] = g_scene.materials[i + 1];
                g_scene.materialCount--;
                selMat = -1;
                g_dirty = true;
            }

            if (selMat >= 0 && selMat < g_scene.materialCount) {
                BrushSceneMaterial *m = &g_scene.materials[selMat];
                ImGui::SeparatorText("Selected Material");
                ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
                if (ImGui::IsItemDeactivatedAfterEdit() && nameBuf[0] != '\0' &&
                    strcmp(nameBuf, m->name) != 0) {
                    // Rename: keep every block reference pointing here.
                    for (int i = 0; i < g_scene.blockCount; i++)
                        if (strcmp(g_scene.blocks[i].material, m->name) == 0)
                            snprintf(g_scene.blocks[i].material,
                                     sizeof(g_scene.blocks[i].material), "%s",
                                     nameBuf);
                    snprintf(m->name, sizeof(m->name), "%s", nameBuf);
                    g_dirty = true;
                }
                bool reresolve = false;
                reresolve |= TexPathCombo("Albedo", m->albedo,
                                          sizeof(m->albedo));
                reresolve |= TexPathCombo("Normal", m->normal,
                                          sizeof(m->normal));
                reresolve |= TexPathCombo("Displacement", m->displacement,
                                          sizeof(m->displacement));
                reresolve |= TexPathCombo("AO", m->ao,
                                          sizeof(m->ao));
                // Thumbnails: proof the textures actually loaded. A path
                // with no image = red MISSING (typo / file moved).
                float thumb = 72.0f;
                if (m->albedoTex.id != 0) {
                    ImGui::Image((ImTextureID)(intptr_t)m->albedoTex.id,
                                 ImVec2(thumb, thumb));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s (%dx%d)", m->albedo,
                                          m->albedoTex.width,
                                          m->albedoTex.height);
                } else if (m->albedo[0] != '\0') {
                    ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1),
                                       "albedo MISSING: %s", m->albedo);
                }
                if (m->normalTex.id != 0) {
                    ImGui::SameLine();
                    ImGui::Image((ImTextureID)(intptr_t)m->normalTex.id,
                                 ImVec2(thumb, thumb));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s (%dx%d)%s", m->normal, m->normalTex.width,
                            m->normalTex.height,
                            BrushAssetsIsSwizzledNormal(m->normalTex)
                                ? "\nDXT5nm (preview looks yellow — normal)"
                                : "");
                } else if (m->normal[0] != '\0') {
                    ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1),
                                       "normal MISSING: %s", m->normal);
                }
                if (m->displacementTex.id != 0) {
                    ImGui::SameLine();
                    ImGui::Image((ImTextureID)(intptr_t)m->displacementTex.id,
                                 ImVec2(thumb, thumb));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s (%dx%d)", m->displacement,
                                          m->displacementTex.width,
                                          m->displacementTex.height);
                } else if (m->displacement[0] != '\0') {
                    ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1),
                                       "displacement MISSING: %s", m->displacement);
                }
                if (m->aoTex.id != 0) {
                    ImGui::SameLine();
                    ImGui::Image((ImTextureID)(intptr_t)m->aoTex.id,
                                 ImVec2(thumb, thumb));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s (%dx%d)", m->ao,
                                          m->aoTex.width,
                                          m->aoTex.height);
                } else if (m->ao[0] != '\0') {
                    ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1),
                                       "ao MISSING: %s", m->ao);
                }
                bool matEdited = false;
                if (ImGui::DragFloat("Tile", &m->tile, 0.05f, 0.25f, 32.0f,
                                     "%.2f m")) matEdited = true;
                if (ImGui::SliderFloat("Specular", &m->spec, 0.0f, 1.0f))
                    matEdited = true;
                if (ImGui::SliderFloat("Normal Depth", &m->normalDepth, 0.0f,
                                       3.0f)) matEdited = true;
                int proj = m->uvProjection ? 1 : 0;
                if (ImGui::Combo("Projection", &proj,
                                 "Triplanar (world wrap)\0Model UVs (authored)\0")) {
                    m->uvProjection = (proj == 1);
                    matEdited = true;
                }
                if (ImGui::SliderFloat("Displacement Scale", &m->heightScale, 0.0f,
                                       0.2f, "%.3f")) matEdited = true;
                if (ImGui::Checkbox("Parallax (POM)", &m->parallax)) matEdited = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Ray-march the displacement map for real depth "
                                      "(cobbles/brick/rock up close). Needs a Displacement "
                                      "map; fades to flat with distance.");
                if (m->parallax && m->displacementTex.id == 0)
                    ImGui::TextColored(ImVec4(1, 0.65f, 0.25f, 1),
                                       "  needs a Displacement map to show");
                if (ImGui::Checkbox("Height Blend", &m->heightBlend)) matEdited = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("For terrain layers (roads): blend this layer's edge "
                                      "by its height so raised features (stones) persist and "
                                      "the neighbour fills the recesses — a crisp interlocking "
                                      "edge instead of a straight fade. Needs a Displacement map.");
                if (m->heightBlend) {
                    if (m->blendSharp <= 0.0f) m->blendSharp = 0.2f;
                    if (ImGui::SliderFloat("Blend Sharpness", &m->blendSharp, 0.02f, 0.6f,
                                           "%.2f")) matEdited = true;
                    if (m->displacementTex.id == 0)
                        ImGui::TextColored(ImVec4(1, 0.65f, 0.25f, 1),
                                           "  needs a Displacement map to show");
                }
                if (ImGui::SliderFloat("AO Strength", &m->aoStrength, 0.0f,
                                       1.0f)) matEdited = true;
                if (reresolve) BrushSceneResolveMaterials(&g_scene);
                if (matEdited || reresolve) {
                    // A material may back a terrain layer; re-push so the world
                    // re-bakes with the new textures/flags (POM, tile, …). Blocks
                    // and models rebuild their props each frame, but terrain
                    // layers are cached in the world.
                    ApplyTerrainLayers();
                    g_dirty = true;
                }
                if (ImGui::SmallButton("Rescan texture folder"))
                    RescanTextureDir();
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Select or add a material.");
            }

        }
        ImGui::End();

        // === Environment =====================================================
        ImGui::Begin("Environment");
        ImGui::SeparatorText("Time of Day");
        if (ImGui::SliderFloat("Hour", &g_tod.timeHours, 0.0f, 24.0f, "%.2f")) g_dirty = true;
        ImGui::DragFloat("Speed", &g_tod.timeScale, 0.01f, -10.0f, 10.0f);

        // Paintable terrain layers: material-library entries per slot
        // (saved as terrain_layer lines; slot 0 is the base coat the whole
        // terrain wears, so setting it retextures everything instantly).
        ImGui::SeparatorText("Terrain Layers");
        {
            bool layersChanged = false;
            for (int slot = 0; slot < BRUSH_TERRAIN_LAYERS; slot++) {
                const char *cur = g_scene.terrainLayers[slot][0]
                                      ? g_scene.terrainLayers[slot]
                                      : "(none)";
                ImGui::PushID(slot);
                if (ImGui::BeginCombo(TextFormat("Layer %d%s", slot,
                                                 slot == 0 ? " (base)" : ""),
                                      cur)) {
                    if (ImGui::Selectable("(none)",
                                          g_scene.terrainLayers[slot][0] == '\0') &&
                        g_scene.terrainLayers[slot][0] != '\0') {
                        g_scene.terrainLayers[slot][0] = '\0';
                        layersChanged = true;
                    }
                    for (int m = 0; m < g_scene.materialCount; m++) {
                        const char *mn = g_scene.materials[m].name;
                        bool sel = (strcmp(g_scene.terrainLayers[slot], mn) == 0);
                        if (ImGui::Selectable(mn, sel) && !sel) {
                            snprintf(g_scene.terrainLayers[slot],
                                     sizeof(g_scene.terrainLayers[slot]), "%s", mn);
                            layersChanged = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            // Slots must be contiguous: painted weights are indexed by
            // slot, so a gap silently disables everything above it.
            {
                int firstEmpty = -1;
                for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++)
                    if (g_scene.terrainLayers[i][0] == '\0') { firstEmpty = i; break; }
                if (firstEmpty >= 0)
                    for (int i = firstEmpty + 1; i < BRUSH_TERRAIN_LAYERS; i++)
                        if (g_scene.terrainLayers[i][0] != '\0') {
                            ImGui::TextColored(
                                ImVec4(1, 0.65f, 0.25f, 1),
                                "Slot %d is empty: slots above it are ignored.\n"
                                "Fill layer slots in order (0, 1, 2, 3).",
                                firstEmpty);
                            break;
                        }
            }

            // Auto-slope: one layer auto-applied to steep ground (the
            // classic "rock on cliffs"). Applied AFTER the height bands, so
            // it wins on cliffs regardless of altitude. Steepness is the
            // surface angle: 0 deg = flat, 90 deg = vertical wall.
            ImGui::SeparatorText("Auto-slope (steep ground)");
            if (TerrainSlotCombo("Layer##slope", &g_scene.autoSlopeLayer))
                layersChanged = true;
            if (g_scene.autoSlopeLayer >= 0) {
                bool a = false;
                a |= ImGui::SliderFloat("Steeper than", &g_scene.autoSlopeStart,
                                        5.0f, 80.0f, "%.0f deg");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Ground steeper than this starts showing "
                                      "the layer (0 deg flat, 90 deg wall).");
                a |= ImGui::SliderFloat("Fully by", &g_scene.autoSlopeEnd,
                                        10.0f, 89.0f, "%.0f deg");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Steeper than this is fully the layer; "
                                      "the two angles set the blend band.");
                if (a) {
                    if (g_scene.autoSlopeEnd < g_scene.autoSlopeStart + 1.0f)
                        g_scene.autoSlopeEnd = g_scene.autoSlopeStart + 1.0f;
                    layersChanged = true;
                }
            } else {
                ImGui::TextDisabled("Off — pick a layer for cliffs/steep faces.");
            }
            // Auto-height: one optional altitude band PER layer, so every
            // configured layer can be placed by height (grass low, rock
            // mid, snow high...). full > start = fades in going UP; full <
            // start = going DOWN (shoreline). Applied in slot order beneath
            // the paint; slope still wins on cliffs.
            ImGui::SeparatorText("Auto-height (per layer)");
            for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++) {
                if (g_scene.terrainLayers[i][0] == '\0') continue; // unset slot
                ImGui::PushID(1000 + i);
                bool on = g_scene.layerHeightOn[i] != 0;
                if (ImGui::Checkbox(TerrainSlotLabel(i), &on)) {
                    g_scene.layerHeightOn[i] = on ? 1 : 0;
                    // Seed a sensible band the first time it's enabled.
                    if (on && g_scene.layerHeightStart[i] == 0.0f &&
                        g_scene.layerHeightFull[i] == 0.0f) {
                        g_scene.layerHeightStart[i] = (float)i * 4.0f;
                        g_scene.layerHeightFull[i] = (float)i * 4.0f + 3.0f;
                    }
                    layersChanged = true;
                }
                if (on) {
                    ImGui::Indent();
                    bool a = false;
                    a |= ImGui::DragFloat("start Y", &g_scene.layerHeightStart[i],
                                          0.1f, -200.0f, 500.0f, "%.1f m");
                    a |= ImGui::DragFloat("full Y", &g_scene.layerHeightFull[i],
                                          0.1f, -200.0f, 500.0f, "%.1f m");
                    if (a) layersChanged = true;
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            if (layersChanged) {
                ApplyTerrainLayers();
                g_roadResyncPending = true; // layer slots moved -> roads re-resolve
                g_dirty = true;
            }
        }
        // Everything below persists into world.def "post" lines on save
        // (BrushSceneCaptureRenderSettings) — the scene carries its look.
        ImGui::SeparatorText("Post Processing");
        if (pp) {
            bool ch = false;
            ch |= ImGui::DragFloat("Exposure", &pp->exposure, 0.05f, 0.1f, 5.0f);
            ch |= ImGui::DragFloat("Bloom", &pp->bloomIntensity, 0.05f, 0.0f, 10.0f);
            ch |= ImGui::DragFloat("Bloom Threshold", &pp->bloomThreshold, 0.05f, 0.0f, 5.0f);
            ImGui::Checkbox("SMAA", &pp->smaaEnabled); // AA is a machine pref, not scene look

            ImGui::SeparatorText("Ambient Occlusion");
            ch |= ImGui::Checkbox("SSAO", &pp->ssaoEnabled);
            if (pp->ssaoEnabled) {
                ch |= ImGui::SliderFloat("Radius##ao", &pp->ssaoRadius, 0.1f, 3.0f, "%.2f m");
                ch |= ImGui::SliderFloat("Strength##ao", &pp->ssaoStrength, 0.0f, 2.0f);
            }

            ImGui::SeparatorText("Depth of Field");
            ch |= ImGui::Checkbox("DOF", &pp->dofEnabled);
            if (pp->dofEnabled) {
                ch |= ImGui::DragFloat("Range##dof", &pp->dofRange, 1.0f, 5.0f, 400.0f, "%.0f m");
                ch |= ImGui::SliderFloat("Strength##dof", &pp->dofStrength, 0.0f, 1.0f);
            }

            ImGui::SeparatorText("God Rays");
            ch |= ImGui::Checkbox("God Rays", &pp->godRaysEnabled);
            if (pp->godRaysEnabled) {
                ch |= ImGui::SliderFloat("Density##gr", &pp->godRaysDensity, 0.0f, 3.0f);
                ch |= ImGui::SliderFloat("Brightness##gr", &pp->godRaysExposure, 0.0f, 3.0f);
            }

            ImGui::SeparatorText("Volumetric Fog");
            ch |= ImGui::Checkbox("Fog", &pp->volFogEnabled);
            if (pp->volFogEnabled) {
                ch |= ImGui::SliderFloat("Density##vf", &pp->volFogDensity, 0.0f, 0.3f, "%.3f");
                ch |= ImGui::DragFloat("Ground Y##vf", &pp->volFogGroundY, 0.1f, -50.0f, 200.0f, "%.1f m");
                ch |= ImGui::DragFloat("Height##vf", &pp->volFogTopY, 0.1f, 0.5f, 60.0f, "%.1f m");
                ch |= ImGui::SliderFloat("Coverage##vf", &pp->volFogCoverage, 0.0f, 1.0f);
            }

            BrushShadow *sh = BrushRenderGetShadow();
            if (sh) {
                ImGui::SeparatorText("Sun Shadows");
                ch |= ImGui::SliderFloat("Softness", &sh->softness, 1.0f, 16.0f);
            }
            if (ch) g_dirty = true; // persisted via "post" lines on save
        } else {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "HDR pipeline offline");
        }

        // --- Brush Constraints UI ---
        if (ImGui::CollapsingHeader("Brush Constraints")) {
            ImGui::Checkbox("Slope Filter", &g_useSlopeFilter);
            if (g_useSlopeFilter) {
                ImGui::Indent();
                ImGui::SliderFloat("Min Angle", &g_constrainSlopeDegMin, 0.0f, 90.0f, "%.0f deg");
                ImGui::SliderFloat("Max Angle", &g_constrainSlopeDegMax, 0.0f, 90.0f, "%.0f deg");
                if (g_constrainSlopeDegMax < g_constrainSlopeDegMin)
                    g_constrainSlopeDegMax = g_constrainSlopeDegMin;
                ImGui::Unindent();
            }

            ImGui::Checkbox("Height Filter", &g_useHeightFilter);
            if (g_useHeightFilter) {
                ImGui::Indent();
                ImGui::DragFloat("Min Height", &g_constrainHeightMin, 0.1f, -100.0f, 1000.0f, "%.1f m");
                ImGui::DragFloat("Max Height", &g_constrainHeightMax, 0.1f, -100.0f, 1000.0f, "%.1f m");
                if (g_constrainHeightMax < g_constrainHeightMin)
                    g_constrainHeightMax = g_constrainHeightMin;
                ImGui::Unindent();
            }

            const char *layerComboStr = "(none)";
            if (g_constrainLayerIdx >= 0 && g_constrainLayerIdx < BRUSH_TERRAIN_LAYERS) {
                layerComboStr = TerrainSlotLabel(g_constrainLayerIdx);
            }
            if (ImGui::BeginCombo("Layer Mask", layerComboStr)) {
                if (ImGui::Selectable("(none)", g_constrainLayerIdx == -1)) {
                    g_constrainLayerIdx = -1;
                }
                BrushTerrainLayer tl[BRUSH_TERRAIN_LAYERS];
                int tlCount = BrushSceneTerrainLayers(&g_scene, tl);
                for (int i = 0; i < tlCount; i++) {
                    bool sel = (g_constrainLayerIdx == i);
                    if (ImGui::Selectable(TerrainSlotLabel(i), sel)) {
                        g_constrainLayerIdx = i;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Only modify samples where this layer is dominant.");
            }
        }

        // --- Road Splines UI ---
        if (ImGui::CollapsingHeader("Road Splines")) {
            if (g_scene.roadCount == 0) {
                ImGui::TextDisabled("No roads in scene.");
            } else {
                for (int i = 0; i < g_scene.roadCount; i++) {
                    char label[64];
                    snprintf(label, sizeof(label), "Road %d (%s, %d pts)", i, 
                             g_scene.roads[i].material[0] ? g_scene.roads[i].material : "none",
                             g_scene.roads[i].pointCount);
                    bool isSel = (g_selectedRoadIdx == i);
                    if (ImGui::Selectable(label, isSel)) {
                        g_selectedRoadIdx = i;
                        g_selectedNodeIdx = -1;
                    }
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Add Road Spline")) {
                if (g_scene.roadCount < BRUSH_SCENE_MAX_ROADS) {
                    g_selectedRoadIdx = g_scene.roadCount++;
                    NewRoad(&g_scene.roads[g_selectedRoadIdx]);
                    g_selectedNodeIdx = -1;
                    g_roadActive = true;
                    g_mode = MODE_SCULPT;
                    g_dirty = true;
                    AddEditorLog("Created new road spline %d — click the terrain to add nodes",
                                 g_selectedRoadIdx);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(live: edits update the terrain)");

            if (g_selectedRoadIdx >= 0 && g_selectedRoadIdx < g_scene.roadCount) {
                ImGui::Separator();
                BrushSceneRoad *road = &g_scene.roads[g_selectedRoadIdx];
                ImGui::Text("Selected: Road %d", g_selectedRoadIdx);
                
                // Only TERRAIN-LAYER materials can be painted (the splat is
                // slot-indexed), so offer just those — no dead choices.
                if (ImGui::BeginCombo("Road Material", road->material[0] ? road->material : "(none)")) {
                    bool any = false;
                    for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++) {
                        const char *mn = g_scene.terrainLayers[i];
                        if (!mn[0]) continue;
                        any = true;
                        bool sel = (strcmp(road->material, mn) == 0);
                        if (ImGui::Selectable(TerrainSlotLabel(i), sel)) {
                            strcpy(road->material, mn);
                            g_dirty = true;
                            g_roadResyncPending = true;
                        }
                    }
                    if (!any) ImGui::TextDisabled("No Terrain Layers set");
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Road paints this layer. Set Terrain Layers in the "
                                      "Terrain panel first; an unset material carves shape only.");

                if (ImGui::SliderFloat("Width", &road->width, 1.0f, 30.0f, "%.1f m")) { g_dirty = true; g_roadResyncPending = true; }
                if (ImGui::SliderFloat("Ground Shoulder", &road->fade, 0.5f, 20.0f, "%.1f m")) { g_dirty = true; g_roadResyncPending = true; }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far the TERRAIN eases back to its original height "
                                      "at the road edge (keep >0 so there's no cliff).");
                if (ImGui::SliderFloat("Texture Edge", &road->paintFade, 0.0f, 20.0f,
                                       road->paintFade < 0.05f ? "hard" : "%.1f m"))
                    { g_dirty = true; g_roadResyncPending = true; }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far the road TEXTURE feathers into the surrounding "
                                      "terrain. 0 = hard edge (paving/concrete); higher = "
                                      "soft (dirt/gravel).");

                ImGui::Text("Control Points (%d/32):", road->pointCount);
                ImGui::Indent();
                for (int pi = 0; pi < road->pointCount; pi++) {
                    char nodeLabel[64];
                    snprintf(nodeLabel, sizeof(nodeLabel), "Node %d: (%.1f, %.1f, %.1f)", pi, 
                             road->points[pi].x, road->points[pi].y, road->points[pi].z);
                    bool nodeSel = (g_selectedNodeIdx == pi);
                    if (ImGui::Selectable(nodeLabel, nodeSel)) {
                        g_selectedNodeIdx = pi;
                    }
                }
                ImGui::Unindent();

                ImGui::Spacing();
                if (ImGui::Button("Delete Selected Node")) {
                    if (g_selectedNodeIdx >= 0 && g_selectedNodeIdx < road->pointCount) {
                        for (int pi = g_selectedNodeIdx; pi < road->pointCount - 1; pi++) {
                            road->points[pi] = road->points[pi+1];
                        }
                        road->pointCount--;
                        g_selectedNodeIdx = -1;
                        g_dirty = true;
                        g_roadResyncPending = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete Spline")) {
                    for (int ri = g_selectedRoadIdx; ri < g_scene.roadCount - 1; ri++) {
                        g_scene.roads[ri] = g_scene.roads[ri+1];
                    }
                    g_scene.roadCount--;
                    g_selectedRoadIdx = -1;
                    g_selectedNodeIdx = -1;
                    g_dirty = true;
                    g_roadResyncPending = true; // rebake to erase the removed road
                }
            }
        }

        // --- Foliage UI ------------------------------------------------------
        // Procedural world config (like terrain layers), not a gizmo entity.
        // Placement edits (density/scale/model) re-scatter; draw-only edits
        // (tint/wind/distance) just rebuild the layers — both deferred to idle.
        if (ImGui::CollapsingHeader("Foliage")) {
            // Runtime graphics setting (not scene data): draw distance + shadow
            // reception. Low cuts grass overdraw — the thermal lever.
            const char *qNames[] = { "Low", "Medium", "High" };
            int q = (int)BrushFoliageGetQuality(g_foliage);
            if (ImGui::Combo("Quality", &q, qNames, 3))
                BrushFoliageSetQuality(g_foliage, (BrushFoliageQuality)q);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Foliage draw distance + shadow reception. Low cuts "
                                  "grass overdraw at 4x retina (the thermal lever).");
            ImGui::Spacing();

            if (g_scene.foliageCount == 0)
                ImGui::TextDisabled("No foliage layers.");
            for (int i = 0; i < g_scene.foliageCount; i++) {
                char label[64];
                int nc = 0, fc = 0;
                BrushFoliageLayerStats(g_foliage, i, &nc, &fc);
                snprintf(label, sizeof(label), "%s##fol%d",
                         g_scene.foliage[i].name[0] ? g_scene.foliage[i].name : "(unnamed)", i);
                if (ImGui::Selectable(label, g_selectedFoliage == i))
                    g_selectedFoliage = i;
            }

            ImGui::Spacing();
            if (ImGui::Button("Add Foliage Layer") &&
                g_scene.foliageCount < BRUSH_SCENE_MAX_FOLIAGE) {
                BrushSceneFoliageLayer *fl = &g_scene.foliage[g_scene.foliageCount];
                memset(fl, 0, sizeof(*fl));
                snprintf(fl->name, sizeof(fl->name), "layer%d", g_scene.foliageCount);
                fl->density = 1.0f; fl->drawDistance = 60.0f; fl->lodDistance = 24.0f;
                fl->scale = 1.0f; fl->scaleJitter = 0.3f; fl->maxSlopeDeg = 42.0f;
                fl->windStrength = 1.0f; fl->farKeepRatio = 0.4f; fl->tint = (Vector3){1, 1, 1};
                g_selectedFoliage = g_scene.foliageCount++;
                g_dirty = true; g_foliageResyncPending = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(live: edits re-scatter the grass)");

            if (g_selectedFoliage >= 0 && g_selectedFoliage < g_scene.foliageCount) {
                ImGui::Separator();
                BrushSceneFoliageLayer *fl = &g_scene.foliage[g_selectedFoliage];
                int nc = 0, fc = 0;
                BrushFoliageLayerStats(g_foliage, g_selectedFoliage, &nc, &fc);
                ImGui::Text("Visible: %d near / %d far  (%d draw call%s)", nc, fc,
                            (nc > 0) + (fc > 0), ((nc > 0) + (fc > 0)) == 1 ? "" : "s");

                ImGui::InputText("Name", fl->name, sizeof(fl->name));
                if (ImGui::IsItemDeactivatedAfterEdit()) g_dirty = true;

                // Model / albedo paths (empty -> procedural tuft/gradient).
                // Path changes need a re-resolve, so they take the resync path.
                ImGui::InputText("Model (.glb)", fl->model, sizeof(fl->model));
                if (ImGui::IsItemDeactivatedAfterEdit()) { g_dirty = true; g_foliageResyncPending = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty = procedural tuft.");
                ImGui::InputText("Albedo", fl->albedo, sizeof(fl->albedo));
                if (ImGui::IsItemDeactivatedAfterEdit()) { g_dirty = true; g_foliageResyncPending = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty = procedural gradient card.");

                ImGui::SeparatorText("Placement");
                if (ImGui::SliderFloat("Density", &fl->density, 0.05f, 6.0f, "%.2f /m2")) { g_dirty = true; g_foliageResyncPending = true; }
                if (ImGui::SliderFloat("Scale", &fl->scale, 0.1f, 5.0f, "%.2f")) { g_dirty = true; g_foliageResyncPending = true; }
                if (ImGui::SliderFloat("Scale Jitter", &fl->scaleJitter, 0.0f, 1.0f, "%.2f")) { g_dirty = true; g_foliageResyncPending = true; }
                if (ImGui::SliderFloat("Height Offset", &fl->heightOffset, -0.5f, 0.5f, "%.2f m")) { g_dirty = true; g_foliageResyncPending = true; }
                if (ImGui::SliderFloat("Max Slope", &fl->maxSlopeDeg, 0.0f, 90.0f, fl->maxSlopeDeg < 0.5f ? "any" : "%.0f deg")) { g_dirty = true; g_foliageResyncPending = true; }

                ImGui::SeparatorText("Distances (overdraw lever)");
                if (ImGui::SliderFloat("Draw Distance", &fl->drawDistance, 10.0f, 200.0f, "%.0f m")) { g_dirty = true; g_foliageRebuildPending = true; }
                if (fl->drawDistance > 120.0f) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(!)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("High draw distance = heavy grass overdraw at 4x retina "
                                          "(the donor's thermal-throttle lever). Keep modest.");
                }
                if (ImGui::SliderFloat("LOD Distance", &fl->lodDistance, 5.0f, 150.0f, "%.0f m")) { g_dirty = true; g_foliageRebuildPending = true; }
                if (ImGui::SliderFloat("Far LOD Keep", &fl->farKeepRatio, 0.05f, 1.0f, fl->farKeepRatio > 0.99f ? "no far LOD" : "%.2f")) { g_dirty = true; g_foliageRebuildPending = true; }

                ImGui::SeparatorText("Look");
                if (ImGui::SliderFloat("Wind", &fl->windStrength, 0.0f, 3.0f, "%.2f")) { g_dirty = true; g_foliageRebuildPending = true; }
                if (ImGui::ColorEdit3("Tint", (float *)&fl->tint)) { g_dirty = true; g_foliageRebuildPending = true; }
                if (ImGui::ColorEdit3("Macro Low", (float *)&fl->macroLow)) { g_dirty = true; g_foliageRebuildPending = true; }
                if (ImGui::ColorEdit3("Macro High", (float *)&fl->macroHigh)) { g_dirty = true; g_foliageRebuildPending = true; }

                ImGui::Spacing();
                if (ImGui::Button("Duplicate Layer") &&
                    g_scene.foliageCount < BRUSH_SCENE_MAX_FOLIAGE) {
                    g_scene.foliage[g_scene.foliageCount] = *fl;
                    g_scene.foliage[g_scene.foliageCount].modelRes = (Model){0};
                    g_scene.foliage[g_scene.foliageCount].albedoTex = (Texture2D){0};
                    g_selectedFoliage = g_scene.foliageCount++;
                    g_dirty = true; g_foliageResyncPending = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete Layer")) {
                    for (int fi = g_selectedFoliage; fi < g_scene.foliageCount - 1; fi++)
                        g_scene.foliage[fi] = g_scene.foliage[fi + 1];
                    g_scene.foliageCount--;
                    g_selectedFoliage = -1;
                    g_dirty = true; g_foliageResyncPending = true;
                }
            }
        }

        ImGui::End();

        // === Console =========================================================
        ImGui::Begin("Console");
        ImGuiID consoleDockId = ImGui::GetWindowDockID();
        if (ImGui::SmallButton("Clear")) g_logLineCount = 0;
        ImGui::Separator();
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None);
        for (int i = 0; i < g_logLineCount; i++) ImGui::TextUnformatted(g_logLines[i]);
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();

        // === Assets ==========================================================
        // Project file browser (Godot's FileSystem dock): everything under
        // assets/. Selecting a texture shows its preview and IMPORT
        // SETTINGS — how that file cooks into the GPU cache (max size,
        // BC compression, mipmaps, normal profile). Editing writes the
        // .import sidecar; the live watch re-cooks and the viewport
        // updates. Materials reference these files; import settings are
        // per FILE, so every material sharing a texture is affected.
        if (consoleDockId != 0)
            ImGui::SetNextWindowDockID(consoleDockId, ImGuiCond_FirstUseEver);
        ImGui::Begin("Assets");
        {
            static int selAsset = -1;
            static BrushTexImportParams impParams;
            if (selAsset >= g_assetFileCount) selAsset = -1;

            if (ImGui::Button("Import Textures...")) {
#if defined(__APPLE__)
                static char picked[8192];
                int n = EditorMacOpenImageDialog(picked, sizeof(picked));
                if (n > 0) {
                    const char *files[64];
                    int count = 0;
                    for (char *tok = strtok(picked, "\n");
                         tok != NULL && count < 64; tok = strtok(NULL, "\n"))
                        files[count++] = tok;
                    ImportTextureFiles(files, count);
                }
#else
                AddEditorLog("Drop image files onto the window to import.");
#endif
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                RescanAssetFiles();
                RescanTextureDir();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("drag & drop images anywhere to import");
            ImGui::Separator();

            // Left: file list. Right: preview + per-file import settings.
            float listW = ImGui::GetContentRegionAvail().x * 0.45f;
            ImGui::BeginChild("##assetlist", ImVec2(listW, 0));
            DrawFolderContents("", selAsset, impParams);
            if (g_assetFileCount == 0)
                ImGui::TextDisabled("No files yet — import textures above.");
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##assetdetail", ImVec2(0, 0));
            if (selAsset >= 0 && selAsset < g_assetFileCount) {
                const char *path = g_assetFiles[selAsset];
                ImGui::TextUnformatted(GetFileName(path));
                ImGui::TextDisabled("%s  (%d KB)", path,
                                    GetFileLength(path) / 1024);
                ImGui::Spacing();
                if (IsImageAsset(path)) {
                    if (g_assetPreview.id != 0) {
                        float side = fminf(
                            ImGui::GetContentRegionAvail().x * 0.5f, 140.0f);
                        ImGui::Image((ImTextureID)(intptr_t)g_assetPreview.id,
                                     ImVec2(side, side));
                        ImGui::SameLine();
                        ImGui::TextDisabled("%dx%d\n%d mips\ncooked",
                                            g_assetPreview.width,
                                            g_assetPreview.height,
                                            g_assetPreview.mipmaps);
                    }
                    ImGui::SeparatorText("Import Settings");
                    bool edited = false;
                    const int sizes[] = { 0, 256, 512, 1024, 2048, 4096 };
                    int sizeIdx = 0;
                    for (int i = 0; i < 6; i++)
                        if (impParams.maxSize == sizes[i]) sizeIdx = i;
                    if (ImGui::Combo("Max Size", &sizeIdx,
                                     "no limit\0 256\0 512\0 1024\0 2048\0 4096\0")) {
                        impParams.maxSize = sizes[sizeIdx];
                        edited = true;
                    }
                    int compIdx = (strcmp(impParams.compress, "bc1") == 0) ? 1
                                  : (strcmp(impParams.compress, "bc3") == 0) ? 2 : 0;
                    if (ImGui::Combo("Compression", &compIdx,
                                     "none (RGBA8)\0BC1 (opaque, 8:1)\0BC3 (alpha/normal, 4:1)\0")) {
                        snprintf(impParams.compress, sizeof(impParams.compress),
                                 compIdx == 1 ? "bc1" : compIdx == 2 ? "bc3" : "none");
                        edited = true;
                    }
                    edited |= ImGui::Checkbox("Mipmaps", &impParams.mipmaps);
                    edited |= ImGui::Checkbox("Normal map (DXT5nm)",
                                              &impParams.isNormalMap);
                    if (edited && BrushAssetsSetImportParams(path, &impParams))
                        AddEditorLog("Import settings saved: %s (re-importing)",
                                     GetFileName(path));
                } else if (IsModelAsset(path)) {
                    ImGui::TextDisabled("3D model");
                    ImGui::TextWrapped(
                        "Drag this file into the viewport to place it in "
                        "the scene.");
                } else {
                    ImGui::TextDisabled("No import options for this file type.");
                }
            } else {
                ImGui::TextDisabled("Select a file to see its details.");
            }
            ImGui::EndChild();
        }
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
            bool imageDropTarget = ImGui::BeginDragDropTarget();

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

            // --- Drop a model from the Assets panel onto the scene -------------
            // Raycast the drop point; land the instance where the cursor hits
            // (or on a ground-plane fallback when nothing is under it).
            if (imageDropTarget) {
                if (const ImGuiPayload *payload =
                        ImGui::AcceptDragDropPayload("BRUSH_MODEL")) {
                    const char *droppedPath = (const char *)payload->Data;
                    Ray ray;
                    Vector3 spawn = { 0, 0, 0 };
                    if (mouseToRay(GetMousePosition(), &ray)) {
                        Vector3 hitPt;
                        if (BrushPhysicsRaycastEx(&g_phys, ray.position,
                                                  ray.direction, 1000.0f,
                                                  &hitPt, NULL, NULL)) {
                            spawn = hitPt;
                        } else if (fabsf(ray.direction.y) > 0.0001f) {
                            float t = -ray.position.y / ray.direction.y;
                            if (t > 0)
                                spawn = Vector3Add(
                                    ray.position,
                                    Vector3Scale(ray.direction, t));
                        }
                    }
                    AddModelEntityAt(droppedPath, spawn);
                }
                ImGui::EndDragDropTarget();
            }

            // --- Gizmo (select mode only) -------------------------------------
            // Roads are edited in the Sculpt Road tool (node gizmo below), not
            // the select gizmo — skip them here so it doesn't sit at the origin.
            if (g_mode == MODE_SELECT && g_selectedType != ENTITY_NONE &&
                g_selectedType != ENTITY_ROAD) {
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
                else if (g_selectedType == ENTITY_MODEL) {
                    // Pure instance TRS for the gizmo (the model's base
                    // transform stays out so decompose round-trips clean).
                    BrushSceneModelInstance *mi = &g_scene.models[g_selectedIdx];
                    modelMat = MatrixScale(mi->scale.x, mi->scale.y, mi->scale.z);
                    modelMat = MatrixMultiply(modelMat, BrushEulerXYZ(mi->rot));
                    modelMat = MatrixMultiply(modelMat,
                                              MatrixTranslate(mi->pos.x, mi->pos.y, mi->pos.z));
                }
                else if (g_selectedType == ENTITY_SPAWN)
                    modelMat = MatrixTranslate(g_scene.spawn.x, g_scene.spawn.y, g_scene.spawn.z);

                Matrix viewT = MatrixTranspose(viewMat);
                Matrix projT = MatrixTranspose(projMat);
                Matrix modelT = MatrixTranspose(modelMat);

                ImGuizmo::Manipulate((float *)&viewT, (float *)&projT,
                                     (g_selectedType == ENTITY_BLOCK ||
                                      g_selectedType == ENTITY_MODEL)
                                         ? g_gizmoOp
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
                if (usingGizmo && !wasUsingGizmo && g_selectedType == ENTITY_MODEL) {
                    for (int j = 0; j < g_modelBodyN[g_selectedIdx]; j++)
                        BrushPhysicsRemoveBody(&g_phys,
                                               g_modelBodies[g_selectedIdx][j]);
                    g_modelBodyN[g_selectedIdx] = 0;
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
                            } else { // models + spawn sit on the surface
                                newPos = hitPt;
                            }
                        }
                    }

                    if (g_selectedType == ENTITY_BLOCK) {
                        BrushSceneBlock *b = &g_scene.blocks[g_selectedIdx];
                        b->pos = newPos;
                        if (g_gizmoOp == ImGuizmo::ROTATE) b->rot = newRot;
                        if (g_gizmoOp == ImGuizmo::SCALE) b->size = newScale;
                    } else if (g_selectedType == ENTITY_MODEL) {
                        BrushSceneModelInstance *mi = &g_scene.models[g_selectedIdx];
                        mi->pos = newPos;
                        if (g_gizmoOp == ImGuizmo::ROTATE) mi->rot = newRot;
                        if (g_gizmoOp == ImGuizmo::SCALE) mi->scale = newScale;
                    } else if (g_selectedType == ENTITY_LIGHT) {
                        g_scene.lights[g_selectedIdx].light.position = newPos;
                    } else if (g_selectedType == ENTITY_SPAWN) {
                        g_scene.spawn = newPos;
                    }
                    g_dirty = true;
                }
            }

            // --- Road Node Gizmo (sculpt mode only, road tool active, node selected) ---
            if (g_mode == MODE_SCULPT && g_roadActive &&
                g_selectedRoadIdx >= 0 && g_selectedRoadIdx < g_scene.roadCount &&
                g_selectedNodeIdx >= 0 && g_selectedNodeIdx < g_scene.roads[g_selectedRoadIdx].pointCount) {
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(imgPos.x, imgPos.y, imgSize.x, imgSize.y);

                Matrix viewMat = GetCameraMatrix(g_camera.cam);
                Matrix projMat = MatrixPerspective(g_camera.cam.fovy * DEG2RAD,
                                                   renderAspect, 0.01f, 1000.0f);
                BrushSceneRoad *r = &g_scene.roads[g_selectedRoadIdx];
                Vector3 *node = &r->points[g_selectedNodeIdx];
                Matrix modelMat = MatrixTranslate(node->x, node->y, node->z);

                Matrix viewT = MatrixTranspose(viewMat);
                Matrix projT = MatrixTranspose(projMat);
                Matrix modelT = MatrixTranspose(modelMat);

                ImGuizmo::Manipulate((float *)&viewT, (float *)&projT,
                                     ImGuizmo::TRANSLATE, ImGuizmo::WORLD, (float *)&modelT);

                if (ImGuizmo::IsUsing()) {
                    modelMat = MatrixTranspose(modelT);
                    node->x = modelMat.m12;
                    node->y = modelMat.m13;
                    node->z = modelMat.m14;
                    g_dirty = true;
                    g_roadResyncPending = true; // re-bakes on release (idle)
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
                if (g_cursorOnGround && !g_roadActive) {
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        PushSculptUndo();     // stroke-level undo record
                        g_sculptStroking = true;
                        g_flattenY = g_cursorPos.y; // flatten to press height
                    }
                    if (g_sculptStroking && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                        float dtStroke = GetFrameTime();
                        bool lower = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

                        // Build constraints
                        BrushConstraints bc = {0};
                        bc.targetLayer = -1;
                        bool hasBC = false;
                        if (g_useSlopeFilter) {
                            bc.checkSlope = true;
                            bc.minCosSlope = cosf(g_constrainSlopeDegMax * DEG2RAD);
                            bc.maxCosSlope = cosf(g_constrainSlopeDegMin * DEG2RAD);
                            hasBC = true;
                        }
                        if (g_useHeightFilter) {
                            bc.checkHeight = true;
                            bc.minHeight = g_constrainHeightMin;
                            bc.maxHeight = g_constrainHeightMax;
                            hasBC = true;
                        }
                        if (g_constrainLayerIdx >= 0) {
                            bc.targetLayer = g_constrainLayerIdx;
                            hasBC = true;
                        }
                        const BrushConstraints *pBc = hasBC ? &bc : NULL;

                        if (g_paintActive) {
                            // shift paints the base coat back (erase).
                            int layer = lower ? 0 : g_paintLayer;
                            float flow = fminf(g_brushStrength * 4.0f * dtStroke, 1.0f);
                            BrushWorldPaintC(g_world, g_cursorPos, g_brushRadius,
                                             flow, layer, pBc);
                        } else {
                            float strength;
                            if (g_sculptOp == BRUSH_SCULPT_ADD)
                                strength = (lower ? -1.0f : 1.0f) * g_brushStrength * 6.0f * dtStroke;
                            else
                                strength = fminf(g_brushStrength * 8.0f * dtStroke, 1.0f);
                            BrushWorldSculptC(g_world, g_sculptOp, g_cursorPos,
                                              g_brushRadius, strength, g_flattenY, pBc);
                        }
                        g_dirty = true;
                    }
                }

                if (g_cursorOnGround && g_roadActive) {
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
                        // Click a node of ANY road to select that road + node
                        // (so roads are viewport-selectable, not just via the
                        // panel list). Nearest hit across all roads wins.
                        float closestDist = 1e9f;
                        int hitRoad = -1, hitNode = -1;
                        for (int ri = 0; ri < g_scene.roadCount; ri++) {
                            BrushSceneRoad *road = &g_scene.roads[ri];
                            for (int ni = 0; ni < road->pointCount; ni++) {
                                RayCollision col = GetRayCollisionSphere(ray, road->points[ni], 0.5f);
                                if (col.hit && col.distance < closestDist) {
                                    closestDist = col.distance;
                                    hitRoad = ri;
                                    hitNode = ni;
                                }
                            }
                        }

                        if (hitRoad >= 0) {
                            g_selectedRoadIdx = hitRoad;
                            g_selectedNodeIdx = hitNode;
                        } else {
                            // Empty click: append a node to the selected road
                            // (creating one if none is selected).
                            if (g_selectedRoadIdx < 0 || g_selectedRoadIdx >= g_scene.roadCount) {
                                if (g_scene.roadCount < BRUSH_SCENE_MAX_ROADS) {
                                    g_selectedRoadIdx = g_scene.roadCount++;
                                    NewRoad(&g_scene.roads[g_selectedRoadIdx]);
                                    AddEditorLog("Created new road spline %d", g_selectedRoadIdx);
                                }
                            }
                            if (g_selectedRoadIdx >= 0 && g_selectedRoadIdx < g_scene.roadCount) {
                                BrushSceneRoad *r = &g_scene.roads[g_selectedRoadIdx];
                                if (r->pointCount < BRUSH_ROAD_MAX_POINTS) {
                                    r->points[r->pointCount++] = g_cursorPos;
                                    g_selectedNodeIdx = r->pointCount - 1;
                                    g_dirty = true;
                                    g_roadResyncPending = true;
                                    AddEditorLog("Added node %d to road %d", g_selectedNodeIdx, g_selectedRoadIdx);
                                }
                            }
                        }
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
                        for (int i = 0; i < g_scene.modelCount && hitIdx < 0; i++)
                            for (int j = 0; j < g_modelBodyN[i]; j++)
                                if (g_modelBodies[i][j] == hitBody) {
                                    closest = Vector3Distance(ray.position, hitPt);
                                    hitType = ENTITY_MODEL;
                                    hitIdx = i;
                                    break;
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
                if (ToolButton(" Raise ", !g_paintActive && !g_roadActive && g_sculptOp == BRUSH_SCULPT_ADD)) {
                    g_sculptOp = BRUSH_SCULPT_ADD;
                    g_paintActive = false;
                    g_roadActive = false;
                }
                ImGui::SameLine();
                if (ToolButton(" Smooth ", !g_paintActive && !g_roadActive && g_sculptOp == BRUSH_SCULPT_SMOOTH)) {
                    g_sculptOp = BRUSH_SCULPT_SMOOTH;
                    g_paintActive = false;
                    g_roadActive = false;
                }
                ImGui::SameLine();
                if (ToolButton(" Flatten ", !g_paintActive && !g_roadActive && g_sculptOp == BRUSH_SCULPT_FLATTEN)) {
                    g_sculptOp = BRUSH_SCULPT_FLATTEN;
                    g_paintActive = false;
                    g_roadActive = false;
                }
                ImGui::SameLine();
                if (ToolButton(" Paint ", g_paintActive && !g_roadActive)) {
                    g_paintActive = true;
                    g_roadActive = false;
                }
                ImGui::SameLine();
                if (ToolButton(" Road ", g_roadActive && !g_paintActive)) {
                    g_roadActive = true;
                    g_paintActive = false;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                ImGui::SliderFloat("Radius", &g_brushRadius, 1.0f, 60.0f, "%.0f m");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                ImGui::SliderFloat("Strength", &g_brushStrength, 0.1f, 4.0f, "%.1f");
                // Paint tool: one swatch per configured layer (albedo
                // thumbnails via the material library); click to pick the
                // painted layer, shift-drag paints layer 0 back (erase).
                if (g_paintActive) {
                    BrushTerrainLayer tl[BRUSH_TERRAIN_LAYERS];
                    int tlCount = BrushSceneTerrainLayers(&g_scene, tl);
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    if (tlCount == 0) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1, 0.65f, 0.25f, 1),
                                           "set Terrain Layers in Environment");
                    }
                    for (int i = 0; i < tlCount; i++) {
                        ImGui::SameLine();
                        ImGui::PushID(i);
                        bool sel = (g_paintLayer == i);
                        if (sel)
                            ImGui::PushStyleColor(ImGuiCol_Button,
                                                  ImVec4(0.25f, 0.5f, 0.85f, 1));
                        if (tl[i].albedo.id != 0) {
                            if (ImGui::ImageButton("##sw",
                                    (ImTextureID)(intptr_t)tl[i].albedo.id,
                                    ImVec2(22, 22)))
                                g_paintLayer = i;
                        } else if (ImGui::Button(TextFormat("%d", i),
                                                 ImVec2(28, 28))) {
                            g_paintLayer = i;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", TerrainSlotLabel(i));
                        if (sel) ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    if (g_paintLayer >= tlCount) g_paintLayer = tlCount > 0 ? tlCount - 1 : 0;
                }
            }
            ImGui::EndGroup();

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

        // Live roads: re-bake the terrain once the user stops dragging (a
        // slider/gizmo still active defers it, so we don't rebake every frame).
        if (g_roadResyncPending && !ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) {
            SyncRoadsToWorld();
            g_roadResyncPending = false;
        }

        // Live foliage: once the user stops dragging, either re-scatter (a
        // placement change) or just rebuild the layers (a draw-only change like
        // tint/wind — instances are untouched, so skip the costly world rebake).
        if (!ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing()) {
            if (g_foliageResyncPending) SyncFoliageToWorld();
            else if (g_foliageRebuildPending) BuildFoliageLayers();
            g_foliageResyncPending = false;
            g_foliageRebuildPending = false;
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
    BrushSceneUnloadMaterials(&g_scene);
    BrushAssetsShutdown();
    UnloadModel(g_spawnMarker);
    BrushWorldDestroy(g_world);   // frees chunk foliage handles via the free hook
    BrushFoliageDestroy(g_foliage);
    BrushPhysicsCleanup(&g_phys);
    BrushRenderShutdown();
    CloseWindow();
    return 0;
}
