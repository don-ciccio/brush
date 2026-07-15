# Biome System — Design & Integration Plan

Status: proposal / analysis. Ties into the chunk-streamed world (`b_world`), the
foliage layers (`b_foliage`), the terrain splat (`b_render`/`lit.fs`), and the
scene format (`b_scene`). Zero-asset ethos and macOS GL 4.1 (no compute) hold.

---

## 1. Vision — what a "biome" is here

A **biome** is a named *bundle of look*, not a new subsystem bolted on the side:

```
biome = { terrain layer set, foliage layer set, grass-ground colour, mood }
                 (ground)        (plants)          (F3 tint)      (post/fog/light)
```

A **biome field** assigns, for every world (x,z), a small set of weighted biome
IDs — e.g. `{meadow:0.7, forest:0.3}` in a transition band. The world already
resolves per-chunk data on the worker thread (height, splat, foliage density,
road) and hands an immutable result to the main thread; the biome field is
**one more per-chunk map** resolved the same way. Everything downstream —
foliage scatter, terrain colour, ground tint — multiplies by the biome weight.

The guiding principle: **biomes ride the pipes we already have.** The grass-ground
tint I just built is already a per-region ground-colour mechanism; the terrain
paint is already "procedural auto rules + painted tiles composited per chunk";
foliage already has per-layer grow/avoid masks. Biomes are the missing *index*
that ties those to regions.

---

## 2. Research summary (what the industry does)

Two orthogonal axes: **how biomes map to space**, and **how they blend**.

**Mapping to space**
- **Climate model (Whittaker).** Biome = f(temperature, moisture), each a
  low-frequency noise field; elevation lowers temperature. A 2D lookup table
  (the Whittaker diagram) turns (temp, moist) → biome. Infinite, coherent,
  zero-asset. Used by most procedural generators.
