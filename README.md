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
| `SHADOW` | sun depth pass (reserved — shadow mapping port) |
| `OPAQUE` | the color pass: albedo × (ambient + diffuse sunlight) + specular |
| `SKY` | procedural atmosphere, drawn after opaque so early-Z rejects covered pixels |
| `TRANSPARENT` | alpha-blended, sorted back-to-front, depth-write off |
| `VOLUME` | volumetric fog / god rays (reserved) |

Games submit draws into layers (`BrushRenderSubmit`) and the engine executes
the stack (`BrushRenderExecute`). Press **F2** in the sandbox to isolate the
lighting terms of the opaque pass on screen — final / albedo / diffuse /
specular / normals. As the HDR post pipeline is ported, these debug views
become real intermediate render targets that later passes (bloom, SSAO, fog)
compose from.

## Build & run

Requires a C11 compiler and raylib (`brew install raylib` on macOS).

```bash
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
| **RMB-drag** / **Q E** / right stick | Orbit camera |
| **Wheel** | Zoom |
| **Tab** / Start | Menu |
| **F1** | Debug console (captures all logs) |
| **F2** | Cycle render layer view |

## Layout

```
engine/     the brush library — never includes game code
  brush.h     public umbrella header
  b_app       window + fixed-timestep loop (sim at 1/60 s, interpolated render)
  b_render    the layer stack + lit shader
  b_anim      skeletal animator: named clips, 1-D gait blend, jump phases
  b_sky       procedural atmospheric-scattering sky + FBM clouds
  b_camera    third-person orbit camera with hybrid auto-follow
  b_input     action-based input (keyboard + gamepad)
  b_console   F1 log console
  shaders/    GLSL (330)
sandbox/    the starter game — the template you copy to begin a new game
assets/     CC0 content only (Quaternius mannequin + animations)
docs/       roadmap and design notes
```

Environment toggles: `BRUSH_PERF=1` (uncapped fps + frame-time logs),
`BRUSH_AUTO_SCREENSHOT=1` (screenshot at frame 180, then exit),
`BRUSH_AUTO_MOVE=walk|jog|sprint` (hold forward input for captures).

## License

zlib/libpng — same as raylib.
