# Biome System — Implementation Plan (Phases 0–2)

Branch: `biome-system`. Design + decisions: `docs/biome-system-plan.md`.
Locked choices: climate base + painted override · 2 biomes/texel (`id0,id1,blend`)
· terrain array **now** · 16-biome ceiling · **geometric splat** (biome = palette).

Each phase compiles, runs, and is independently verifiable. Constants:
`BRUSH_MAX_BIOMES 16`, `BRUSH_TERRAIN_LAYERS 4` (unchanged), scene `version 4`.

> **Rev 2 (review incorporated).** Fixed the Phase 0 fast path (per-texel raw-id
> compare, not corners — altitude+noise flip the interior); pinned `blend`
> filtering to manual bilinear + documented the within-pair caveat; biomes use
> the pure base `heightFn` (deadlock-safe, verified); Phase 2 array assembly now
> addresses the compressed-`.ctex` readback wall (GPU-blit assemble or cook
> uncompressed) + layer dedupe + normalise normals at assemble time.

---

## Phase 0 — Biome field + plumbing (no visible change)

Goal: resolve a per-chunk biome map on the worker, expose a `biomeAt` sampler,
upload a `biomeTex`, and add a debug view. With one biome configured the world
looks identical — this phase is pure infrastructure.

### 0.1 Types (`b_world.h`, new `b_biome.h`)
- `#define BRUSH_MAX_BIOMES 16`
- `typedef struct { unsigned char id0, id1; float blend; } BrushBiomeSample;`
  (`blend` = fraction of `id1`; `id0` dominates when `blend==0`).
- Climate config (author-tunable, lives on `BrushWorld`, set from the scene):
  ```c
  typedef struct BrushBiomeClimate {
    float tempScale, moistScale;   // world metres per noise period
    float lapse;                   // temp drop per metre of height
    float seaLevel, warp;          // moisture bias, domain-warp amount
    unsigned seed;
    unsigned char whittaker[8*8];  // (temp,moist) 8×8 -> biomeId lookup
    int biomeCount;                // 0 -> single implicit biome (id 0)
  } BrushBiomeClimate;
  ```

### 0.2 Field evaluation (`b_world.c`)
- Reuse the existing value-noise used by `Height()` (find `ValueNoise`/`fbm`
  helper; add a 2-octave low-freq variant if needed — biomes want *coarse*).
- `static void BiomeRaw(const BrushWorld*, float wx, float wz, float *temp, float *moist)`:
  domain-warped fbm for each; `temp -= lapse * fmaxf(0, Height(...))`.
- `static unsigned char BiomeIdAt(const BrushWorld*, float wx, float wz)`:
  quantise (temp,moist)→whittaker cell→id.
- `static void BiomeWeightsAt(const BrushWorld*, float wx, float wz, BrushBiomeSample*)`:
  **scattered blend** — jittered hex points in a fixed radius (precomputed
  offset list), each weighted `max(0, r²−d²)²` by the id it lands on; keep the
  **top 2** ids, normalise → `blend`. Single-id neighbourhood → `{id,id,0}`.
- Biomes read the **base `heightFn`** for the lapse term — NOT the sculpted /
  road-flattened heightmap. Reasons: (a) `Height()` is pure (verified — no
  locks), so neighbour-point evals under `sculptMutex` can't deadlock; (b) hand
  sculpting a hill shouldn't silently repaint the biome under it.
- **Fast path (corrected).** The naïve "test 4 corners" is **wrong**: because
  `temp` is altitude-modulated *and* has a noise term, the biome can flip at an
  interior peak/dip the corners never see. Instead — the raw id is *cheap* (2
  noise taps + a lookup), the scattered blend is the ~10–20× cost. So:
  1. Precompute the **raw id per texel** into a scratch `hmRes²` buffer (cheap).
  2. If every raw id is equal **and** no painted biome tiles touch the chunk →
     fill the map `{id,id,0}`, skip the blend entirely.
  3. Otherwise scatter-blend, *reusing* the scratch ids as the point lookups.
  This is fully correct (no corner assumption), needs no min/max-height tracking
  (only `maxY` exists today anyway), and the scratch buffer feeds the blend so
  the raw ids aren't recomputed.

