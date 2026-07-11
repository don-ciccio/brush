# Asset Pipeline, Projects & Packaging

Integrates Danilo's asset-pipeline draft (2026-07) with engine reality checks
and the larger productization picture: brush stops being "a repo with a
sandbox in it" and becomes **an engine that hosts projects** — you launch the
editor, land on a project screen, create a game from a template, and ship it
as a packaged build. The asset pipeline is one pillar of that; the project
system is the foundation it stands on, so it comes first.

Reference points: Godot (project.godot + `.godot/imported/` + `.pck`),
Unity (`Library/` + `.meta`), Unreal (DDC + `.pak`), id Software (`.pak`/
`.wad`). We take the *shape* of those systems and cut everything a
two-binary C engine doesn't need.

---

## 0. The dependency that makes ordering non-negotiable

Today every file the engine touches is **cwd-relative to the repo root**:
`engine/shaders/lit.vs`, `assets/gym.def`, `assets/textures/rock.png`. A
project system breaks that assumption in half:

* **Engine-owned files** (16 shader loads in b_render/b_post/b_sky, SMAA
  lookup textures) must resolve relative to the **engine installation** —
  found by walking up from the executable path (`_NSGetExecutablePath` on
  macOS, `/proc/self/exe` on Linux; the binary lives in `<repo>/build/`).
  The SMAA LUTs move out of the project-facing `assets/` into
  `engine/resources/`. Later, a release build can embed the shaders as C
  arrays; exe-relative lookup is the dev-tree mechanism.
* **Project-owned files** (world.def scenes, .terrain, textures, models,
  the import cache) resolve relative to the **project root**. Mechanism:
  the editor/player `chdir()`s into the project at startup, so every
  existing `assets/...` path in scenes and code keeps working unchanged —
  no path-prefix plumbing through the whole codebase.

That split (BrushEnginePath() for engine files + chdir for project files)
is the entire structural cost of the project system, and it's small. Every
later phase assumes it.

---

## 1. Project system (Phase 0 — build first)

### project.def

A project is a folder with a `project.def` at its root (same plain-text,
warn-and-skip format family as world.def):

```
version 1
name My Game
scene assets/main.def
```

`scene` is what the player runs and what the editor opens by default. The
brush repo itself carries a `project.def` (name "Sandbox Gym", scene
`assets/gym.def`) so the repo is just another openable project and nothing
about today's workflow changes.

### Project Manager screen (Godot-style)

The editor becomes a two-state app: **PROJECT MANAGER → EDITOR**.

* Launching with no argument shows the manager: a list of known projects
  (name from each project.def, path, missing-folder detection) plus
  **Open** / **Create New**.
* Known projects persist in `~/.brush/projects` (one absolute path per
  line, most-recent first). Opening from the manager or CLI
  (`--project <dir>`, `BRUSH_PROJECT` env) re-orders the list.
* **Create New Project**: name + parent folder + template:
  * **Empty** — project.def, an empty scene with a spawn point on flat
    terrain, and the character (the player binary needs the mannequin).
  * **Sandbox Gym** — the full current gym: scene, .terrain, starter
    textures, character. This is the "bare engine with the starter
    sandbox" idea: the gym stops being hardcoded bootstrap and becomes a
    **template that gets copied into your project**.
* Templates are copied file-by-file at create time; nothing in the engine
  references them afterwards. v0 sources them from the engine repo's own
  `assets/` (the gym *is* the template); a dedicated `templates/` tree can
  take over when templates diverge from the dev sandbox.