- **Discrete regions (Voronoi / polygon map).** Scatter seed points, assign a
  biome per cell (Amit Patel's polygon-map, Vagabond). Good for art-directed,
  countable patches ("there are 5 regions").
- **Painted masks.** Landscape paint layers drive placement; UE's PCG samples
  painted layers to pick species. Best for hand-authored, local control.
- These **combine**: procedural climate as the base, painting/regions as
  overrides — exactly how our terrain splat already mixes auto-slope + painted
  tiles.

**Blending (the seam problem)**
- Naive per-texel biome lookup gives hard borders and, on a chunk grid, visible
  square seams. The clean fix is **Scattered Biome Blending** (NoisePosting):
  sample biome IDs at a *jittered hex grid* of points around the query, weight
  each by `max(0, r² − d²)²` (finite-radius Gaussian-ish bump), and **normalise**
  so weights sum to 1. No grid squareness, blends any number of biomes.
- **Critical optimisation for us:** when a whole chunk sits in one biome, skip
  blending entirely (up to ~36% faster, and most chunks are single-biome). This
  maps perfectly onto our per-chunk bake.
- Height-aware blending (dirt-in-cracks) is a *texture* concern we already have
  via the ORH height channel; biome blending sits one level above it.

Sources:
- [Amit Patel — Polygonal Map Generation](http://www-cs-students.stanford.edu/~amitp/game-programming/polygon-map-generation/)
- [AutoBiomes: procedural multi-biome landscapes](https://link.springer.com/article/10.1007/s00371-020-01920-7)
- [Fast Biome Blending, Without Squareness — NoisePosti.ng](https://noiseposti.ng/posts/2021-03-13-Fast-Biome-Blending-Without-Squareness.html)
- [UE5 PCG Biome Core reference](https://dev.epicgames.com/documentation/unreal-engine/procedural-content-generation-pcg-biome-core-and-sample-plugins-reference-guide-in-unreal-engine)
- [Advanced Terrain Texture Splatting](https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting)
- [Vagabond — Map Generation (Voronoi)](https://pvigier.github.io/2019/05/12/vagabond-map-generation.html)

---

## 3. The biome field (how biomes map to space)

Mirror the terrain-paint model: a **procedural base** the author can override
with **painted tiles**, resolved per-chunk on the worker.

**3a. Procedural base — climate.** Two low-frequency value-noise fields sampled
in world space:
- `temperature(x,z) = warpNoise(...)  − elevationLapse * max(0, height)`
- `moisture(x,z)    = warpNoise(...)`

A small **Whittaker lookup** (a coarse 2D table, author-editable) maps
`(temp, moist) → biomeId`. Both fields are domain-warped so borders meander
instead of following noise axes. This needs **no assets** and works the moment
the feature lands (every world gets biomes for free).

**3b. Authored overrides — painted biome tiles.** Reuse the exact paint-tile
infrastructure that backs foliage density and splat (`fmtile`/paint tiles):
a sparse set of RGBA8 tiles where each channel is a biome's paint weight. The
editor's biome brush writes these; the worker composites them over the climate
base. Optional **region seeds** (Voronoi) can be a later authoring convenience
that *bakes into* the same painted tiles.

**3c. Blending — scattered, per-chunk, single-biome fast path.**
On the worker, per texel of the biome map:
1. If the chunk's climate+paint is provably single-biome (cheap min/max test
   over the chunk corners + painted-tile presence) → write `{id:255}`, skip.
2. Else run scattered blending over a jittered hex neighbourhood → top-K biome
   weights (K=2, maybe 3). Normalise.

Output: a per-chunk **biome map** `hmRes²`, storing the **top-2 biome IDs + a
blend factor** (e.g. RG = id0/id1 as indices, B = blend 0..255). One RGBA8
texture, one new sampler. This is the single source of truth both foliage and
terrain read.

---

## 4. The layer-budget problem — and the refactor it forces

This is the real engineering content and where "refactor the foliage layers"
becomes "refactor how layers are *indexed*."

**Today.** 4 global terrain layers (`BRUSH_TERRAIN_LAYERS`), splat RGBA = 4
weights, each layer = its own albedo/normal/ORH samplers in `lit.fs` (~13 of 16
units used). 8 global foliage layers, each grow/avoid-masked against a splat
slot. **Everything is global** — there is no notion of "these plants/textures
belong to region X."

**Why biomes break this.** N biomes × their own ground textures blows past 4
layers and past 16 samplers instantly. We cannot keep one flat global set.

**Two-part fix, in increasing cost:**

### 4a. Foliage: scope layers to a biome (small, high payoff)
Add `int biomeId` to the foliage layer config. In the scatter, multiply the
layer's density by the biome weight from the new `biomeAt` sampler:

```
effectiveDensity = layer.density × biomeWeight(biomeId, x, z)
```

A forest-tree layer with `biomeId = forest` simply stops emitting as the forest
weight fades to 0 at the border — and the *neighbouring* biome's grass layer
fades in over the same band. **No new samplers, no draw-cost increase** (arguably
*less* overdraw, since layers no longer scatter world-wide). This alone makes
meadow / forest / scrubland feel distinct over identical ground.

This is the foliage refactor the user anticipated, and it is genuinely small:
one field, one sampler read, one multiply. `growLayer`/`avoidLayer` stay as-is
(they compose *within* a biome).

### 4b. Terrain: move to a texture **array**, index per biome (the big lift)
The 16-sampler wall is what forces discrete layers today. The standard escape is
a **`sampler2DArray`**: put all biome ground textures into one array texture
(albedo array + normal array + ORH array = **3 samplers total, any layer count**).
Splat/biome weights then carry *array indices* instead of being bound to fixed
sampler slots. GL 4.1 supports texture arrays natively (no compute needed).

- Per texel, the biome map gives up to 2 biome IDs + blend; each biome names a
  small palette of array indices; the shader samples the array at those indices
  and blends by (splat weight × biome weight).
- This is a real refactor of `lit.fs` and the terrain layer plumbing, but it
  **permanently removes the sampler ceiling** and is the correct long-term
  shape regardless of biomes (it also unblocks >4 terrain layers, which the
  road/height-blend work keeps bumping into).

**Incremental escape hatch:** ship 4a + **per-biome grass-ground colour**
(already a uniforms-only mechanism — no samplers!) *first*. Different plants +
different ground tint per biome delivers ~70% of the perceived variety with
near-zero risk. Do the texture-array refactor (4b) only when distinct ground
*textures* per biome become the bottleneck.

---

## 5. Data model

**New sampler** (`BrushChunkSamplers`, mirrors `splatAt`):
```c
void (*biomeAt)(void *ctx, float wx, float wz, BrushBiomeSample *out);
// out: id0, id1, blend (0..1)  — or a weights[K] form
```
Backed by `ChunkBiomeAt` reading the per-chunk biome map (like `ChunkSplatAt`).

**Chunk storage** (`WorldChunk`): `unsigned char *biomePixels; bool biomeValid;
Texture2D biomeTex;` — composed in `BuildCpu` next to `splatPixels`, uploaded at
finalize. Same lock discipline, same immutable handoff.

**Scene format** (`b_scene`, version bump → v4):
```
biome <name> <id> <grassColorHex> <priority>       # a biome definition
biome_terrain <biomeId> <layerIdx...>              # ground palette (array indices)
biome_climate <tempScale> <moistScale> <lapse> <seaLevel> <warp>
biome_whittaker <t0..tN> <m0..mN> <idGrid...>      # the lookup table (small)
foliage ... <biomeId>                              # foliage layer gains a biome
```
Painted biome tiles persist like the existing paint tiles (sidecar or in-scene).

**Structs**: `BrushSceneBiome { name, id, grassColor, priority, terrainLayers[],
foliageMask }`. Foliage layer + config gain `int biomeId` (−1 = all biomes,
back-compatible).

---

## 6. Worker / streaming integration

Nothing new architecturally — biomes slot into the existing bake order:

```
BuildCpu (worker, under sculptMutex):
  heightmap  = heightFn + sculpt + roads
  biomePixels = climate(temp,moist) ⊕ painted tiles, scattered-blended   ← NEW
  splatPixels = SplatWeightsAt (auto-slope/height + paint)   [may read biome]
  maskPixels  = FoliageMaskAt × biomeWeight                  [reads biome]     ← changed
  roadPixels  = RoadCoverageAt
  BuildMesh, cook collider
Foliage scatter (worker): density × biomeAt(biomeId)                           ← changed
Finalize (main): upload biomeTex alongside splatTex; swap handles
```

The **single-biome fast path** keeps the common chunk cheap. Biome resolution is
pure CPU noise + a tiny lookup, comfortably inside the existing worker budget.

---

## 7. The authoring tool (editor) — feature set

Modelled on UE's Biome Core + our existing paint tools:

**Biome library panel**
- Add/remove biomes: name, ID, identifying colour, priority.
- Per biome: assign terrain layer palette, grass-ground colour, foliage layers,
  and (later) a mood block (fog density/colour, exposure, light warmth).

**Biome field controls**
- Climate: temperature/moisture scale sliders, elevation lapse, sea level,
  domain-warp amount, seed. Live **Whittaker preview** (the 2D table with the
  current world's samples plotted).
- Whittaker table editor: paint biome IDs into the (temp × moist) grid.

**Biome paint brush** (mirrors the terrain/foliage brush already in the editor)
- Paint a biome onto regions with radius + **falloff**, painting into the biome
  tiles that override the climate base. Erase → revert to procedural.
- Optional region/Voronoi seed tool that bakes into painted tiles.

**Blend controls**
- Border width (blend radius), blend noise amount, single-biome threshold.

**Debug visualisation** (reuse the F2 layer-view cycling)
- Biome map false-colour overlay (each biome its ID colour), blend-band view,
  temperature/moisture heatmaps. Essential for tuning.

---

## 8. Phased rollout (each phase ships standalone)

- **Phase 0 — field + plumbing.** Climate noise, Whittaker lookup, `biomeAt`
  sampler, per-chunk biome map + texture, scattered blend w/ single-biome fast
  path, debug overlay. One biome configured → *zero visible change*, pure infra.
- **Phase 1 — foliage per biome.** `biomeId` on foliage layers; density ×
  biome weight. **First visible payoff:** different plants per region. (This is
  the anticipated foliage refactor — small.)
- **Phase 2 — grass-ground colour per biome.** Drive the existing F3 tint from
  the biome map (uniforms-only, no samplers). Ground reads distinct per region.
- **Phase 3 — painted biome regions.** Editor biome brush + biome tiles +
  library panel. Art-directed patches over the climate base.
- **Phase 4 — terrain texture-array refactor.** Distinct ground *textures* per
  biome; removes the 16-sampler ceiling for good.
- **Phase 5 — biome mood.** Per-biome fog/exposure/light, climate-driven,
  cross-faded by biome weight (ties into the day-night authority).

Phases 0–2 are the high-value / low-risk core. 3 is authoring. 4 is the deep
refactor, deferrable. 5 is polish.

---

## 9. Constraints & risks

- **GL 4.1 / no compute.** Biome resolution is CPU (worker); the texture array
  (Phase 4) is core GL 3.3+. No compute needed anywhere. ✅
- **16-sampler limit.** Phases 0–3 add exactly **one** sampler (biome map).
  Phase 4 *reduces* pressure (N layers → 3 array samplers). ✅
- **Chunk seams.** Scattered blending + shared world-space noise (no per-chunk
  RNG divergence) keeps borders seamless across chunk boundaries — same
  discipline the splat/paint tiles already follow.
- **Thermal / overdraw.** Biome-gated foliage can only *reduce* draw load
  (layers stop scattering world-wide). Watch that border bands don't *stack*
  two biomes' full foliage — cap combined density in the blend zone.
- **Back-compat.** `biomeId = −1` means "all biomes"; a v3 scene with no biome
  block behaves exactly as today (one implicit biome).
- **Scope creep.** Rivers/hydrology, temperature-driven snow lines, seasonal
  variation are all natural extensions — explicitly out of scope for v1.

---

## 10. Decisions (locked 2026-07-15)

1. **Field source:** ✅ **climate base + painted override** (matches the splat model).
2. **Blend K:** ✅ **2 biomes per texel** — biome map stores `id0, id1, blend`.
3. **Terrain array:** ✅ **now** — pulled forward from Phase 4 to Phase 2. Enabled
   by the *geometric-splat insight* (§11): splat weights stay biome-independent
   (auto-slope/height are geometry, not biome); a biome is a **palette** that
   maps the 4 splat slots to texture-array indices, and the shader lerps two
   biomes' palette-sampled results by `blend`. Splat storage is unchanged.
4. **Biome ceiling:** ✅ **16** (IDs 0..15; stored one-per-byte in the biome map).

Revised phase order: **0 field+plumbing → 1 foliage-per-biome → 2 terrain array
+ per-biome palette → 3 painted regions/editor → 4 per-biome mood.** (Old Phase 2
grass-ground colour folds into Phase 2's palette: a biome's `grassColor` drives
the existing F3 tint, uniforms-only.)

Implementation plan for Phases 0–2: **`docs/biome-implementation-plan.md`**.

## 11. The geometric-splat insight (why "terrain array now" is tractable)

Auto-slope (steep→rock) and auto-height (high→snow) are **geometric** rules —
they don't depend on which biome you're in. What changes per biome is *which
texture* plays the role of "the flat ground," "the slope," "the peak." So:

- `splatPixels` stays **4 biome-independent weights** (flat / slope / height /
  paint) — no change to how it's baked.
- A **biome** carries `palette[4]` = texture-array indices for those 4 slots,
  plus `grassColor`.
- The shader, per texel: sample the array at `biome0.palette[i]` and
  `biome1.palette[i]` for i in 0..3, weight each by `splat[i]`, sum per biome,
  then **lerp the two biome results by `blend`**. 8 array taps, **one** array
  sampler (×3 for albedo/normal/ORH). Splat map untouched, no ID interpolation
  in the weights.