### 0.3 Chunk storage + bake (`b_world.c`)
- `WorldChunk`: `unsigned char *biomePixels; bool biomeValid; Texture2D biomeTex;`
  Layout `hmRes²` RGBA8: **R=id0, G=id1, B=blend·255, A=255**. (IDs 0..15 in a
  full byte — headroom, no nibble packing.)
- In `BuildCpu`, under `sculptMutex`, next to `splatPixels`: alloc + fill
  `biomePixels` via `BiomeWeightsAt`. Always valid when `biomeCount>0`.
- Free in the chunk teardown (mirror `splatPixels`/`maskPixels`).
- Finalize (main thread): upload `biomeTex` **point-filtered** (`GL_NEAREST`) —
  IDs must never interpolate (lerping id 2 and id 5 → 3.5 is nonsense).
- **Filtering the `blend` (corrected).** `GL_NEAREST` on the packed RGBA snaps
  `blend` too → stepped borders. Fix = **manual bilinear in the shader** (Phase 2)
  and in `ChunkBiomeAt` (Phase 1, CPU): take `id0,id1` from the **nearest** texel,
  but bilinear-interpolate **`blend`** from the 4 neighbours using the UV
  fraction. One NEAREST texture, ~4 taps on a tiny map — cheaper than a second
  sampler, and it keeps us under the unit budget.
  - **Caveat the review missed:** a single `blend` scalar is only meaningful
    *within a region where the (id0,id1) pair is constant*. Where the dominant
    pair flips (triple-junctions), bilinearly mixing `blend` across the flip is
    technically wrong — but at `hmRes` (~1 m texels) that's a ≤1-texel seam,
    below the noise floor. If it ever shows, widen the scattered-blend radius so
    pairs change more slowly; do NOT try to hardware-filter it.

### 0.4 Sampler (`b_world.h`/`.c`)
- Extend `BrushChunkSamplers`:
  `void (*biomeAt)(void *ctx, float wx, float wz, BrushBiomeSample *out);`
- `static void ChunkBiomeAt(void*, float, float, BrushBiomeSample*)`: nearest
  texel of `biomePixels` (bilinear on `blend` only, optional). Single implicit
  biome → `{0,0,0}`.
- Add to the `samplers` initialiser (currently `b_world.c:820`).

### 0.5 Scene format (`b_scene.h`/`.c`, version 4)
- `BrushSceneBiome { char name[32]; int id; Color grassColor; float priority;
  int palette[4]; }` + `biomes[BRUSH_MAX_BIOMES]; int biomeCount;` on `BrushScene`.
  Also store the `BrushBiomeClimate` (climate + whittaker) on the scene.
- Parse lines: `biome <name> <id> <grassHex> <priority>`,
  `biome_climate <tempScale> <moistScale> <lapse> <seaLevel> <warp> <seed>`,
  `biome_whittaker <64 ids>`. Save them back (mirror the foliage save block).
- Back-compat: a v≤3 scene (or v4 with `biomeCount==0`) → one implicit biome,
  `biomeAt`→`{0,0,0}`, nothing changes.
- Wire the scene's climate/biomes into `BrushWorld` at world init (where
  layers/roads are already handed over).

### 0.6 Debug view (`b_render.c` / console F2)
- Add a `BIOME` entry to the F2 layer-view cycle: a debug fragment path that
  samples `biomeTex` and maps id→its `grassColor` (or a fixed 16-colour LUT),
  showing `blend` as a dither/gradient at borders.
- **Milestone:** F2 → BIOME shows coherent, meandering false-colour regions;
  FINAL view unchanged.

---

## Phase 1 — Foliage per biome (first visible payoff)

Goal: a foliage layer only scatters inside its biome, fading across borders.

### 1.1 Config (`b_foliage.h`)
- `BrushFoliageLayerConfig`: add `int biomeId; // -1 = all biomes`.

