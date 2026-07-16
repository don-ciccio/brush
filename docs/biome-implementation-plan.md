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
>
> **Rev 3 (Phase 2 in progress).** Confirmed the array must assemble from the
> resolved layer **Texture2D handles**, not source paths — `BrushTerrainLayer`
> carries only textures, so the CPU-from-paths idea is dropped in favour of the
> §2.1 option-2 **GPU-blit assemble** (render each layer texture into an array
> slice; decompresses BCn sources, resizes, normalises normals in one pass).
> Sequencing: **do the array refactor with the CURRENT 4 layers first — terrain
> must look IDENTICAL** — as an isolated de-risk of the sampler2DArray path
> before the biome palette (§2.3) rides on top. Sub-split: albedo array, then
> normal array.

---

## Progress (living log)

- **Phase 0 — DONE, committed `35b39b5`.** As specced, with fixes found in build:
  the `biome_whittaker`/`biome_climate` parse must come **before** `biome ` (the
  `sscanf("biome %s")` prefix otherwise eats them — it corrupted a scene on save);
  temp/moisture get a ×2.2 contrast expansion (value-noise fbm clusters at 0.5,
  so without it the world reads as 1–2 biomes); no chunk-local jitter in the blend
  (it stitched a seam down every chunk edge); F2 BIOME view gated to terrain so
  props/player shade normally.
- **Phase 1 — DONE, committed `f0d7571`.** `biomeId` on foliage layers end to end.
  Extra fix: the **editor** never pushed the biome climate to its world, so
  biome-gated foliage was invisible in the preview (worked in-game) — added to
  `ApplyTerrainLayers`. Folded in an independent **foliage-LOD crossfade fix**
  (offset the near↔far fade windows so grass doesn't flatten then stand up;
  thin single-strand foliage sets `farKeepRatio=1` to avoid a crossfade double).
