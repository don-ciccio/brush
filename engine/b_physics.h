/*******************************************************************************************
 *   b_physics.h - Physics world (Jolt Physics via joltc)
 *
 *   Thin engine facade over Jolt: one world with a static/moving/trigger
 *   layer scheme, static box and triangle-mesh colliders, sensor volumes,
 *   and raycasts. Step it from the fixed-timestep update.
 *
 *   Build note: run `make deps` once — it CMake-builds external/joltc (which
 *   fetches the Jolt sources) into static libs the makefile links.
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_PHYSICS_H
#define B_PHYSICS_H

#include <joltc.h>
#include <raylib.h>
#include <stdbool.h>

// Object layers
#define BRUSH_PHYS_LAYER_STATIC 0
#define BRUSH_PHYS_LAYER_MOVING 1
#define BRUSH_PHYS_LAYER_TRIGGER 2

// Broadphase layers
#define BRUSH_PHYS_BP_STATIC 0
#define BRUSH_PHYS_BP_MOVING 1

#define BRUSH_BODY_INVALID 0xFFFFFFFFu

typedef struct BrushPhysics {
  JPH_PhysicsSystem *system;
  JPH_BodyInterface *bodyInterface;
  JPH_TempAllocator *tempAllocator;
  JPH_JobSystem *jobSystem;
  JPH_BroadPhaseLayerInterface *bpInterface;
  JPH_ObjectVsBroadPhaseLayerFilter *objVsBpFilter;
  JPH_ObjectLayerPairFilter *objPairFilter;
} BrushPhysics;

bool BrushPhysicsInit(BrushPhysics *pw);
void BrushPhysicsStep(BrushPhysics *pw, float dt); // call per fixed step
void BrushPhysicsCleanup(BrushPhysics *pw); // destroy characters first

// Static axis-aligned box collider (props, floors, walls).
JPH_BodyID BrushPhysicsAddStaticBox(BrushPhysics *pw, Vector3 position,
                                    Vector3 size, int userData,
                                    const char *tag);

// Static triangle-mesh collider: `mesh` vertices are transformed by
// `transform` into world space (exact collision for arbitrary geometry).
JPH_BodyID BrushPhysicsAddStaticMesh(BrushPhysics *pw, Mesh mesh,
                                     Matrix transform, int userData,
                                     const char *tag);

// Sensor volume: overlaps report but never collide.
JPH_BodyID BrushPhysicsAddTriggerBox(BrushPhysics *pw, Vector3 position,
                                     Vector3 size, int userData,
                                     const char *tag);

void BrushPhysicsRemoveBody(BrushPhysics *pw, JPH_BodyID bodyID);

// Raycast against solid bodies. Returns true on hit with point/normal filled
// (either may be NULL).
bool BrushPhysicsRaycast(BrushPhysics *pw, Vector3 origin, Vector3 direction,
                         float maxDistance, Vector3 *hitPoint,
                         Vector3 *hitNormal);

#endif // B_PHYSICS_H