### 1.2 Scene (`b_scene.h`/`.c`)
- `BrushSceneFoliageLayer`: add `int biomeId;`. Append it to the `foliage` line
  format + save (default `-1`, back-compatible with existing scenes).

### 1.3 Conversion (both call sites)
- `sandbox/main.c` (~`:417`) and `editor/main.cpp` (`BuildFoliageLayers`,
  `~:286`): copy `.biomeId = fl->biomeId`.

### 1.4 Scatter (`b_foliage.c`, `FoliagePlace`)
- After the height/slope gates, compute biome weight of this layer's biome:
  ```c
  if (cfg->biomeId >= 0) {
    BrushBiomeSample bs; s->s->biomeAt(s->s->ctx, jx, jz, &bs);
    float w = (bs.id0==cfg->biomeId ? 1.0f-bs.blend : 0.0f)
            + (bs.id1==cfg->biomeId ? bs.blend      : 0.0f);
    if (w <= 0.001f) continue;
    // smooth border: probabilistic reject like the maxHeight fade band
    float rp = (float)((h >> 3) & 0xffff)/65535.0f; // distinct hash slice —
    if (rp > w) continue;                           // heightfade uses >>7, model >>9
  }
  ```
- `ChunkBiomeAt` does the **manual-bilinear-on-`blend`** from §0.3 so the fade is
  smooth per-instance (nearest ids, bilinear blend).
- Cap combined density in blend bands: since each biome's layers reject by their
  own weight and the two weights sum to 1, total instances are conserved — no
  stacking. (Verify by eye at a border.)

### 1.5 Editor UI (`editor/main.cpp`)
- Foliage panel: a **Biome** combo per layer (─, then each biome by name).
- **Milestone:** two biomes (meadow=grass layer, forest=tree layer); walking the
  border shows grass thinning out as trees fade in, no hard line.

---

## Phase 2 — Terrain texture array + per-biome palette

Goal: distinct ground **textures** per biome. Uses the geometric-splat model:
splat weights stay biome-independent; a biome maps the 4 slots to array indices;
the shader lerps two biomes' palette samples by `blend`. Removes the 16-sampler
ceiling permanently.

### 2.1 Texture arrays (`b_assets.c`)
- rlgl has **no** `TEXTURE_2D_ARRAY` path — this is a **raw-GL** block:
  `glGenTextures` → `glBindTexture(GL_TEXTURE_2D_ARRAY)` →
  `glTexImage3D(..., depth=count)` → `glGenerateMipmap`. Wrap the id in a
  `Texture2D` (id + a flag) or a small `BrushTexArray` handle.
