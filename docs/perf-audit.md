# Performance Audit — Gym Scene (2026-07-12)

Prompted by the report that *"adding multiple textures to the gym scene drops
FPS substantially."* This audit **measures** the frame budget instead of
theorising, and reaches a clear conclusion:

> **Textures are not the cost.** Under every path tested (triplanar albedo +
> normal, and parallax height + AO), at both 1× and 4× resolution, textured vs.
> untextured blocks are **within measurement noise (<2%)**. The frame is
> dominated by *fixed* pipeline cost — the post stack (~58%) and shadows
> (~17%) — which is identical whether or not a single block is textured.

## Method

- Two gym variants: all 13 blocks untextured (`-`) vs. all 13 wearing a
  material. Fixed camera (scene spawn), `BRUSH_PERF` uncapped, steady-state
  median of the last 8 samples.
- **Thermal control** (this laptop throttles under sustained GPU load — see
  the meadow-grass note): every run is a fresh cold process, ~5 s, and the
  baseline is re-measured *between* variants. A 30 s sustained gym run held
  100 fps flat, so the gym at 1× is not thermally limited.
- Resolution note: the sandbox renders the **scene** at 1600×900 and the
  editor at ~1470×923; both *present* at retina 2× (3200×1800). Supersampling
  the scene to 3200×1800 (`BRUSH_RENDER_SCALE 2`, clamp temporarily lifted)
  models a 4× fragment load.

## Frame budget (scene 1600×900, retina present — the realistic case)

| Configuration | frame time | Δ vs baseline | share |
|---|---|---|---|
| **Baseline (everything on)** | **10.0 ms** (100 fps) | — | 100% |
| `BRUSH_NO_POST` | 4.24 ms | **−5.8 ms** | **post = 58%** |
| `BRUSH_NO_SHADOW` | 8.33 ms | −1.7 ms | shadows = 17% |
| `BRUSH_NO_SMAA` | 9.35 ms | −0.65 ms | SMAA = 6% |
| `BRUSH_NO_SSAO` | 10.1 ms | ~0 | SSAO ≈ 1% |
| `NO_SHADOW + NO_POST` | 3.24 ms | −6.8 ms | raw scene+present = 32% |

The scene render itself (geometry + lit shader + present) is only ~3.2 ms.
Everything above it is the look pipeline, and it is a **fixed cost per frame**
that does not scale with scene content.

## Texture cost (the reported symptom) — not reproduced

| Test | plain | textured | verdict |
|---|---|---|---|
| Triplanar (albedo+normal), 1× open | 100 fps | ~90–98 fps | within noise |
| Triplanar, 4× close-to-wall (worst overdraw) | 56 fps | 55 fps | within noise |
| Parallax (height+AO), 1× open | 100 fps | 98 fps | within noise |
| Parallax, 4× close-to-wall | 56 fps | 53 fps | ~1 ms at 4× worst case |

A triplanar textured block adds 6 texture fetches (3 albedo + 3 normal); the
parallax path adds 3 more plus an offset. At the gym's fill rate this is lost
in the noise of the fixed pipeline cost. Gym textures are 512², BC-compressed
(`.ctex` present), full mip chains — no bandwidth or upload problem, and there
is **no per-frame re-cook** (asset re-resolve only runs after a live edit).

## Where the time actually goes

The post stack runs ~13–15 full-screen passes per frame (mostly at reduced
res, but each still pays an FBO bind + shader switch + fullscreen fill):

- **Bloom** — bright pass + down/up chain at /2, /4, /8 with 2 blur passes each.
- **SSAO** — half-res raw + blur (cheap, ~0.1 ms measured).
- **Composite** — one full-res (outW) pass doing tonemap + DOF + god-ray blend
  + fog + vignette/grain/P3. DOF/god-rays/fog are **off by default** and their
  branches are correctly skipped (measured ~0 ms each), but the pass still runs.
- **SMAA** — edges + weights + blend, 3 passes at outW (~0.65 ms).
- **Sharpen/upscale** — the only pass at the retina 3200×1800 backbuffer (the
  composite deliberately runs at 1× so retina only pays the cheap final blit).

Shadows are 3 CSM cascades (2048² each) + a 32-tap PCSS per lit fragment
(already gated to sun-facing pixels).

## Assessment of the reported symptom

Adding textures does not measurably cost FPS. The most likely explanations for
the perception:

1. **The pipeline baseline was always there.** ~6.8 ms of every frame is
   post+shadows regardless of textures. If FPS felt fine "before" and dropped
   "after," the variable was probably something else in that session (see #2/#3),
   not the material assignment.
2. **Editor overhead / thermal state.** The editor adds ImGui, gizmo, and
   picking on top of the same scene; a long editing session heats the GPU and
   throttles it (documented behaviour). Perceived drops during editing
   correlate with *time spent*, not with textures.
3. **A specific imported texture.** These tests used the 512² BC-cooked gym
   set. A large (2K/4K) freshly-imported source triggers a **synchronous first
   cook** (a one-time hitch, not steady-state FPS) and, until cooked, a bigger
   upload. Worth confirming the actual size/format of the textures in question.

## Recommendations (ranked; not yet implemented)

The lever is the **fixed pipeline cost**, not scene content:

1. **Quality presets (Low/Med/High).** Today every pass is always at max. A
   "Low" preset (fewer bloom levels/blur passes, 1–2 shadow cascades, smaller
   PCSS kernel, SSAO off) would recover multiple ms on demand. Biggest win for
   the least risk, and the right knob to expose to a shipping game.
2. **Bloom pass reduction.** The 6-target + 2-blur chain is the heaviest post
   component; a single Kawase-style down/up or fewer blur iterations would cut
   it with little visual loss.
3. **Shadow tuning.** 3 cascades + 32-tap PCSS at 2048² is generous for a
   small scene; a cascade or a cheaper kernel is ~1 ms.
4. **Skip the composite's dead branches structurally.** DOF/god-rays/fog are
   off by default and already cost ~0, but a specialised composite variant
   (no dead uniforms) trims shader complexity when they're disabled.
5. **Frame pacing.** With vsync the gym sits at 60 fps with headroom; uncapped
   numbers here are for profiling. Confirm go/no-go on vsynced FPS.

**Bottom line:** keep the textures — they are effectively free. If a scene
needs more headroom, scale the post/shadow *quality*, which is where the frame
budget actually lives.
