#pragma once
#include "Math.h"

// Persistent model of THIS player, refined across rounds. Values are smoothed
// (exponential moving averages) so the bot learns gradually, never instantly.
struct PlayerModel {
    float aggression = 0.5f;
    float routeRepeatability = 0.0f;

    float prefersMid = 0.34f;
    float prefersLeftFlank = 0.33f;
    float prefersRightFlank = 0.33f;

    float campingTendency = 0.0f;
    float fastRushTendency = 0.0f;
    float slowClearTendency = 0.0f;

    float widePeekTendency = 0.0f;
    float retreatAfterContactTendency = 0.0f;
    float pushAfterContactTendency = 0.0f;

    float avgFirstContactTime = 7.0f;
    float confidence = 0.0f;

    // Where the player tends to engage / kill the bot from (learned across
    // rounds). The bot pre-aims this spot instead of a generic lane entry.
    float favEngageX = 0.0f;
    float favEngageY = 0.0f;
    int engageSamples = 0;

    static void ema(float& v, float sample, float a) { v = lerpf(v, sample, a); }
};
