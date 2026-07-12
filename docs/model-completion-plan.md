# Model Assets — Completion Plan

Models can be imported, placed, gizmo'd, textured (library material or their
own embedded diffuse), and collided with. But the feature is half-baked in
three ways the user named — indexed tangents, shape caching, instance
culling — plus two adjacents that fall out of the same work (embedded
normal maps, shadow-caster gating). This plan closes them without disturbing
the placement/scene/editor surface already shipped.

## Where it stands (grounded in the code)

* **Registry** (`BrushAssetsModel`, b_assets): ref-counted by path, binds
  the lit shader to every material, generates tangents ONLY for unindexed
  meshes (`indices == NULL`) — raylib's `GenMeshTangents` computes garbage
  on indexed data, so most glTF (indexed) gets NO tangents and its normal
  maps fall back to the geometric normal.
* **Colliders** (`BrushPhysicsAddStaticModel` → `AddStaticMesh` →
  `CookMeshShape`): `CookMeshShape(mesh, transform)` bakes the vertices
  into WORLD space using the full instance matrix, and `AddStaticShape`
  drops the body at the origin. So every instance cooks its own BVH at its
  own world position — **no two instances can share a cooked shape as
  structured today**, and cooking is synchronous on the main thread.
* **Rendering**: model instances submit every frame; the render execute
  loop draws every command with **no frustum or distance culling
  anywhere**. Shadow casters likewise submit unconditionally.
* **Materials on models**: a library material (triplanar/UV) works; a plain
  model draws its embedded diffuse (props == NULL). But props == NULL also
  means `uHasNormalMap = 0`, so a model that SHIPS its own normal map never
  gets normal-mapped even once tangents exist.

> **Status (2026-07-12):** P1, P2, P3 DONE and headless-verified (commits
> 813400a, a5f4792, 3b1ee95). P2.3 (async cook) and all of P4 remain deferred.

## Phase P1 — Correct rendering (tangents + embedded normals) — DONE

