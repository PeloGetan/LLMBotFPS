#pragma once
#include "raylib.h"

struct Entity {
    Vector2 pos{0, 0};
    Vector2 vel{0, 0};
    float radius = 14.0f;
    float facing = 0.0f;   // radians
    float health = 100.0f;
    bool alive = true;

    void reset(Vector2 spawn) {
        pos = spawn;
        vel = {0, 0};
        health = 100.0f;
        alive = true;
    }
    void damage(float d) {
        if (!alive) return;
        health -= d;
        if (health <= 0) { health = 0; alive = false; }
    }
};
