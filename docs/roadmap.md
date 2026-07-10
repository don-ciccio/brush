# brush roadmap

brush is extracted from a working open-world game (private repo: "racer" /
Crop Circles, ~14K LOC), not written from scratch. Each system below already
exists and runs there; the work is decoupling it from that game's god struct
and content, then landing it here behind the layer/config APIs. Order follows
dependency, not glamour. The donor game keeps every system honest: nothing is
ported speculatively.

## Ground rules (learned the hard way in the donor codebase)

- **The worker thread never touches GL.** Streamed systems split CPU-build
  (background) from GPU-finalize (main thread) with an atomic handoff.
- **Determinism**: procedural functions are seeded world-space evaluations;
  neighbouring chunks must agree at shared edges.
- **Instanced draw-call count is the metric**: batch instances across chunks,
  never per-chunk.
- **Single sun authority**: day/night animates the one directional light.
- **raylib glTF loads prepend a default material at index 0** — always assign
  through `meshMaterial[]`.
- Engine never includes game code; every port keeps `make verify` green.

## v0 — done

Layered render pipeline (submit/execute, F2 term isolation), procedural sky
(Rayleigh/Mie + volumetric FBM clouds + stars), fixed-timestep loop with
render interpolation, orbit camera with auto-follow, action-based input
(keyboard+gamepad), F1 log console, zero-asset sandbox, screenshot harness.

Also landed early (was v1 item 8): **skeletal animation core** — b_anim ports
the donor animator's sampling/blending (frame-interpolated clip sampling,
per-bone lerp/slerp blending, phase-synced 1-D gait blend idle→walk→jog→sprint,
three-phase jump, snapshot cross-fades), driving the CC0 Quaternius mannequin
(assets/character/mannequin.glb, trimmed from UAL1 via headless Blender —
tools/trim_ual.py). Still to port later: look-at/foot IK, procedural leans,
landing absorption, 2-D strafe blends.

## v0.x — pipeline depth (ports from the donor)

1. **HDR post pipeline — core DONE** (b_post): linear RGBA16F scene target at
   an internal render scale, hue-preserving bright pass, 3-mip multi-scale
   bloom, ACES tone map + CDL grade + vibrance + vignette + film grain +
   Display P3 gamut map, CAS sharpen fused into the final upscale. Sky is back
   to linear HDR output; the lit shader linearizes sRGB albedo on the HDR path
   so tone mapping happens exactly once. F3 toggles; debug layer views bypass
   post. Still to port from the donor: **SSAO**, **SMAA**, DOF, god rays,
   volumetric fog (the scene target already keeps a sampleable depth texture
   for them).
2. ~~Shadow mapping~~ — DONE (b_shadow): depth-only ortho pass over the
   `BRUSH_LAYER_SHADOW` submissions, PCSS soft shadows in the lit shader
   (blocker search -> penumbra-widening PCF), light box follows the view
   target with texel snapping so edges don't crawl. F4 toggles; "sun shadow"
   joined the F2 layer views. Future work when the world grows: cascades.
3. ~~Render scale~~ — DONE, part of b_post (`BrushConfig.renderScale`,
   `BRUSH_RENDER_SCALE`). The HUD stays at native res.
4. **Time of day**: animated sun + moon driving sky, shadows, exposure.

## v1 — the open-world core

5. **Jolt physics** (vendored joltc): static/mesh/trigger bodies, raycasts,
   kinematic character controller; camera anti-clip raycast hook.
6. **Chunk-streamed world**: 64 m chunks, background generation worker,
   seeded heightmap terrain + collision, rebase seam (`WorldToRender`
   chokepoint), hysteresis load/unload.
7. **Instanced foliage**: per-layer scatter tables, unified cross-chunk
   instance batches, LOD decimation, trail interaction, horizon culling.
8. **Skeletal animation, advanced layer** (core landed in v0 — see above):
   look-at/foot IK, procedural leans + landing absorption, 2-D strafe blends,
   transition-condition tables.

## v1.x — foundation systems (new builds, not ports)

9. **Asset manager**: logical names -> paths via manifest, graceful fallbacks.
10. **Config**: settings file replacing env-var sprawl; data-driven bindings.
11. **World definition files**: seed, foliage tables, authored placements —
    a new game becomes data, not code.
12. **Audio**: bus mixer, 3D positional playback, ambient beds.
13. **Save/load**: versioned, section-registered.
14. **UI toolkit**: immediate-mode widgets grown out of the sandbox menu.

## v2 — RPG toolkit

Entity pool (chunk-resident spawn/despawn), interaction volumes ("press E"),
NPC behavior states + waypoint navigation, data-driven dialogue, quest flags,
inventory/stats.
