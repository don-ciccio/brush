/*******************************************************************************************
 *   b_character.h - Kinematic character controller (Jolt CharacterVirtual)
 *
 *   LICENSE: zlib/libpng
 ********************************************************************************************/

#ifndef B_CHARACTER_H
#define B_CHARACTER_H

#include "b_physics.h"
#include <raylib.h>
#include <joltc.h>

typedef struct BrushCharacter {
    JPH_CharacterVirtual* character;
    JPH_Shape*            capsuleShape;
    float                 comOffset;        // offset from feet position to COM (center of capsule)
    
    Vector3 position;       // feet position (bottom of capsule)
    Vector3 velocity;       // current linear velocity
    bool isGrounded;        // true if supported by ground
    Vector3 groundNormal;   // normal of the ground surface we are standing on

    // Tunables (defaults set in Init; adjust per game before/between moves)
    float stepUp;       // max stair riser the controller climbs (m)
    float stickDown;    // floor-snap probe distance when walking down (m)
} BrushCharacter;

// Initialize the character virtual controller at startPos (feet position)
bool BrushCharacterInit(BrushCharacter *c, BrushPhysics *pw, Vector3 startPos, float radius, float totalHeight);

// Clean up character allocations
void BrushCharacterCleanup(BrushCharacter *c, BrushPhysics *pw);

// Move the character virtual by inputVelocity (XZ movement + Y jump/gravity).
// Handles wall-sliding, slope limits, and stair-climbing.
// Returns the actual visual movement applied.
Vector3 BrushCharacterMove(BrushCharacter *c, BrushPhysics *pw, Vector3 inputVelocity, float dt);

// Teleport the capsule to a feet position and zero its velocity (respawns,
// mantles, scripted moves). No collision sweep — the caller guarantees the
// target is clear.
void BrushCharacterWarp(BrushCharacter *c, Vector3 feetPos);

#endif // B_CHARACTER_H