The **player** (today's sandbox binary) honours the same `--project` /
`BRUSH_PROJECT` selection: it chdirs to the project, reads project.def,
loads `scene`. The editor's Play button forks the player from the engine
tree (exe-relative path) with cwd already inside the project — the live
edit → hot-reload loop is unchanged.

Out of scope for Phase 0: switching projects without relaunching (Godot
also restarts), per-project engine versions, project icons.

---

## 2. Source vs. runtime asset separation (Phase 1)

The industry split, kept verbatim from the draft:

* **`assets/`** — raw source files (.png, .glb, .wav), git-tracked, human
  units. Slow to parse (PNG inflate), un-mippped, uncompressed for VRAM.
* **`.brush/imported/`** — git-ignored, machine-made cache of cooked
  binaries mirroring the assets tree. Deletable at any time; the importer
  regenerates it.
* **`<asset>.import`** — INI-style sidecar next to each source file with
  the import *parameters*:

```ini
[params]
max_size = 1024
generate_mipmaps = true
compress = bc3        ; none | bc1 | bc3 (see §3 — NOT bc7, see below)
is_normal_map = false ; disables gamma-space ops, picks bc3 layout
```

**Asset identity is the logical path**, not a UUID. Godot needs UUIDs
because resources cross-reference each other by the thousands; in brush,
references live in world.def lines you can grep and sed. A rename is a
rename-plus-fixup (the editor already does exactly this for material
renames). UUIDs can be retrofitted if cross-references ever justify them;
starting with them buys complexity and an opaque assets folder.

**Cache invalidation** (missing from the draft, and the part that makes
or breaks trust in an import cache): each cooked file's header stores
(a) source file size + mtime (upgradeable to a content hash), (b) a hash
of the .import params, (c) an importer code version. On lookup, any
mismatch → silent reimport. Bumping the importer version constant
invalidates the world after an algorithm change.

**Fallback chain keeps development unblocked** — `BrushAssetsTexture()`
resolves in order:

1. mounted `.pak` (release builds, §4)
2. `.brush/imported/<path>.ctex` if valid (fast path)
3. the raw source file, imported on the spot (cache miss → cook → load),
   or loaded raw if cooking fails for any reason

So a fresh clone with an empty cache still runs, just slower on first
load — Godot behaves the same way.

---

## 3. Texture cooking: what's actually possible on our targets

The draft proposes BC7. **BC7 is not available on macOS**: Apple's OpenGL
stops at 4.1 and BPTC became core in 4.2 (and Apple never shipped the
extension). What raylib's GL context on macOS *does* support is S3TC —
BC1 (opaque, 8:1) and BC3 (alpha/normal-friendly, 4:1) — uploaded with
`glCompressedTexImage2D` (raylib's `rlLoadTexture` already handles the
`PIXELFORMAT_COMPRESSED_DXT*` formats, including per-mip data).

So the realistic cooking ladder is:

* **Tier 0 (first ship):** decode PNG once, downscale to `max_size`,
  generate the full mip chain on CPU (`ImageMipmaps()`), write raw
  RGBA8 + mips into `.ctex`. Wins: no PNG inflate at runtime, no GPU-side
  mip generation, VRAM safety via max_size. This is most of the load-time
  win for a day of work.
* **Tier 1:** BC1/BC3 encoding via **stb_dxt** (single vendorable header,
  same family as our other deps). Albedo → BC1 (or BC3 with alpha),
  normal maps → BC3 with the X-in-alpha "DXT5nm" swizzle the shader
  already half-supports (we control lit.fs). 4-8× VRAM reduction, GPU
  decodes for free.
* **Tier 2 (only with a platform that needs it):** BC7/ASTC via bc7enc /
  astcenc as alternate profiles in the same .ctex container. Not before.

### .ctex container

```
magic 'BTX1' | importerVersion u32 | srcSize u64 | srcMtime u64 |
paramsHash u32 | format u32 (raylib PixelFormat) | width u16 | height u16 |
mipCount u16 | reserved | mip data, tightly packed, mip0 first
```

Loading = read header, validate, one `fread` per mip straight into
`rlLoadTexture`. No image library in the fast path. (The draft's DDS
alternative would also work — raylib parses DDS — but a 40-line custom
header we fully control beats implementing DDS's legacy corner cases.)

### Import execution

Cooking runs on a **worker thread** (pure CPU: stb_image decode, resize,
mip chain, stb_dxt) exactly like chunk baking; only the final
`rlLoadTexture` happens on the main thread. The editor watches source
mtimes (same 30-frame poll cadence as scene hot-reload) and re-imports
changed files, then swaps the texture in place through the registry —
this is precisely why b_assets was built ref-counted and central.

Model cooking (glTF → binary vertex/index blobs + pre-cooked Jolt shapes)
rides the same registry/cache/sidecar pattern later; it is explicitly not
part of the first pipeline milestone.

---

## 4. Packaging for distribution (Phase 3)

One archive, mounted as a virtual filesystem — the draft's format is
right; two hardening details added (alignment, hashed lookup):

```
[Header]   magic 'BPK1' | version u32 | index offset u64
[Data]     each file's bytes, 16-byte aligned (direct/mmap-friendly reads)
[Index]    count u32, then per entry:
           pathHash u64 (FNV-1a) | path (len-prefixed, for tooling/debug) |
           offset u64 | size u64   — entries sorted by pathHash
```

* `tools/packager` walks `assets/` + `.brush/imported/`, prefers the
  cooked form of every asset (raw PNGs stay out of the pak unless nothing
  cooked them), writes the archive. Scenes/.def files go in as-is —
  they're tiny and diff-friendly.
* Runtime lookup = binary search on the hash + memcmp confirm, one seek,
  one read. The index loads once at mount into the registry.
* `BrushAssetsMount("game.pak")` sits at the top of the §2 fallback
  chain; a release player build is engine binary + pak + project.def.
* Per-entry compression (LZ4) is a later flag in the same container —
  cooked textures are already GPU-compressed, so the win is mostly for
  text and meshes.

---

## 5. Phasing

| Phase | Deliverable | Unblocks |
|---|---|---|
| **0** | ~~Engine/project path split (BrushEnginePath + chdir), project.def, Project Manager screen, templates (Empty, Gym), player `--project`~~ DONE | everything below; "brush as a product" |
| **1** | ~~.import sidecars, `.brush/imported/` cache, Tier-0 .ctex (mips, max_size), invalidation, worker cooking, live re-import on change~~ DONE | fast loads, VRAM safety, drag-in textures |
| **2** | ~~stb_dxt BC1/BC3 tier, normal-map profile, import settings UI in the editor (per-texture panel)~~ DONE | shipping-quality VRAM budgets |
| **3** | packager tool + pak mount + release player build | distributable games |

Each phase lands usable on its own; no phase requires touching game-facing
APIs (`BrushAssetsTexture(path)` is already the stable seam all of this
hides behind).

Phase 1 implementation notes (2026-07): cooking lives entirely inside
b_assets — first loads cook synchronously (a cache miss must return a
texture), edits re-cook on the worker and `BrushAssetsUpdate()` lands the
swap on the main thread; callers re-resolve their Texture2D copies when it
returns true. `BRUSH_TEST_REIMPORT=<texture>` in the player edits that
texture's sidecar mid-run to exercise the whole watch → cook → swap chain
deterministically.

Phase 2 implementation notes (2026-07): BC chains must reach 1x1 — raylib
never clamps GL_TEXTURE_MAX_LEVEL, so a chain stopped at 4x4 is incomplete
under trilinear and samples black. Sub-block tail levels (2x2/1x1) encode
as one edge-clamped padded block, which matches raylib's mip-size walk
(w*h*bpp/8, clamped to a single block only when BOTH dims < 4) exactly;
chains with a non-4-aligned level (512x256 hits 4x2) cook uncompressed
with a warning. DXT5nm state persists in the .ctex header (reserved bit0)
and reaches the shader via BrushAssetsIsSwizzledNormal -> material props
-> uNormalSwizzled.
