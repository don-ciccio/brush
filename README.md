# brush

A small C11 + [raylib](https://www.raylib.com/) engine for third-person
open-world games, built by extracting the proven systems out of a working game
(HDR pipeline, chunk-streamed terrain, skeletal animation, Jolt physics — see
the [roadmap](docs/roadmap.md) for what lands when).

The only binary asset in the repo is the CC0 [Quaternius](https://quaternius.com)
mannequin + Universal Animation Library clips (`assets/character/`); everything
else — the checkerboard, the blob shadow, the sky with its volumetric clouds —
is generated procedurally at runtime.

## Design: the image is built in layers

brush renders a frame as an explicit ordered stack of layers, like paint:

| Layer | Contents |
|---|---|
| `SHADOW` | sun depth pass — casters render into the shadow map; receivers get PCSS soft shadows |
| `OPAQUE` | the color pass: albedo × (ambient + diffuse sunlight) + specular |
| `SKY` | procedural atmosphere, drawn after opaque so early-Z rejects covered pixels |
| `TRANSPARENT` | alpha-blended, sorted back-to-front, depth-write off |
| `VOLUME` | volumetric fog / god rays (reserved) |

Games submit draws into layers (`BrushRenderSubmit`) and the engine executes
the stack (`BrushRenderExecute`) into a **linear HDR target** (RGBA16F, at a
configurable internal render scale). The post pipeline (`b_post`) then paints
the final image: multi-scale bloom -> ACES tone map -> colour grade ->
vignette/grain -> CAS-sharpened upscale to the backbuffer. Tone mapping
happens exactly once, so HDR highlights (the sun, bright sky) have real
headroom to bloom.

Press **F2** in the sandbox to isolate the lighting terms of the opaque pass —
final / albedo / diffuse / specular / normals / sun shadow — **F3** toggles
the HDR post pipeline and **F4** the sun shadows.

## Build & run

Requires a C11 compiler, CMake, and raylib (`brew install raylib cmake` on
macOS).

```bash
make deps   # once: builds vendored Jolt physics (fetches Jolt sources)
make        # builds build/libbrush.a + build/sandbox
make run    # run the sandbox
make verify # 180 frames -> screenshot.png -> exit (automated visual check)
```

Run from the repo root (shaders load from `engine/shaders/`).

## Controls

| Input | Action |
|---|---|
| **W A S D** / arrows / left stick | Move |
| **Shift** / pad X | Sprint |
| **Space** / pad A | Jump |
| **LCtrl** / pad R3 (hold) | Crouch |
| **R** / pad B | Roll |
| **RMB-drag** / **Q E** / right stick | Orbit camera |
| **Wheel** | Zoom |
| **Tab** / Start | Menu |
| **F1** | Debug console (captures all logs) |
| **F2** | Cycle render layer view |
| **F3** | Toggle HDR post pipeline |
| **F4** | Toggle sun shadows |
| **[** / **]** | Scrub the time of day |

## Layout

```
engine/     the brush library — never includes game code
  brush.h     public umbrella header
  b_app       window + fixed-timestep loop (sim at 1/60 s, interpolated render)
  b_render    the layer stack + lit shader
  b_post      HDR pipeline: bloom, ACES + grade composite, CAS upscale
  b_shadow    sun shadow map: ortho depth pass, texel-snapped follow, PCSS
  b_tod       day/night clock: solar geometry + swappable look palette
  b_world     chunk-streamed terrain: worker thread, game heightFn, colliders
              (flat + checker by default; BRUSH_HILLS for rolling hills)
  b_physics   Jolt physics world: static box/mesh colliders, triggers, raycasts
  b_character kinematic capsule controller: wall slide, slopes, stair steps
  b_anim      skeletal animator: gait/crouch blends, jump phases, roll,
              foot IK + pelvis drop, landing absorption (bones by name)
  b_sky       procedural atmospheric-scattering sky + FBM clouds
  b_camera    third-person orbit camera with hybrid auto-follow
  b_input     action-based input (keyboard + gamepad)
  b_console   F1 log console
  shaders/    GLSL (330)
sandbox/    the starter game — the template you copy to begin a new game
external/   vendored dependencies (joltc -> Jolt Physics)
assets/     CC0 content only (Quaternius mannequin + animations)
docs/       roadmap and design notes
```

Environment toggles: `BRUSH_PERF=1` (uncapped fps + frame-time logs),
`BRUSH_AUTO_SCREENSHOT=1` (screenshot at frame 180, then exit),
`BRUSH_AUTO_MOVE=walk|jog|sprint` (hold forward input for captures),
`BRUSH_NO_POST`, `BRUSH_RENDER_SCALE=0.75`, `BRUSH_EXPOSURE`,
`BRUSH_BLOOM_THRESH`, `BRUSH_BLOOM_INT`, `BRUSH_SHARPEN`, `BRUSH_NO_SHARPEN`,
`BRUSH_NO_P3` / `BRUSH_DISPLAY_P3`, `BRUSH_P3_STRENGTH`, `BRUSH_NO_SHADOW`,
`BRUSH_SHADOW_RES=2048`, `BRUSH_SHADOW_SOFT=4`, `BRUSH_TIME=<0..24h>`,
`BRUSH_DAY_LENGTH=<seconds>`.

## License

zlib/libpng — same as raylib.
