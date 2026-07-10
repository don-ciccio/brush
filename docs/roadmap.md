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
tools/trim_ual.py). Since extended with the motion-starter set (crouch idle/walk state, roll
one-shot) and REAL foot IK — the standard pipeline (cf. ozz-animation's
foot_ik sample): ground queried under the ANIMATED feet via a game callback,
pelvis lowers to the lowest reachable foot (pelvisOffset output), analytic
two-bone leg IK (law of cosines, knee plane preserved, model-space rotations
through the parent chain), ankles aimed to the ground normal. Landing
absorption rides on the same IK (pelvis dips, feet stay planted).
BRUSH_ANIM_LAND pins the dip.
Still to port later: look-at IK, procedural leans, 2-D strafe blends (note:
the UAL packs ship no strafe clips — donor used procedural lean for lateral).

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
4. ~~Time of day~~ — DONE (b_tod): 0..24h clock -> sun/moon directions from
   real solar geometry (hour angle, seasonal declination, latitude), and an
   elevation-keyed LUT palette (sun color, ambient color+level, fog color,
   exposure). The palette is per-instance data — games swap in their own
   keyframes to restyle the cycle; engine ships a neutral default (the donor
   game's tuned meadow palette stays in the donor).
   `BrushRenderApplyTimeOfDay` animates the single light authority: sun by
   day, the moon takes the same directional slot at night (both colors ramp
   through black at the horizon so the handover never pops). Ambient
   generalized from a scalar to a color (indigo night fill). BRUSH_TIME /
   BRUSH_DAY_LENGTH envs; sandbox scrubs with [ ]. Fog color query is ready
   for the volumetric fog port.

## v1 — the open-world core

6.5. ~~Chunk-streamed world~~ — DONE (b_world): 64 m chunks, pthread worker
   (CPU build: heightmap + displaced terrain mesh; main-thread GPU upload +
   per-chunk Jolt collider), lock-free atomic handoff (raylib #827 pattern),
   load/unload rings with hysteresis, rebase-seam chokepoint (identity),
   frustum cull, bilinear ground-height query. GENERALIZED: the terrain
   surface is a game-supplied `heightFn(user, wx, wz)` (racer's ridge/road/
   flatten logic is the game's business now); chunk size/radius/resolution
   are config; terrain draws through the engine lit shader + shadow layers
   via a new BrushRenderSubmitMesh. Zero-asset default: slope vertex-colour
   shading (green lowland / grey rock). Sandbox streams rolling hills with
   the character + camera on the ground query.  STILL TODO here: horizon
   occlusion cull, LOD rings, and the foliage system (next). — DONE: joltc vendored (source-only, `make deps`
   CMake-builds it + fetches Jolt). b_physics ports the donor's facade
   (static box + exact triangle-mesh colliders, sensor volumes, raycasts,
   layer scheme); b_character ports the CharacterVirtual kinematic capsule
   (wall sliding, slope limits, stair climbing — step height now a tunable
   instead of a donor-specific constant). Orbit camera gained an obstruction
   hook (physics raycast keeps geometry from cutting between camera and
   focus). Sandbox: obstacle course (crates + a rotated-mesh ramp) with the
   character fully on the controller. Future: trigger-overlap callbacks,
   dynamic bodies, Jolt debug draw.
6. **Chunk-streamed world**: 64 m chunks, background generation worker,
   seeded heightmap terrain + collision, rebase seam (`WorldToRender`
   chokepoint), hysteresis load/unload.
7. **Instanced foliage**: per-layer scatter tables, unified cross-chunk
   instance batches, LOD decimation, trail interaction, horizon culling.
8. **Skeletal animation, advanced layer** (core + foot IK + landing
   absorption + crouch/roll landed — see above): look-at IK, procedural
   leans, 2-D strafe blends, transition-condition tables.

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
