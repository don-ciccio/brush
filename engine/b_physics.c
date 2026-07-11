/*******************************************************************************************
 *   b_physics.c - Physics world (Jolt Physics via joltc), see b_physics.h
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_physics.h"
#include <raymath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Object-layer filter for queries: SOLID geometry only. Sensor/trigger
// volumes must never block raycasts — the camera anti-clip, foot-IK probes,
// and mantle detection would all collide with invisible gameplay volumes.
static bool JPH_API_CALL SolidShouldCollide(void *userData,
                                            JPH_ObjectLayer layer) {
    (void)userData;
    return layer != BRUSH_PHYS_LAYER_TRIGGER;
}

bool BrushPhysicsInit(BrushPhysics *pw) {
    if (!pw) return false;
    memset(pw, 0, sizeof(BrushPhysics));

    // 1. Initialize Jolt global allocator and type registries
    if (!JPH_Init()) {
        TraceLog(LOG_ERROR, "JoltC: JPH_Init failed");
        return false;
    }

    // 2. Create JobSystem
    JobSystemThreadPoolConfig jobConfig;
    jobConfig.maxJobs = JPH_MAX_PHYSICS_JOBS;
    jobConfig.maxBarriers = JPH_MAX_PHYSICS_BARRIERS;
    jobConfig.numThreads = -1; // -1 means auto-detect (use all available cores)
    pw->jobSystem = JPH_JobSystemThreadPool_Create(&jobConfig);
    if (!pw->jobSystem) {
        TraceLog(LOG_ERROR, "JoltC: JPH_JobSystemThreadPool_Create failed");
        return false;
    }

    // 3. Create TempAllocator
    pw->tempAllocator = JPH_TempAllocator_Create(10 * 1024 * 1024); // 10 MB buffer
    if (!pw->tempAllocator) {
        TraceLog(LOG_ERROR, "JoltC: JPH_TempAllocator_Create failed");
        return false;
    }

    // 4. Set up Layer Filtering
    // Table interfaces map ObjectLayers to BroadPhaseLayers and define pair collisions.
    pw->bpInterface = JPH_BroadPhaseLayerInterfaceTable_Create(3, 2);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(pw->bpInterface, BRUSH_PHYS_LAYER_STATIC, BRUSH_PHYS_BP_STATIC);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(pw->bpInterface, BRUSH_PHYS_LAYER_MOVING, BRUSH_PHYS_BP_MOVING);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(pw->bpInterface, BRUSH_PHYS_LAYER_TRIGGER, BRUSH_PHYS_BP_STATIC);

    pw->objPairFilter = JPH_ObjectLayerPairFilterTable_Create(3);
    JPH_ObjectLayerPairFilterTable_DisableCollision(pw->objPairFilter, BRUSH_PHYS_LAYER_STATIC, BRUSH_PHYS_LAYER_STATIC);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pw->objPairFilter, BRUSH_PHYS_LAYER_STATIC, BRUSH_PHYS_LAYER_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pw->objPairFilter, BRUSH_PHYS_LAYER_STATIC, BRUSH_PHYS_LAYER_TRIGGER);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pw->objPairFilter, BRUSH_PHYS_LAYER_MOVING, BRUSH_PHYS_LAYER_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pw->objPairFilter, BRUSH_PHYS_LAYER_MOVING, BRUSH_PHYS_LAYER_TRIGGER);
    JPH_ObjectLayerPairFilterTable_DisableCollision(pw->objPairFilter, BRUSH_PHYS_LAYER_TRIGGER, BRUSH_PHYS_LAYER_TRIGGER);

    pw->objVsBpFilter = JPH_ObjectVsBroadPhaseLayerFilterTable_Create(
        pw->bpInterface, 2,
        pw->objPairFilter, 3
    );

    // 5. Create Physics System
    JPH_PhysicsSystemSettings settings;
    settings.maxBodies = 2048;
    settings.numBodyMutexes = 0;
    settings.maxBodyPairs = 4096;
    settings.maxContactConstraints = 2048;
    settings._padding = 0;
    settings.broadPhaseLayerInterface = pw->bpInterface;
    settings.objectLayerPairFilter = pw->objPairFilter;
    settings.objectVsBroadPhaseLayerFilter = pw->objVsBpFilter;

    pw->system = JPH_PhysicsSystem_Create(&settings);
    if (!pw->system) {
        TraceLog(LOG_ERROR, "JoltC: JPH_PhysicsSystem_Create failed");
        return false;
    }

    // Cache the body interface for adding/removing geometry
    pw->bodyInterface = JPH_PhysicsSystem_GetBodyInterface(pw->system);

    // 6. Query filter: raycasts collide with solid layers only (see
    // SolidShouldCollide). CAREFUL: SetProcs stores the POINTER (joltc keeps
    // s_Procs = procs), so the procs table must outlive every query — static,
    // never stack-local (a local here SIGBUSes on the first raycast).
    static const JPH_ObjectLayerFilter_Procs solidProcs = {
        .ShouldCollide = SolidShouldCollide,
    };
    JPH_ObjectLayerFilter_SetProcs(&solidProcs);
    pw->solidFilter = JPH_ObjectLayerFilter_Create(NULL);

    TraceLog(LOG_INFO, "JoltC: Centralized BrushPhysics initialized successfully");
    return true;
}

void BrushPhysicsStep(BrushPhysics *pw, float dt) {
    if (!pw || !pw->system) return;
    // Step Jolt physics using TempAllocator and JobSystem
    JPH_PhysicsSystem_Update2(pw->system, dt, 1, pw->tempAllocator, pw->jobSystem);
}

void BrushPhysicsCleanup(BrushPhysics *pw) {
    if (!pw) return;

    // BrushCharacterCleanup must be called BEFORE this to free the virtual character.
    //
    // JPH_PhysicsSystem_Destroy frees the broadPhaseLayerInterface,
    // objectVsBroadPhaseLayerFilter, and objectLayerPairFilter internally
    // (see joltc.cpp:1044-1046). Do NOT call their Destroy functions — that
    // would double-free and crash.

    if (pw->solidFilter) {
        JPH_ObjectLayerFilter_Destroy(pw->solidFilter);
        pw->solidFilter = NULL;
    }
    if (pw->system) {
        JPH_PhysicsSystem_Destroy(pw->system);
        pw->system = NULL;
    }
    // These are freed by JPH_PhysicsSystem_Destroy — just NULL our handles.
    pw->bodyInterface = NULL;
    pw->objVsBpFilter = NULL;
    pw->objPairFilter = NULL;
    pw->bpInterface = NULL;

    if (pw->tempAllocator) {
        JPH_TempAllocator_Destroy(pw->tempAllocator);
        pw->tempAllocator = NULL;
    }
    if (pw->jobSystem) {
        JPH_JobSystem_Destroy(pw->jobSystem);
        pw->jobSystem = NULL;
    }

    JPH_Shutdown();
    TraceLog(LOG_INFO, "JoltC: Centralized BrushPhysics shut down and cleaned up");
}

JPH_BodyID BrushPhysicsAddStaticBox(BrushPhysics *pw, Vector3 position, Vector3 size, Vector3 rotation, int userData, const char *tag) {
    if (!pw || !pw->bodyInterface) return BRUSH_BODY_INVALID;

    // Jolt box shapes are defined by half-extents
    JPH_Vec3 halfExtents = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    JPH_BoxShapeSettings* boxSettings = JPH_BoxShapeSettings_Create(&halfExtents, 0.05f);
    JPH_BoxShape* boxShape = JPH_BoxShapeSettings_CreateShape(boxSettings);
    JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)boxSettings); // Free settings memory after shape is created!
    
    // Create creation settings
    JPH_RVec3 pos = { position.x, position.y, position.z };
    // Euler XYZ order — must match BrushEulerXYZ (b_scene) and ImGuizmo, or
    // the collider silently diverges from the rendered block.
    Matrix rm = MatrixMultiply(MatrixMultiply(MatrixRotateX(rotation.x * DEG2RAD),
                                              MatrixRotateY(rotation.y * DEG2RAD)),
                               MatrixRotateZ(rotation.z * DEG2RAD));
    Quaternion q = QuaternionFromMatrix(rm);
    JPH_Quat rot = { q.x, q.y, q.z, q.w };
    JPH_BodyCreationSettings* bodySettings = JPH_BodyCreationSettings_Create3(
        (const JPH_Shape*)boxShape,
        &pos,
        &rot,
        JPH_MotionType_Static,
        BRUSH_PHYS_LAYER_STATIC
    );

    JPH_BodyCreationSettings_SetUserData(bodySettings, (uint64_t)userData);

    // Create and add body
    JPH_BodyID bodyID = JPH_BodyInterface_CreateAndAddBody(pw->bodyInterface, bodySettings, JPH_Activation_DontActivate);

    // Clean up temporary settings object
    JPH_BodyCreationSettings_Destroy(bodySettings);

    if (bodyID == BRUSH_BODY_INVALID) {
        TraceLog(LOG_WARNING, "JoltC: Failed to create static box collider '%s'", tag ? tag : "unnamed");
    } else {
        TraceLog(LOG_INFO, "JoltC: Registered static box '%s' (BodyID: %u) at (%.1f, %.1f, %.1f)", 
                 tag ? tag : "unnamed", bodyID, position.x, position.y, position.z);
    }

    return bodyID;
}

JPH_Shape *BrushPhysicsCookMeshShape(Mesh mesh, Matrix transform) {
    if (mesh.vertexCount == 0 || mesh.triangleCount == 0) return NULL;

    // Convert Raylib mesh triangles directly to JoltC triangles
    uint32_t triangleCount = mesh.triangleCount;
    JPH_Triangle* joltTriangles = (JPH_Triangle*)malloc(sizeof(JPH_Triangle) * triangleCount);

    for (uint32_t i = 0; i < triangleCount; i++) {
        int idx0, idx1, idx2;
        if (mesh.indices) {
            idx0 = mesh.indices[i * 3 + 0];
            idx1 = mesh.indices[i * 3 + 1];
            idx2 = mesh.indices[i * 3 + 2];
        } else {
            idx0 = i * 3 + 0;
            idx1 = i * 3 + 1;
            idx2 = i * 3 + 2;
        }

        Vector3 v0 = { mesh.vertices[idx0 * 3 + 0], mesh.vertices[idx0 * 3 + 1], mesh.vertices[idx0 * 3 + 2] };
        Vector3 v1 = { mesh.vertices[idx1 * 3 + 0], mesh.vertices[idx1 * 3 + 1], mesh.vertices[idx1 * 3 + 2] };
        Vector3 v2 = { mesh.vertices[idx2 * 3 + 0], mesh.vertices[idx2 * 3 + 1], mesh.vertices[idx2 * 3 + 2] };

        // Apply visual transformation matrix directly to vertices to put them in world space
        v0 = Vector3Transform(v0, transform);
        v1 = Vector3Transform(v1, transform);
        v2 = Vector3Transform(v2, transform);

        joltTriangles[i].v1 = *(JPH_Vec3*)&v0;
        joltTriangles[i].v2 = *(JPH_Vec3*)&v1;
        joltTriangles[i].v3 = *(JPH_Vec3*)&v2;
        joltTriangles[i].materialIndex = 0;
    }

    // Cook the BVH-optimized mesh shape — the expensive step, and the reason
    // this function exists separately: it touches no physics-world state, so
    // streaming workers call it off the main thread.
    JPH_MeshShapeSettings* meshSettings = JPH_MeshShapeSettings_Create(joltTriangles, triangleCount);
    JPH_MeshShape* meshShape = JPH_MeshShapeSettings_CreateShape(meshSettings);
    JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)meshSettings);
    free(joltTriangles);
    return (JPH_Shape*)meshShape;
}

void BrushPhysicsReleaseShape(JPH_Shape *shape) {
    if (shape) JPH_Shape_Destroy(shape); // drops our reference (refcounted)
}

JPH_BodyID BrushPhysicsAddStaticShape(BrushPhysics *pw, JPH_Shape *shape, int userData, const char *tag) {
    if (!pw || !pw->bodyInterface || !shape) return BRUSH_BODY_INVALID;

    // Pre-cooked shapes are already in world coordinates: spawn at origin.
    JPH_RVec3 pos = { 0.0f, 0.0f, 0.0f };
    JPH_Quat rot = { 0.0f, 0.0f, 0.0f, 1.0f };
    JPH_BodyCreationSettings* bodySettings = JPH_BodyCreationSettings_Create3(
        shape,
        &pos,
        &rot,
        JPH_MotionType_Static,
        BRUSH_PHYS_LAYER_STATIC
    );

    JPH_BodyCreationSettings_SetUserData(bodySettings, (uint64_t)userData);

    JPH_BodyID bodyID = JPH_BodyInterface_CreateAndAddBody(pw->bodyInterface, bodySettings, JPH_Activation_DontActivate);

    JPH_BodyCreationSettings_Destroy(bodySettings);
    // The body holds its own shape reference; drop the caller's (this API
    // consumes it). Without this every recycled chunk leaked a cooked shape.
    JPH_Shape_Destroy(shape);

    if (bodyID == BRUSH_BODY_INVALID) {
        TraceLog(LOG_WARNING, "JoltC: Failed to create static shape body '%s'", tag ? tag : "unnamed");
    } else {
        TraceLog(LOG_INFO, "JoltC: Registered static shape '%s' (BodyID: %u)",
                 tag ? tag : "unnamed", bodyID);
    }

    return bodyID;
}

JPH_BodyID BrushPhysicsAddStaticMesh(BrushPhysics *pw, Mesh mesh, Matrix transform, int userData, const char *tag) {
    if (!pw || !pw->bodyInterface) return BRUSH_BODY_INVALID;
    JPH_Shape *shape = BrushPhysicsCookMeshShape(mesh, transform);
    if (!shape) return BRUSH_BODY_INVALID;
    return BrushPhysicsAddStaticShape(pw, shape, userData, tag);
}

JPH_BodyID BrushPhysicsAddTriggerBox(BrushPhysics *pw, Vector3 position, Vector3 size, int userData, const char *tag) {
    if (!pw || !pw->bodyInterface) return BRUSH_BODY_INVALID;

    JPH_Vec3 halfExtents = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    JPH_BoxShapeSettings* boxSettings = JPH_BoxShapeSettings_Create(&halfExtents, 0.05f);
    JPH_BoxShape* boxShape = JPH_BoxShapeSettings_CreateShape(boxSettings);
    JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)boxSettings); // Free settings memory after shape is created!
    
    JPH_RVec3 pos = { position.x, position.y, position.z };
    JPH_Quat rot = { 0.0f, 0.0f, 0.0f, 1.0f };
    JPH_BodyCreationSettings* bodySettings = JPH_BodyCreationSettings_Create3(
        (const JPH_Shape*)boxShape,
        &pos,
        &rot,
        JPH_MotionType_Static,
        BRUSH_PHYS_LAYER_TRIGGER
    );

    // Mark body as sensor (trigger volume)
    JPH_BodyCreationSettings_SetIsSensor(bodySettings, true);
    JPH_BodyCreationSettings_SetUserData(bodySettings, (uint64_t)userData);

    JPH_BodyID bodyID = JPH_BodyInterface_CreateAndAddBody(pw->bodyInterface, bodySettings, JPH_Activation_DontActivate);

    JPH_BodyCreationSettings_Destroy(bodySettings);

    if (bodyID == BRUSH_BODY_INVALID) {
        TraceLog(LOG_WARNING, "JoltC: Failed to create trigger box '%s'", tag ? tag : "unnamed");
    } else {
        TraceLog(LOG_INFO, "JoltC: Registered trigger box '%s' (BodyID: %u) at (%.1f, %.1f, %.1f)", 
                 tag ? tag : "unnamed", bodyID, position.x, position.y, position.z);
    }

    return bodyID;
}

void BrushPhysicsRemoveBody(BrushPhysics *pw, JPH_BodyID bodyID) {
    if (!pw || !pw->bodyInterface || bodyID == BRUSH_BODY_INVALID) return;
    
    if (JPH_BodyInterface_IsAdded(pw->bodyInterface, bodyID)) {
        JPH_BodyInterface_RemoveAndDestroyBody(pw->bodyInterface, bodyID);
        TraceLog(LOG_INFO, "JoltC: Removed and destroyed BodyID: %u", bodyID);
    }
}

bool BrushPhysicsRaycast(BrushPhysics *pw, Vector3 origin, Vector3 direction, float maxDistance, Vector3 *hitPoint, Vector3 *hitNormal) {
    return BrushPhysicsRaycastEx(pw, origin, direction, maxDistance, hitPoint, hitNormal, NULL);
}

bool BrushPhysicsRaycastEx(BrushPhysics *pw, Vector3 origin, Vector3 direction, float maxDistance, Vector3 *hitPoint, Vector3 *hitNormal, JPH_BodyID *hitBodyID) {
    if (!pw || !pw->system) return false;

    const JPH_NarrowPhaseQuery* query = JPH_PhysicsSystem_GetNarrowPhaseQuery(pw->system);
    if (!query) return false;

    JPH_RVec3 start = { origin.x, origin.y, origin.z };
    JPH_Vec3 dir = { direction.x * maxDistance, direction.y * maxDistance, direction.z * maxDistance }; // Raycast direction is magnitude-based in Jolt
    JPH_RayCastResult hit;
    memset(&hit, 0, sizeof(JPH_RayCastResult));

    // Solid layers only: sensor/trigger volumes never block a ray (camera
    // anti-clip, foot-IK probes, and mantle detection must see through them).
    bool hasHit = JPH_NarrowPhaseQuery_CastRay(query, &start, &dir, &hit, NULL,
                                               pw->solidFilter, NULL);

    if (hasHit && hit.bodyID != BRUSH_BODY_INVALID) {
        float fraction = hit.fraction;
        if (hitPoint) {
            hitPoint->x = origin.x + direction.x * maxDistance * fraction;
            hitPoint->y = origin.y + direction.y * maxDistance * fraction;
            hitPoint->z = origin.z + direction.z * maxDistance * fraction;
        }

        if (hitNormal) {
            const JPH_Body* body = JPH_PhysicsSystem_GetBodyPtr(pw->system, hit.bodyID);
            if (body) {
                JPH_RVec3 hitPt = {
                    origin.x + direction.x * maxDistance * fraction,
                    origin.y + direction.y * maxDistance * fraction,
                    origin.z + direction.z * maxDistance * fraction
                };
                JPH_Vec3 normal;
                // JPH_Body_GetWorldSpaceSurfaceNormal retrieves the normal in JPH_Vec3 format
                JPH_Body_GetWorldSpaceSurfaceNormal(body, hit.subShapeID2, &hitPt, &normal);
                *hitNormal = *(Vector3*)&normal;
            } else {
                *hitNormal = (Vector3){ 0.0f, 1.0f, 0.0f };
            }
        }
        if (hitBodyID) {
            *hitBodyID = hit.bodyID;
        }
        return true;
    }

    return false;
}
