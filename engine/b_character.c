/*******************************************************************************************
 *   b_character.c - Kinematic character controller (see b_character.h)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#include "b_character.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

bool BrushCharacterInit(BrushCharacter *c, BrushPhysics *pw, Vector3 startPos, float radius, float totalHeight) {
    if (!c || !pw) return false;
    memset(c, 0, sizeof(BrushCharacter));

    // Calculate cylinder half-height. A capsule is defined by a cylinder + 2 hemispheres.
    // totalHeight = 2 * radius + 2 * halfHeightOfCylinder
    float halfHeightOfCylinder = (totalHeight * 0.5f) - radius;
    if (halfHeightOfCylinder <= 0.0f) {
        TraceLog(LOG_ERROR, "BrushCharacterInit: Invalid capsule dimensions (height too small for radius)");
        return false;
    }

    // Create Capsule Shape
    JPH_CapsuleShapeSettings* capsuleSettings = JPH_CapsuleShapeSettings_Create(halfHeightOfCylinder, radius);
    c->capsuleShape = (JPH_Shape*)JPH_CapsuleShapeSettings_CreateShape(capsuleSettings);
    JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)capsuleSettings); // Free settings memory after shape is created!
    if (!c->capsuleShape) {
        TraceLog(LOG_ERROR, "BrushCharacterInit: Failed to create capsule shape");
        return false;
    }

    // Character position is centered at its Center Of Mass (COM)
    c->comOffset = radius + halfHeightOfCylinder;
    JPH_RVec3 comPos = { startPos.x, startPos.y + c->comOffset, startPos.z };
    JPH_Quat comRot = { 0.0f, 0.0f, 0.0f, 1.0f }; // Identity rotation

    // Setup Virtual Character Settings
    JPH_CharacterVirtualSettings settings;
    JPH_CharacterVirtualSettings_Init(&settings);
    settings.base.shape = c->capsuleShape;
    settings.base.up = (JPH_Vec3){ 0.0f, 1.0f, 0.0f };
    settings.base.maxSlopeAngle = 50.0f * DEG2RAD; // radians, not cosine!
    settings.mass = 80.0f;
    settings.maxStrength = 100.0f;
    settings.predictiveContactDistance = 0.1f;
    settings.characterPadding = 0.04f;
    settings.maxNumHits = 16;
    settings.penetrationRecoverySpeed = 1.0f;

    c->character = JPH_CharacterVirtual_Create(&settings, &comPos, &comRot, 0, pw->system);
    if (!c->character) {
        TraceLog(LOG_ERROR, "BrushCharacterInit: Failed to create virtual character");
        return false;
    }

    c->position = startPos;
    c->velocity = (Vector3){ 0.0f, 0.0f, 0.0f };
    c->stepUp = 0.45f;    // fits typical 30cm risers with the headroom Jolt's
                          // stair-walk needs above the step
    c->stickDown = 0.5f;
    c->isGrounded = true;
    c->groundNormal = (Vector3){ 0.0f, 1.0f, 0.0f };

    TraceLog(LOG_INFO, "JoltC: Kinematic Virtual Character initialized at (%.1f, %.1f, %.1f)", 
             startPos.x, startPos.y, startPos.z);
    return true;
}

void BrushCharacterCleanup(BrushCharacter *c, BrushPhysics *pw) {
    (void)pw;
    if (!c) return;

    if (c->character) {
        JPH_CharacterBase_Destroy((JPH_CharacterBase*)c->character);
        c->character = NULL;
    }
    // Capsule shape is owned by the virtual character; JPH_CharacterBase_Destroy handles releasing it.
    c->capsuleShape = NULL;
}

bool BrushCharacterSetDimensions(BrushCharacter *c, BrushPhysics *pw, float radius, float totalHeight) {
    if (!c || !c->character || !pw || !pw->system) return false;

    float halfHeightOfCylinder = (totalHeight * 0.5f) - radius;
    if (halfHeightOfCylinder <= 0.0f) {
        TraceLog(LOG_ERROR, "BrushCharacterSetDimensions: Invalid capsule dimensions (height too small for radius)");
        return false;
    }

    // Create new Capsule Shape
    JPH_CapsuleShapeSettings* capsuleSettings = JPH_CapsuleShapeSettings_Create(halfHeightOfCylinder, radius);
    JPH_Shape* newShape = (JPH_Shape*)JPH_CapsuleShapeSettings_CreateShape(capsuleSettings);
    JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)capsuleSettings);
    if (!newShape) {
        TraceLog(LOG_ERROR, "BrushCharacterSetDimensions: Failed to create capsule shape");
        return false;
    }

    float oldOffset = c->comOffset;
    float newOffset = radius + halfHeightOfCylinder;

    // Get current COM position
    JPH_RVec3 comPos;
    JPH_CharacterVirtual_GetPosition(c->character, &comPos);

    // Adjust position so that feet position remains constant
    JPH_RVec3 newComPos = {
        comPos.x,
        comPos.y - oldOffset + newOffset,
        comPos.z
    };

    JPH_CharacterVirtual_SetPosition(c->character, &newComPos);

    bool ok = JPH_CharacterVirtual_SetShape(
        c->character,
        newShape,
        1.5f,
        BRUSH_PHYS_LAYER_MOVING,
        pw->system,
        NULL,
        NULL
    );

    if (ok) {
        if (c->capsuleShape) {
            JPH_Shape_Destroy(c->capsuleShape);
        }
        c->capsuleShape = newShape;
        c->comOffset = newOffset;
        c->position = (Vector3){ (float)newComPos.x, (float)newComPos.y - newOffset, (float)newComPos.z };
        return true;
    } else {
        // Rollback position and destroy the new shape
        JPH_CharacterVirtual_SetPosition(c->character, &comPos);
        JPH_Shape_Destroy(newShape);
        return false;
    }
}

void BrushCharacterWarp(BrushCharacter *c, Vector3 feetPos) {
    if (!c || !c->character) return;
    JPH_RVec3 comPos = { feetPos.x, feetPos.y + c->comOffset, feetPos.z };
    JPH_CharacterVirtual_SetPosition(c->character, &comPos);
    JPH_Vec3 zero = { 0.0f, 0.0f, 0.0f };
    JPH_CharacterVirtual_SetLinearVelocity(c->character, &zero);
    c->position = feetPos;
    c->velocity = (Vector3){ 0.0f, 0.0f, 0.0f };
}

Vector3 BrushCharacterMove(BrushCharacter *c, BrushPhysics *pw, Vector3 inputVelocity, float dt) {
    if (!c || !c->character || !pw || !pw->system) return (Vector3){ 0.0f, 0.0f, 0.0f };

    // 1. Set velocity
    JPH_Vec3 vel = { inputVelocity.x, inputVelocity.y, inputVelocity.z };
    JPH_CharacterVirtual_SetLinearVelocity(c->character, &vel);

    // 2. Setup Extended Update Settings (Step up stairs, Floor sticking, etc.)
    JPH_ExtendedUpdateSettings updateSettings;
    updateSettings.stickToFloorStepDown = (JPH_Vec3){ 0.0f, -c->stickDown, 0.0f };
    updateSettings.walkStairsStepUp = (JPH_Vec3){ 0.0f, c->stepUp, 0.0f };
    updateSettings.walkStairsMinStepForward = 0.02f;
    updateSettings.walkStairsStepForwardTest = 0.15f;
    updateSettings.walkStairsCosAngleForwardContact = cosf(75.0f * DEG2RAD);
    updateSettings.walkStairsStepDownExtra = (JPH_Vec3){ 0.0f, 0.0f, 0.0f };

    // 3. Step Character
    JPH_CharacterVirtual_ExtendedUpdate(
        c->character,
        dt,
        &updateSettings,
        BRUSH_PHYS_LAYER_MOVING,
        pw->system,
        NULL, // No body filter
        NULL  // No shape filter
    );

    // 4. Retrieve Position
    JPH_RVec3 comPos;
    JPH_CharacterVirtual_GetPosition(c->character, &comPos);
    Vector3 oldPos = c->position;
    c->position = (Vector3){ (float)comPos.x, (float)comPos.y - c->comOffset, (float)comPos.z };

    // 5. Retrieve Velocity
    JPH_Vec3 outVel;
    JPH_CharacterVirtual_GetLinearVelocity(c->character, &outVel);
    c->velocity = *(Vector3*)&outVel;

    // 6. Retrieve Ground State
    JPH_GroundState groundState = JPH_CharacterBase_GetGroundState((JPH_CharacterBase*)c->character);
    c->isGrounded = (groundState == JPH_GroundState_OnGround);

    if (getenv("BRUSH_STEP_DBG")) {
        static int f = 0;
        if (++f % 30 == 0)
            TraceLog(LOG_INFO, "STEP pos=(%.1f,%.2f,%.1f) grounded=%d",
                     c->position.x, c->position.y, c->position.z, c->isGrounded);
    }

    if (c->isGrounded) {
        JPH_Vec3 norm;
        JPH_CharacterBase_GetGroundNormal((JPH_CharacterBase*)c->character, &norm);
        c->groundNormal = *(Vector3*)&norm;
    } else {
        c->groundNormal = (Vector3){ 0.0f, 1.0f, 0.0f };
    }

    // Return visual displacement applied
    return (Vector3){
        c->position.x - oldPos.x,
        c->position.y - oldPos.y,
        c->position.z - oldPos.z
    };
}
