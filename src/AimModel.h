#pragma once
#include "Math.h"

// FAIR aim model with FIXED difficulty limits. The LLM is NOT allowed to make
// the bot more accurate or lower its reaction time between rounds; only
// positioning / timing / strategy may change. These constants are the ceiling.
struct AimModel {
    // Reaction delay before the bot starts tracking a freshly spotted target.
    float reactionTime = 0.22f;          // seconds (when swinging onto a fresh target)
    // When the bot was already PRE-AIMING the spot where the target appears
    // (i.e. it learned where the player peeks), it reacts much faster and aims
    // steadier. This is the bot's reward for learning you - it is the ONLY way it
    // wins fast duels, and you can defeat it by attacking from a new spot.
    float reactionPreaimFactor = 0.22f;  // pre-aimed reaction = 0.22 * 0.22 ~= 0.05s
    // Aiming error cone around the true angle (swinging vs steady/pre-aimed).
    float aimErrorRad = 0.085f;
    float aimErrorRadSteady = 0.035f;
    // How fast aim converges onto the target after reaction (rad/sec).
    float turnSpeed = 11.0f;
    // Time between shots and damage (tuned so the bot can actually win duels,
    // while still being fair: it has reaction delay + spread and can miss).
    float fireInterval = 0.16f;
    float damagePerShot = 38.0f;
    // Only fire when the aim is within this cone of the target.
    float fireConeRad = 0.13f;
    // Effective max engagement distance (beyond this, the bot holds fire).
    float maxRange = 760.0f;
    // Field-of-view HALF angle: the bot only perceives targets within this cone
    // of its facing. Narrow enough that the bot can be flanked, which makes the
    // "scan / look around" behavior meaningful.
    float fovHalf = 1.05f;               // ~60 deg half => ~120 deg total FOV

    // Runtime aim state.
    float currentAim = 0.0f;   // current aim angle
    float spotTimer = 0.0f;    // time since target first seen this exposure
    float fireCooldown = 0.0f;
    bool preAimed = false;     // was the angle already aimed where the target appeared?
};
