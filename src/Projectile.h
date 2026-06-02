#pragma once
#include "raylib.h"

struct Projectile {
    Vector2 pos{0, 0};
    Vector2 origin{0, 0}; // where it was fired from (used for flank detection)
    Vector2 vel{0, 0};
    float damage = 0.0f;
    float radius = 4.0f;
    float life = 1.0f;
    int owner = 0; // 0 = player, 1 = bot
    bool alive = true;
};