### P1.1 Indexed tangent generation
Replace the unindexed-only guard with a correct per-vertex generator
(Lengyel's method): for each triangle (walked via indices), derive the
tangent from the UV gradient, accumulate tangent + bitangent at its three
vertices; after all triangles, Gram-Schmidt-orthonormalise each vertex
tangent against its normal and store handedness in `.w`. Works for indexed
AND unindexed (unindexed = trivial index sequence).

Gotcha — the GPU upload: `LoadModel` already uploaded the mesh, so a mesh
that had no tangents has no tangent VBO. After filling `mesh.tangents`,
mirror what raylib's `GenMeshTangents` does at the end — bind the mesh VAO
and `rlLoadVertexBuffer` the tangents into the tangent attribute slot (or
`rlUpdateVertexBuffer` if it already exists). Without this the CPU tangents
never reach the shader.

### P1.2 Embedded normal maps
When a model draws WITHOUT a library material, detect an embedded normal
map (`model.materials[m].maps[MATERIAL_MAP_NORMAL].texture.id != 0`) and
submit props that enable the normal path (normal texture + `normalDepth`,
`triplanar = false` so it uses the mesh UVs + the P1.1 tangents). A small
`BrushSceneModelEmbeddedProps(model)`-style builder, used when
`BrushSceneModelProps` returns false. Embedded albedo already works; this
finishes "apply textures" for self-textured models (the common case for
downloaded assets).

**Proof**: a glTF rock/prop with a packed normal map lights with surface
detail (F2 normals view shows per-texel normals, not faceted geometry).

## Phase P2 — Shape caching (cook once, share) — DONE (P2.3 deferred)

The redesign that lets N instances of one model share one BVH:

### P2.1 Local-space cook + cache
Add `BrushPhysicsCookMeshShapeLocal(mesh)` that cooks in the mesh's OWN
space (no transform). Cache these keyed by `(path, meshIndex)`, ref-counted,
living in b_assets next to the model (natural asset lifetime):

```c
// +1 ref; cooks each mesh's shape once on first request. NULL if uncookable.
JPH_Shape *BrushAssetsModelShape(const char *path, int meshIndex);
void BrushAssetsReleaseModelShapes(const char *path); // drop this path's refs
```

### P2.2 Instance placement via ScaledShape
`AddStaticShape` currently spawns at origin (shapes are pre-world-baked).
Add `BrushPhysicsAddStaticShapeAt(base, pos, rotEuler, scale, ...)` that
wraps the cached base in a `JPH_ScaledShape` for the instance scale (Jolt
supports non-uniform; uniform-scale is the fast path / identity skips the
wrap) and creates the body at the instance's translate + rotation. It does
NOT consume the base ref — the cache owns it.

Rework `BrushPhysicsAddStaticModel` to take `(path, model, pos, rot, scale)`
and build bodies from cached base shapes. Editor/sandbox pass the instance's
components instead of the composed matrix. Net: placing 50 identical rocks
cooks 1 BVH, not 50 — and the per-instance body is cheap.

### P2.3 Async / lazy cook (removes the load hitch)
Cooking is thread-safe (already used off-thread for terrain). Cook a shape
on first request; for scene load, kick the whole set to the chunk worker
pattern (or a tiny job) and create bodies as they finish. Scoped small
because the cache already collapses repeat cooks — the remaining cost is
first-time unique cooks, worth hiding for scenes with many distinct props.

**Proof**: BRUSH harness places K copies of one model; log shows 1 cook,
K bodies; collision still exact (character stands on each); scene-load
frame time flat vs. today's per-instance spike.

## Phase P3 — Instance culling — DONE

### P3.1 Per-model bounds
Cache each model's local AABB (`GetModelBoundingBox` at load) in the
registry; expose a bounding sphere (center + radius). Per instance, the
world sphere = matrix·center, radius·maxAxisScale.

### P3.2 Reusable frustum test
The engine has no frustum test today. Add `BrushFrustum` — extract the 6
planes from `view*projection` once per frame — plus
`BrushFrustumContainsSphere(f, center, radius)`. This serves models now and
blocks/props later (a general win; the terrain keeps its cheaper
behind-camera test).

### P3.3 Cull the submits
Model draw loop (sandbox + editor): skip opaque submit when the instance
sphere fails the frustum or exceeds a draw distance; gate shadow submit by
a shadow distance (like terrain's `BRUSH_SHADOW_DIST`) — a model 400 m away
can't shadow the view. Keep it a game-side loop (models are game data), but
lean on the shared `BrushFrustum` helper so every consumer culls the same
way.

**Proof**: BRUSH_LOD_DBG-style counter logs submitted vs. total instances;
spinning the camera drops off-screen models from the count; fps holds with
a few hundred placed instances where today it would submit all of them.

## Phase P4 — Deferred (name them so they're not forgotten)

* **GPU instancing** (`DrawMeshInstanced`): one draw call for many copies of
  the same model — the foliage-scale optimisation. Per-instance draws +
  P3 culling are fine for tens–low-hundreds of static props; revisit when
  foliage lands and counts hit thousands.
* **Chunk-streamed model placement**: scene models are global (all resident
  at once). An open world wants props to stream with terrain chunks. Big
  feature; P3 culling covers the render cost meanwhile.
* **Convex / box collider option per instance**: trimesh is overkill for a
  crate. An instance flag choosing box/convex-hull/trimesh would cut
  collider cost, but needs UI + scene fields — separate small feature.

## Phasing table

| Phase | Deliverable | Proof |
|---|---|---|
| **P1** | correct indexed tangents (+VBO upload) + embedded normal-map props | self-textured glTF shows normal detail; indexed models light right |
| **P2** | local-space cook + (path,mesh) shape cache + ScaledShape placement + AddStaticModel rework; lazy cook | K copies = 1 cook / K bodies; collision exact; no load spike |
| **P3** | per-model bounds, BrushFrustum helper, opaque frustum+distance cull, shadow-distance gate | submitted-instance count drops off-screen; fps holds at scale |
| **P4** | (deferred) GPU instancing, chunk streaming, convex/box colliders | — |

## Gotchas to respect

* **Tangent VBO**: computing `mesh.tangents` isn't enough post-`LoadModel`;
  the tangent buffer must be loaded into the existing VAO or the shader
  reads a zero attribute (the current unindexed path already relies on
  raylib doing this — the custom generator must replicate it).
* **World→local cook flip**: switching `CookMeshShape` to local space means
  the body now carries the pos/rot/scale that the cooked vertices used to
  bake in. Every current caller (terrain chunks!) uses the world-space
  form — keep `CookMeshShape(mesh, transform)` for terrain and add the
  local variant for models, don't repoint terrain.
* **ScaledShape + negative/zero scale**: guard degenerate instance scales
  (clamp away from 0); Jolt asserts on some degenerate scales.
* **Shape cache lifetime**: base shapes are refcounted separately from the
  model's GL data — releasing a model must release its shapes too, and the
  editor's mesh-swap/duplicate/delete paths must ref/unref correctly (the
  same discipline the model registry already follows).
* **Frustum from the render camera**: build planes from the SAME
  view/projection the renderer uses (fov, aspect, near/far) or culling
  won't match what's on screen — reuse the renderer's matrices, don't
  recompute a divergent projection.
* Editor collider policy (remove-at-drag-start / rebuild-at-end) must keep
  working through the cache — rebuild goes through the cached path, and a
  gizmo drag that changes scale re-wraps the ScaledShape, not re-cooks.
* Thermal-throttle testing rule: baseline first, vsynced FPS for go/no-go.