- **Where the pixels come from — this is the ORH lesson again.** Assembling the
  array needs each layer's pixels at a **uniform size**, and `ImageResize` only
  works on **uncompressed** images. In a release pak the sources are cooked BCn
  `.ctex`, and desktop GL **cannot read compressed pixels back** (we proved this
  fixing the surface map — `rlReadTexturePixels` refuses `format ≥ DXT1`). So:
  - **Editor / loose PNGs:** `LoadImage` → `ImageResize` → `ImageFormat(R8G8B8A8)`
    → copy each into the `glTexImage3D` upload. Simple, works today.
  - **Release robustness (two options):**
    1. Cook terrain layers **uncompressed** (they're few, ≤~32) so the CPU-assemble
       path also works from the pak — simplest.
    2. Or **GPU-assemble**: load each layer as a GPU texture (pak-aware, compressed
       OK via `SurfaceSourceTex`), then blit it into the array layer with
       `glFramebufferTextureLayer` + a quad (GL 4.1 — `glCopyImageSubData` is 4.3,
       unavailable on macOS). This also lets us normalise normal encoding in the
       blit (see 2.2). Recommend option 2 — it reuses the ORH-composite pattern
       and keeps cooking/compression intact.
- **Dedupe:** the array holds the **union** of all biomes' terrain layers, keyed
  by source path — a rock shared by 3 biomes is **one** array layer, and each
  biome's `palette[]` just points at it. Size the array to the actual union, not
  a fixed 32.
- Build **three** arrays (albedo, normal, ORH). VRAM ≈ layers × 1K² × 3 × mips —
  order ~150 MB at 32 layers, fine for desktop and "free" per the perf audit.

### 2.2 Shader (`lit.fs`)
- Replace `uLayerAlbedo0..3`, `uLayerNormal0..3`, `uLayerSurface*` **discrete**
  samplers with `uniform sampler2DArray uAlbedoArr, uNormalArr, uSurfaceArr;`
  (frees ~9 units → 3).
- New uniforms: `uBiomePalette[16*4]` (int array indices), sample `uBiomeTex`
  (point) for `id0,id1`; sample a bilinear-blend for `blend` (store `blend` in a
  channel that *is* safe to bilinear).
- Per texel, for each biome b in {id0,id1}:
  `colB = Σ_i splat[i] * texture(uAlbedoArr, vec3(uv_i, palette[b][i]))`
  then `albedo = mix(col_id0, col_id1, blend)`; same for normal/ORH.
- `SampleLayer`/`SampleLayerBump` gain a `float layerIndex` arg and sample the
  array; triplanar/tiling logic is unchanged (per-slot tile still applies).

### 2.3 Data (`b_scene`, `b_render`)
- Biome `palette[4]` = indices into the arrays (parsed in Phase 0's `biome`
  line — extend it: `biome <name> <id> <grassHex> <priority> <p0 p1 p2 p3>`).
- `b_render`: build the 3 arrays from the union of all biome terrain layers at
  scene resolve; upload `uBiomePalette`, bind arrays + `biomeTex`.
- `splatPixels` bake is **unchanged** (still 4 geometric weights). `biomeTex`
  from Phase 0 is reused directly.

### 2.4 Grass-ground colour per biome (folded in — uniforms only)
- Drive the existing F3 grass-ground tint (`BrushRenderSetGrassGround`) from the
  per-texel biome: `mix(biome[id0].grassColor, biome[id1].grassColor, blend)`.
  No samplers. Ground reads distinct per region for free.
- **Milestone:** meadow (green ground + grass) vs rocky highland (rock palette +
  pale tint) blend seamlessly across a border; sampler count ≤ current.

### 2.5 Risks specific to Phase 2
- **`blend` filtering:** resolved in §0.3 — manual bilinear on `blend`, ids from
  the nearest texel. No second texture, no hardware filtering of ids.
- **Array layer resolution:** all layers must share dimensions in the array —
  resize to a fixed 1K (or the union max) at assemble; warn if a source is wildly
  off (a 4K terrain in a 1K array wastes the source detail).
- **Normal encoding — normalise at assemble time (not in the shader).** A
  `sampler2DArray` can't carry a per-layer "is DXT5nm" flag without a bool array
  uniform + a branch per tap. Instead, **reconstruct every normal layer to plain
  RGB when building the array** — trivial in the GPU-assemble blit (apply the
  DXT5nm→RGB reconstruction in the blit shader for swizzled sources), or on the
  CPU for the uncompressed path. `uNormalArr` is then uniformly RGB and the main
  shader samples it with **zero** conditional swizzle.
- **8 taps vs 4:** ~2× terrain texture work per pixel. Per the perf audit
  textures are near-free (<2% at 4×), so acceptable; the single-biome dynamic
  branch (`id0==id1`) early-outs to 4 taps and coheres per warp since borders are
  thin — the vast majority of screen pixels take the fast path.

---

## Sequencing & test gates

1. Phase 0 → merge gate: F2 BIOME view correct, FINAL unchanged, no perf regression.
2. Phase 1 → merge gate: border foliage fade looks natural, density conserved.
3. Phase 2 → merge gate: two-texture-set biomes blend seamlessly; sampler count
   verified ≤ 16 (should drop); single-biome early-out confirmed.

Editor authoring (biome brush, library panel, Whittaker editor) and per-biome
mood are **Phase 3–4**, planned after 0–2 land.
