/*******************************************************************************************
 *   b_scene.c - World definition files (see b_scene.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_scene.h"
#include "b_assets.h"
#include "b_post.h"
#include "b_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <raymath.h>

// "-" means "no path/name" in world.def fields (sscanf can't skip words).
static void CopyField(char *dst, int cap, const char *src) {
  if (strcmp(src, "-") == 0) { dst[0] = '\0'; return; }
  strncpy(dst, src, (size_t)cap - 1);
  dst[cap - 1] = '\0';
}

bool BrushSceneLoad(BrushScene *s, const char *path) {
  BrushScene temp = {0};
  temp.timeHours = -1.0f;
  strncpy(temp.path, path, BRUSH_SCENE_PATH_MAX - 1);
  temp.path[BRUSH_SCENE_PATH_MAX - 1] = '\0';

  // LoadFileText (not fopen) so a mounted release pak serves the scene.
  char *text = LoadFileText(path);
  if (text == NULL) return false;

  char *cursor = text;
  int lineNo = 0;
  while (*cursor != '\0') {
    char *line = cursor;
    char *nl = strchr(cursor, '\n');
    if (nl != NULL) { *nl = '\0'; cursor = nl + 1; }
    else cursor += strlen(cursor);
    lineNo++;
    // Strip leading whitespace; skip blanks and comments.
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '#') continue;

    int version;
    float x, y, z, h, a, rx, ry, rz, sx, sy, sz, r, g, b, radius;
    int ir, ig, ib, flicker, n;
    char w1[256], w2[256], w3[256], w4[256], w5[256];

    if (sscanf(p, "version %d", &version) == 1) {
      if (version < 1 || version > 3)
        TraceLog(LOG_WARNING,
                 "BRUSH scene: %s is version %d (engine reads 1-3)", path,
                 version);
    } else if (sscanf(p, "spawn %f %f %f", &x, &y, &z) == 3) {
      temp.spawn = (Vector3){x, y, z};
    } else if (sscanf(p, "time %f", &x) == 1) {
      temp.timeHours = x;
    } else if ((n = sscanf(p,
                           "material %31s %255s %255s %255s %255s %f %f %f %f %f %d",
                           w1, w2, w3, w4, w5, &x, &y, &z, &h, &a,
                           &flicker)) >= 10) {
      if (temp.materialCount < BRUSH_SCENE_MAX_MATERIALS) {
        BrushSceneMaterial *m = &temp.materials[temp.materialCount++];
        memset(m, 0, sizeof(*m));
        CopyField(m->name, sizeof(m->name), w1);
        CopyField(m->albedo, sizeof(m->albedo), w2);
        CopyField(m->normal, sizeof(m->normal), w3);
        CopyField(m->displacement, sizeof(m->displacement), w4);
        CopyField(m->ao, sizeof(m->ao), w5);
        m->tile = (x > 0.01f) ? x : 1.0f;
        m->spec = y;
        m->normalDepth = z;
        m->heightScale = h;
        m->aoStrength = a;
        m->uvProjection = (n == 11 && flicker != 0);
      }
    } else if (sscanf(p, "material %31s %255s %255s %f %f %f", w1, w2, w3, &x,
                      &y, &z) == 6) {
      if (temp.materialCount < BRUSH_SCENE_MAX_MATERIALS) {
        BrushSceneMaterial *m = &temp.materials[temp.materialCount++];
        memset(m, 0, sizeof(*m));
        CopyField(m->name, sizeof(m->name), w1);
        CopyField(m->albedo, sizeof(m->albedo), w2);
        CopyField(m->normal, sizeof(m->normal), w3);
        m->tile = (x > 0.01f) ? x : 1.0f;
        m->spec = y;
        m->normalDepth = z;
        m->heightScale = 0.05f;
        m->aoStrength = 1.0f;
      }
    } else if (sscanf(p, "terrain_layer %d %31s", &flicker, w1) == 2) {
      if (flicker >= 0 && flicker < BRUSH_TERRAIN_LAYERS)
        CopyField(temp.terrainLayers[flicker],
                  sizeof(temp.terrainLayers[0]), w1);
    } else if (sscanf(p, "post %23s %f", w1, &x) == 2) {
      if (temp.postCount < BRUSH_SCENE_MAX_POST) {
        BrushScenePostSetting *ps = &temp.post[temp.postCount++];
        memset(ps, 0, sizeof(*ps));
        CopyField(ps->key, sizeof(ps->key), w1);
        ps->value = x;
      }
    } else if ((n = sscanf(p, "block %f %f %f %f %f %f %f %f %f %d %d %d %31s",
                           &x, &y, &z, &rx, &ry, &rz, &sx, &sy, &sz, &ir, &ig,
                           &ib, w1)) >= 12) {
      if (temp.blockCount < BRUSH_SCENE_MAX_BLOCKS) {
        BrushSceneBlock *k = &temp.blocks[temp.blockCount++];
        *k = (BrushSceneBlock){
            .pos = {x, y, z},
            .size = {sx, sy, sz},
            .rot = {rx, ry, rz},
            .color = {(unsigned char)ir, (unsigned char)ig, (unsigned char)ib, 255},
        };
        if (n == 13) CopyField(k->material, sizeof(k->material), w1);
      }
    } else if (sscanf(p, "block %f %f %f %f %f %f %d %d %d", &x, &y, &z, &sx, &sy, &sz, &ir, &ig, &ib) == 9) {
      if (temp.blockCount < BRUSH_SCENE_MAX_BLOCKS) {
        temp.blocks[temp.blockCount++] = (BrushSceneBlock){
            .pos = {x, y, z},
            .size = {sx, sy, sz},
            .rot = {0, 0, 0},
            .color = {(unsigned char)ir, (unsigned char)ig, (unsigned char)ib, 255},
        };
      }
    } else if ((n = sscanf(p, "model %255s %f %f %f %f %f %f %f %f %f %31s",
                           w2, &x, &y, &z, &rx, &ry, &rz, &sx, &sy, &sz,
                           w1)) >= 7) {
      if (temp.modelCount < BRUSH_SCENE_MAX_MODELS) {
        BrushSceneModelInstance *mi = &temp.models[temp.modelCount++];
        memset(mi, 0, sizeof(*mi));
        CopyField(mi->path, sizeof(mi->path), w2);
        mi->pos = (Vector3){x, y, z};
        mi->rot = (Vector3){rx, ry, rz};
        mi->scale = (n >= 10) ? (Vector3){sx, sy, sz} : (Vector3){1, 1, 1};
        if (n == 11) CopyField(mi->material, sizeof(mi->material), w1);
      }
    } else if (sscanf(p, "light %f %f %f %f %f %f %f %d", &x, &y, &z, &r, &g,
                      &b, &radius, &flicker) == 8) {
      if (temp.lightCount < BRUSH_SCENE_MAX_LIGHTS) {
        temp.lights[temp.lightCount++] = (BrushSceneLight){
            .light = {.position = {x, y, z},
                      .color = {r, g, b},
                      .radius = radius},
            .flicker = (flicker != 0),
        };
      }
    } else {
      // Forward compatibility: unknown entity types are skipped, not fatal.
      TraceLog(LOG_WARNING, "BRUSH scene: %s:%d unknown line: %.40s", path,
               lineNo, p);
    }
  }
  UnloadFileText(text);

  temp.modTime = GetFileModTime(path);
  BrushSceneUnloadMaterials(s); // drop the outgoing scene's texture refs
  *s = temp;
  BrushSceneResolveMaterials(s);
  TraceLog(LOG_INFO,
           "BRUSH scene: loaded %s (%d blocks, %d lights, %d materials)",
           path, s->blockCount, s->lightCount, s->materialCount);
  return true;
}

bool BrushSceneSave(BrushScene *s, const char *path) {
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    TraceLog(LOG_WARNING, "BRUSH scene: cannot write %s", path);
    return false;
  }
  fprintf(f, "# brush world definition (see engine/b_scene.h)\n");
  fprintf(f, "version 3\n\n");
  fprintf(f, "spawn %g %g %g\n", s->spawn.x, s->spawn.y, s->spawn.z);
  if (s->timeHours >= 0.0f) fprintf(f, "time %g\n", s->timeHours);
  if (s->materialCount > 0)
    fprintf(f, "\n# material  name  albedo  normal  displacement  ao  tile spec depth scale aoStrength\n");
  for (int i = 0; i < s->materialCount; i++) {
    const BrushSceneMaterial *m = &s->materials[i];
    fprintf(f, "material %s %s %s %s %s %g %g %g %g %g %d\n", m->name,
            m->albedo[0] ? m->albedo : "-", m->normal[0] ? m->normal : "-",
            m->displacement[0] ? m->displacement : "-", m->ao[0] ? m->ao : "-",
            m->tile, m->spec, m->normalDepth, m->heightScale, m->aoStrength,
            m->uvProjection ? 1 : 0);
  }
  fprintf(f, "\n# block  x y z  rx ry rz  sx sy sz  r g b  material\n");
  for (int i = 0; i < s->blockCount; i++) {
    const BrushSceneBlock *k = &s->blocks[i];
    fprintf(f, "block %g %g %g  %g %g %g  %g %g %g  %d %d %d  %s\n", k->pos.x,
            k->pos.y, k->pos.z, k->rot.x, k->rot.y, k->rot.z, k->size.x,
            k->size.y, k->size.z, k->color.r, k->color.g, k->color.b,
            k->material[0] ? k->material : "-");
  }
  if (s->modelCount > 0)
    fprintf(f, "\n# model  path  x y z  rx ry rz  sx sy sz  material\n");
  for (int i = 0; i < s->modelCount; i++) {
    const BrushSceneModelInstance *mi = &s->models[i];
    fprintf(f, "model %s  %g %g %g  %g %g %g  %g %g %g  %s\n", mi->path,
            mi->pos.x, mi->pos.y, mi->pos.z, mi->rot.x, mi->rot.y, mi->rot.z,
            mi->scale.x, mi->scale.y, mi->scale.z,
            mi->material[0] ? mi->material : "-");
  }
  fprintf(f, "\n# light  x y z  r g b (linear)  radius  flicker\n");
  for (int i = 0; i < s->lightCount; i++) {
    const BrushSceneLight *l = &s->lights[i];
    fprintf(f, "light %g %g %g  %g %g %g  %g %d\n", l->light.position.x,
            l->light.position.y, l->light.position.z, l->light.color.x,
            l->light.color.y, l->light.color.z, l->light.radius,
            l->flicker ? 1 : 0);
  }
  bool anyLayer = false;
  for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++)
    if (s->terrainLayers[i][0] != '\0') anyLayer = true;
  if (anyLayer) fprintf(f, "\n# terrain_layer  slot  material\n");
  for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++)
    if (s->terrainLayers[i][0] != '\0')
      fprintf(f, "terrain_layer %d %s\n", i, s->terrainLayers[i]);
  if (s->postCount > 0) fprintf(f, "\n# post  key value (render tunables)\n");
  for (int i = 0; i < s->postCount; i++)
    fprintf(f, "post %s %g\n", s->post[i].key, s->post[i].value);
  fclose(f);
  strncpy(s->path, path, BRUSH_SCENE_PATH_MAX - 1);
  s->modTime = GetFileModTime(path);
  TraceLog(LOG_INFO, "BRUSH scene: saved %s (%d blocks, %d lights)", path,
           s->blockCount, s->lightCount);
  return true;
}

bool BrushSceneHotReload(BrushScene *s) {
  if (s->path[0] == '\0') return false;
  long mt = GetFileModTime(s->path);
  if (getenv("BRUSH_SCENE_DBG") != NULL)
    TraceLog(LOG_INFO, "SCENEDBG path=%s mt=%ld have=%ld", s->path, mt,
             s->modTime);
  if (mt == s->modTime || mt == 0) return false;
  char path[BRUSH_SCENE_PATH_MAX];
  strncpy(path, s->path, sizeof(path));
  path[sizeof(path) - 1] = '\0';
  if (!BrushSceneLoad(s, path)) return false;
  TraceLog(LOG_INFO, "BRUSH scene: hot-reloaded %s", path);
  return true;
}

// Euler (degrees) -> rotation matrix, composed X then Y then Z in raylib's
// row-vector convention. This is EXACTLY ImGuizmo's Recompose/Decompose
// order — the editor gizmo, this render matrix, and the physics collider
// (QuaternionFromMatrix of the same rotation in b_physics) must all agree
// or blocks visibly jump when a gizmo drag ends.
Matrix BrushEulerXYZ(Vector3 degrees) {
  Matrix r = MatrixMultiply(MatrixRotateX(degrees.x * DEG2RAD),
                            MatrixRotateY(degrees.y * DEG2RAD));
  return MatrixMultiply(r, MatrixRotateZ(degrees.z * DEG2RAD));
}

Matrix BrushBlockGetModelMatrix(const BrushSceneBlock *k) {
  Matrix xf = MatrixScale(k->size.x, k->size.y, k->size.z);
  xf = MatrixMultiply(xf, BrushEulerXYZ(k->rot));
  return MatrixMultiply(xf, MatrixTranslate(k->pos.x, k->pos.y, k->pos.z));
}

Matrix BrushModelInstanceMatrix(const BrushSceneModelInstance *m) {
  // The model's authored base transform (e.g. glTF axis conversion) applies
  // FIRST, then the instance's scale/rotation/translation. The renderer's
  // draw path REPLACES model.transform with the submitted matrix, so the
  // base transform must be part of it or it would be lost.
  Matrix xf = m->model.transform;
  xf = MatrixMultiply(xf, MatrixScale(m->scale.x, m->scale.y, m->scale.z));
  xf = MatrixMultiply(xf, BrushEulerXYZ(m->rot));
  return MatrixMultiply(xf, MatrixTranslate(m->pos.x, m->pos.y, m->pos.z));
}

// --- Materials ---------------------------------------------------------------

int BrushSceneFindMaterial(const BrushScene *s, const char *name) {
  if (name == NULL || name[0] == '\0') return -1;
  for (int i = 0; i < s->materialCount; i++)
    if (strcmp(s->materials[i].name, name) == 0) return i;
  return -1;
}

void BrushSceneUnloadMaterials(BrushScene *s) {
  // Guard against a never-initialized struct (first Load into stack storage).
  if (s->materialCount < 0 || s->materialCount > BRUSH_SCENE_MAX_MATERIALS) {
    s->materialCount = 0;
    return;
  }
  for (int i = 0; i < s->materialCount; i++) {
    BrushAssetsReleaseTexture(s->materials[i].albedoTex);
    BrushAssetsReleaseTexture(s->materials[i].normalTex);
    BrushAssetsReleaseTexture(s->materials[i].displacementTex);
    BrushAssetsReleaseTexture(s->materials[i].aoTex);
    s->materials[i].albedoTex = (Texture2D){0};
    s->materials[i].normalTex = (Texture2D){0};
    s->materials[i].displacementTex = (Texture2D){0};
    s->materials[i].aoTex = (Texture2D){0};
  }
  if (s->modelCount < 0 || s->modelCount > BRUSH_SCENE_MAX_MODELS) {
    s->modelCount = 0;
    return;
  }
  for (int i = 0; i < s->modelCount; i++) {
    if (s->models[i].model.meshCount > 0)
      BrushAssetsReleaseModel(s->models[i].path);
    s->models[i].model = (Model){0};
  }
}

void BrushSceneResolveMaterials(BrushScene *s) {
  // Release-then-acquire keeps refcounts neutral for unchanged paths, so the
  // editor can call this after every material edit.
  BrushSceneUnloadMaterials(s);
  for (int i = 0; i < s->materialCount; i++) {
    BrushSceneMaterial *m = &s->materials[i];
    m->albedoTex = BrushAssetsTexture(m->albedo);
    m->normalTex = BrushAssetsTexture(m->normal);
    m->displacementTex = BrushAssetsTexture(m->displacement);
    m->aoTex = BrushAssetsTexture(m->ao);
    if (m->tile <= 0.01f) m->tile = 1.0f;
  }
  for (int i = 0; i < s->modelCount; i++) {
    BrushSceneModelInstance *mi = &s->models[i];
    mi->model = BrushAssetsModel(mi->path);
    if (mi->scale.x == 0 && mi->scale.y == 0 && mi->scale.z == 0)
      mi->scale = (Vector3){1, 1, 1};
  }
}

// Build submit props from a material table entry (shared by blocks and
// placed models).
static bool MaterialProps(const BrushScene *s, const char *name,
                          BrushMaterialProps *out) {
  int mi = BrushSceneFindMaterial(s, name);
  if (mi < 0) return false;
  const BrushSceneMaterial *m = &s->materials[mi];
  if (m->albedoTex.id == 0 && m->normalTex.id == 0 && m->displacementTex.id == 0 && m->aoTex.id == 0) return false;
  *out = (BrushMaterialProps){
      .albedo = m->albedoTex,
      .normal = m->normalTex,
      .displacement = m->displacementTex,
      .ao = m->aoTex,
      .triplanar = !m->uvProjection,
      .normalSwizzled = BrushAssetsIsSwizzledNormal(m->normalTex),
      .texScale = m->tile,
      .specStrength = m->spec,
      .normalDepth = m->normalDepth,
      .heightScale = m->heightScale,
      .aoStrength = m->aoStrength,
  };
  return true;
}

bool BrushSceneBlockProps(const BrushScene *s, const BrushSceneBlock *k,
                          BrushMaterialProps *out) {
  return MaterialProps(s, k->material, out);
}

int BrushSceneTerrainLayers(const BrushScene *s,
                            BrushTerrainLayer out[BRUSH_TERRAIN_LAYERS]) {
  int count = 0;
  for (int i = 0; i < BRUSH_TERRAIN_LAYERS; i++) {
    out[i] = (BrushTerrainLayer){0};
    if (count != i) continue; // contiguous from slot 0
    int mi = BrushSceneFindMaterial(s, s->terrainLayers[i]);
    if (mi < 0) continue;
    const BrushSceneMaterial *m = &s->materials[mi];
    if (m->albedoTex.id == 0) continue;
    out[i].albedo = m->albedoTex;
    out[i].normal = m->normalTex;
    out[i].tile = (m->tile > 0.01f) ? m->tile : 1.0f;
    out[i].normalSwizzled = BrushAssetsIsSwizzledNormal(m->normalTex);
    count = i + 1;
  }
  return count;
}

bool BrushSceneModelProps(const BrushScene *s,
                          const BrushSceneModelInstance *m,
                          BrushMaterialProps *out) {
  return MaterialProps(s, m->material, out);
}

// --- Persisted render settings -------------------------------------------------
// One table maps world.def "post" keys onto the live tunable they drive.
// Bools ride as 0/1 floats. Capture rewrites the scene's whole post list
// from the live values; Apply pushes the file's values into the engine.

typedef enum { TUN_F, TUN_B } TunableKind;

typedef struct Tunable {
  const char *key;
  TunableKind kind;
  void *ptr; // float* or bool* into the live post/shadow state
} Tunable;

static int CollectTunables(Tunable *t, int cap) {
  BrushPost *pp = BrushRenderGetPost();
  struct BrushShadow *sh = BrushRenderGetShadow();
  int n = 0;
  if (pp != NULL && n + 19 <= cap) {
    t[n++] = (Tunable){"exposure", TUN_F, &pp->exposure};
    t[n++] = (Tunable){"bloom_threshold", TUN_F, &pp->bloomThreshold};
    t[n++] = (Tunable){"bloom_intensity", TUN_F, &pp->bloomIntensity};
    t[n++] = (Tunable){"sharpen", TUN_B, &pp->sharpenEnabled};
    t[n++] = (Tunable){"sharpen_amount", TUN_F, &pp->sharpenAmount};
    t[n++] = (Tunable){"ssao", TUN_B, &pp->ssaoEnabled};
    t[n++] = (Tunable){"ssao_radius", TUN_F, &pp->ssaoRadius};
    t[n++] = (Tunable){"ssao_strength", TUN_F, &pp->ssaoStrength};
    t[n++] = (Tunable){"dof", TUN_B, &pp->dofEnabled};
    t[n++] = (Tunable){"dof_range", TUN_F, &pp->dofRange};
    t[n++] = (Tunable){"dof_strength", TUN_F, &pp->dofStrength};
    t[n++] = (Tunable){"godrays", TUN_B, &pp->godRaysEnabled};
    t[n++] = (Tunable){"godrays_density", TUN_F, &pp->godRaysDensity};
    t[n++] = (Tunable){"godrays_exposure", TUN_F, &pp->godRaysExposure};
    t[n++] = (Tunable){"volfog", TUN_B, &pp->volFogEnabled};
    t[n++] = (Tunable){"volfog_density", TUN_F, &pp->volFogDensity};
    t[n++] = (Tunable){"volfog_ground", TUN_F, &pp->volFogGroundY};
    t[n++] = (Tunable){"volfog_top", TUN_F, &pp->volFogTopY};
    t[n++] = (Tunable){"volfog_coverage", TUN_F, &pp->volFogCoverage};
  }
  if (sh != NULL && n + 1 <= cap)
    t[n++] = (Tunable){"shadow_softness", TUN_F, &sh->softness};
  return n;
}

void BrushSceneApplyRenderSettings(const BrushScene *s) {
  Tunable t[32];
  int tn = CollectTunables(t, 32);
  for (int i = 0; i < s->postCount; i++) {
    const BrushScenePostSetting *ps = &s->post[i];
    bool known = false;
    for (int j = 0; j < tn; j++) {
      if (strcmp(t[j].key, ps->key) != 0) continue;
      if (t[j].kind == TUN_B) *(bool *)t[j].ptr = (ps->value > 0.5f);
      else *(float *)t[j].ptr = ps->value;
      known = true;
      break;
    }
    if (!known)
      TraceLog(LOG_WARNING, "BRUSH scene: unknown post setting '%s'",
               ps->key);
  }
}

void BrushSceneCaptureRenderSettings(BrushScene *s) {
  Tunable t[32];
  int tn = CollectTunables(t, 32);
  s->postCount = 0;
  for (int j = 0; j < tn && s->postCount < BRUSH_SCENE_MAX_POST; j++) {
    BrushScenePostSetting *ps = &s->post[s->postCount++];
    memset(ps, 0, sizeof(*ps));
    strncpy(ps->key, t[j].key, sizeof(ps->key) - 1);
    ps->value = (t[j].kind == TUN_B) ? (*(bool *)t[j].ptr ? 1.0f : 0.0f)
                                     : *(float *)t[j].ptr;
  }
}