- **Phase 2 — DONE (branch, committing now).**
  - **2.1 array builder.** `BrushAssetsTextureArray(const Texture2D*, const bool
    *swizzled, count, size, isNormal)` — GPU-blit from resolved textures via a
    RenderTexture + readback + `glTexSubImage3D` (raw GL, macOS `<OpenGL/gl3.h>`).
    Normals normalised DXT5nm→RGB in the blit. RT readback is fine (RGBA8); the
    compressed-source wall is dodged (sample the source, don't read it back).
    Release pak untested (loose sources work in dev).
  - **2.2 albedo + normal arrays — VERIFIED identical.** Built in `BuildLayerArrays`
    (b_world), passed via `BrushSplatDraw`, bound on units 3/6;
    `SampleLayerArr`/`SampleLayerBumpArr` in lit.fs.
  - **2.3 palette — via material LIBRARY.** Decided: **biomes index `materials[]`
    directly.** The array is built from the whole library (slice i = `materials[i]`,
    `BrushWorldSetTerrainLibrary` / `BrushSceneTerrainLibrary`); a biome
    `palette[slot]` is a material index; the **default** for an unset slot is the
    material index of the painted `terrainLayers[slot]` (so biome-less scenes are
    unchanged — this fixed the "everything green / toggles dead" regression from a
    zero/identity default). Shader double-taps the two biomes' palettes and lerps
    by blend. `ChunkBiomeAt` does bilinear-on-blend (blocky-border fix).
  - **2.4 per-biome grass-ground colour.** `uBiomeGrassColor[16]` + `uBiomeGroundOn`
    drive the existing F3 tint; off when the scene defines no biomes.
  - **Deviations:** only **albedo + normal** arrays (no ORH/surface array — POM
    uses `uLayerHeight`, roughness stays scalar; per-biome height/roughness
    deferred). Layer-3 normal is in the array but unsampled.
  - **Per-material tile — DONE (follow-up landed 2026-07-16).** `uMatTile[32]`
    (metres-per-repeat per array slice) replaces the per-slot `uLayerTiles` in
    all 12 array-sample calls, so a palette-swapped material keeps ITS authored
    tiling instead of inheriting the painted slot's. Pushed from
    `BuildLayerArrays` (both library and 4-layer fallback), unused slices pad
    to 1.0 (no div-by-zero from stale indices). Bit-identical for default
    palettes (slot tile WAS the painted material's tile). POM stays slot-level
    via `uPomTile` — displacement isn't in the array, so the palette can't
    retarget it anyway. Verified: lit.fs compiles + links via the editor
    screenshot harness (silent default-shader fallback is the failure mode).
- **Launch road-carve — FIXED.** Not a config slot after all: `BrushWorldWaitResident`
  drains all pending (re)bakes after the post-create setup (roads/sculpt/biomes),
  called at the end of `SandboxInit`, so the terrain launches fully carved.
- **Phase 3 authoring (steps 1–2) — DONE (branch, uncommitted).** The reason the
  branch exists: biomes are now *editable*, not just procedural.
  - **3.1 Biome library panel** (`Environment ▸ Biomes`): list/add/delete biomes,
    edit name, grass-ground tint (live), and the 4-slot terrain palette as material
    dropdowns (`(default)` = the painted `terrainLayers[slot]` material). Edits set
    `g_dirty` + re-apply palette/climate immediately.
  - **3.2 Biome paint brush**: sparse `BiomeTile` overlay (1 byte/sample,
    `0xFF`=no override) keyed on the heightmap grid like splat/foliage;
    `ComposeBiomeMap` reads the override before the climate lookup so the field
    stays continuous across chunks. `BrushWorldPaintBiome` = hard discrete stamp,
    only re-bakes touched chunks. Toolbar **Biome** tool + swatch strip (tinted by
    grass colour); shift-drag erases to climate. Persisted in the sculpt blob
    (**`BSC4`**, back-compatible).
  - **3.3 Paint overlay helper**: `uBiomeOverlay` blends the lit terrain toward each
    region's biome colour (0.55) while the Biome tool is live, so painted shapes
    read against the ground. Editor-only, gated to `MODE_SCULPT` + biomes-exist +
    normal view; zero at runtime.
  - **3.4 Whittaker / climate editor — DONE (branch, uncommitted).** Climate
    sliders (temp/moist region size — log scale, altitude lapse, sea level, border
    warp, border blend, seed + Reroll) and the **8×8 Whittaker grid** (rows
    cold→hot, cols dry→wet; cells coloured by the biome's swatch, grey =
    undefined id). Click or **drag-paint** cells with the biome selected in the
    list; "Fill map with selected" floods it. All edits collect in a pending flag
    and apply **on mouse release** (one `ApplyTerrainLayers` → full re-bake +
    foliage resync), never per drag-frame. Phase 3 authoring is COMPLETE.

---

## Performance notes (measured/assessed 2026-07-16)

Biomes are **cheap**; the only lever worth watching is texture-array memory.

- **Per-frame GPU — negligible.** The only added runtime cost is the terrain
  shader **double-tapping** the albedo+normal arrays (`id0` and `id1`, lerp by
  blend). Two guards keep it small: the **single-biome fast path** in
  `ComposeBiomeMap` writes `blend=0` for uniform chunks so `if (id0!=id1 &&
  bblend>0)` skips the 2nd tap — only the *border band* pays 2× — and the gym
  audit shows terrain sampling is <2% of the frame (post+shadows dominate). No
  per-frame CPU: biome maps are baked to textures; `biomeAt` is scatter-time only.
- **Chunk bake (worker) — one-time, off-thread.** `ComposeBiomeMap` blend loop is
  O(hmRes²·M²) with **M≤6** and the uniform fast-path; `BiomeRaw` is a few fbm
  octaves per apron texel. Rides the existing bake, no extra pass.
- **Memory — the one real lever.** `BuildLayerArrays` packs the **whole material
  library** into `sampler2DArray`s: **RGBA8 (uncompressed), ≤2048², mipmapped, ×2**
  (albedo+normal). Uncompressed because GPU-blit assembly can't read back `.ctex`.
  Scales with **library size × resolution — NOT biome count or world extent**:
  8 mats @512² ≈ 20 MB (fine) · 16 @1024² ≈ 170 MB · 32 @2048² ≈ ~1.3 GB (ceiling).
  Every slice is full-res whether a biome uses it or not.
- **Trivia:** biome paint tiles = 1 byte/sample sparse; per-chunk `biomeTex` = one
  RGBA8 `hmRes²` map (~17 KB/chunk). Both negligible.

**Discipline (no code needed now):** keep the terrain library to materials biomes
actually use, and don't push the whole set to 2048². **Optimisations if it ever
bites**, in effort order: (1) prune the array to *referenced* slices (dedup) —
biggest win; (2) clamp array resolution independent of source (one line in
`BuildLayerArrays`); (3) keep `blendRadius` modest (widens both the bake
neighbourhood and the double-tap band); (4) *(big lift, likely unnecessary)*
compressed BC7 arrays via a GPU path. Deferred — no current scene is close.

### Code audit findings — FIXED (2026-07-16, pre-commit)

A line-by-line audit of the new paths found five real defects (all fixed):

1. **Data race (crash-grade):** `ComposeBiomeMap` read the biome paint tiles on
   the bake worker WITHOUT `sculptMutex`, while main-thread strokes `MemRealloc`
   the tile array. The Phase-0 "compose outside the lock" comment assumed a pure
   function — the paint override broke that invariant. **Fix:** pre-read the
   overrides under the mutex into a buffer (with a last-tile cache — strokes are
   spatially coherent), keep the expensive climate noise outside the lock.
2. **Full-world rebake per paint stroke:** the editor's biome-stroke handler set
   `g_foliageResyncPending`, which runs `SyncFoliageToWorld` →
   **`BrushWorldRebakeAll`**. The localized chunk rebake already re-scatters
   foliage via the chunk hooks (why Grass paint never sets the flag). **Fix:**
   removed — biome paint is now as cheap as splat paint (touched chunks only).
3. **Double full rebake on climate edits:** climate apply called
   `ApplyTerrainLayers` (→ `SetBiomeClimate` → rebake) AND set
   `g_foliageResyncPending` (→ a second full rebake). **Fix:** flag removed.
4. **Unguarded heavy setters:** `SetBiomeClimate` rebaked the whole world and
   `SetTerrainLibrary` rebuilt both GPU arrays (per-slice RT blit + **pipeline
   readback stall** + mipmap gen) on EVERY call — and both sit inside the
   editor's `ApplyTerrainLayers`, which runs on scene reload, texture re-import,
   and biome add/delete. **Fix:** memcmp no-change guards on both (mirroring the
   `SetLayers` guard that fixed the launch flicker).
5. **Save-loss bug (not perf):** `BrushWorldSculptAny` didn't count biome tiles,
   so a biome-paint-only edit skipped the terrain-blob save — painted biomes
   silently lost. **Fix:** `btileCount` added.

Also: biome uniform locations are now cached in `g_r` (engine pattern; the
overlay setter runs every editor frame and now early-outs on unchanged value).
Audited and OK as-is: shader border double-tap (fast-pathed), per-chunk biome
map memory (~17 KB), BSC4 undo-blob growth (1 B/sample), Whittaker grid ImGui
cost, `ChunkBiomeAt` bilinear (scatter-time only).

### Structural review — FIXED + notes (2026-07-16, pre-commit)

A follow-up architectural review of the whole feature found three more defects
(fixed) and some accepted trade-offs:

1. **id-vs-list-order bug:** `BrushSceneApplyBiomePalette` filled
   `uBiomePalette`/`uBiomeGrassColor` by **list position**, but the shader
   indexes them by **biome ID**. Identical only while `biomes[i].id == i` —
   one panel Delete+Add decouples them and palettes/tints attach to the wrong
   biomes. **Fix:** both tables keyed by `bm->id`; stale material indices
   bounds-checked; IDs no biome defines tint like the FIRST defined biome
   (never black — stale whittaker cells/painted tiles must not punch holes).
2. **Palette persisted as raw material indices** — the only index-based
   material reference in the format (terrain layers are name-based), and the
   Materials panel CAN delete ⇒ silent palette corruption on reorder/delete.
   **Fix:** palettes now save as material NAMES ("-" = unset); the loader
   accepts both (bare ints = legacy), resolving AFTER the full parse so file
   order never matters. Verified by a new headless round-trip test
   (`test_biome_roundtrip`, 40 checks: names, legacy ints, unknown names,
   climate/whittaker survival — also re-guards the sscanf-prefix bug).
3. **Space in the default biome name** (`"biome %d"`) would corrupt the
   space-delimited format on the save round-trip. **Fix:** `"biome%d"` +
   spaces→underscores in the name InputText.

**Accepted trade-offs (documented, no code):** `priority` field is dormant
(parsed/saved, never consumed — reserved for border dominance); deleting a
biome leaves its id in whittaker cells/painted tiles/foliage gates, and a later
Add reusing that id resurrects those regions (visible as grey cells in the
grid; a "clear paint for id" API is a possible follow-up); the sampler's
nearest-id + bilinear-blend is semantically approximate where neighbouring
texels reference different biome pairs (invisible in practice — the field is
continuous). Verified sound: point-filtered `biomeTex` (ids must never
interpolate), one-directional data flow (scene → world → chunk map →
shader/foliage), types-only `b_biome.h`, canonical-grid paint keying (seam-free
by construction), BSC4 magic-versioned append format.

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
