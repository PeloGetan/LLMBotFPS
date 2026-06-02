#pragma once
#include "raylib.h"
#include "Weapon.h"

struct Entity {
    Vector2 pos{0, 0};
    Vector2 vel{0, 0};
    float radius = 14.0f;
    float facing = 0.0f;   // radians
    float health = 100.0f;
    bool alive = true;

    WeaponType weapon = WeaponType::Pistol;
    int ammo = -1;         // -1 = infinite (pistol)
    float weaponCd = 0.0f; // time until the next shot is allowed

    void reset(Vector2 spawn) {
        pos = spawn;
        vel = {0, 0};
        health = 100.0f;
        alive = true;
        weapon = WeaponType::Pistol;
        ammo = -1;
        weaponCd = 0.0f;
    }

    void equip(WeaponType w) {
        weapon = w;
        ammo = weaponStats(w).startAmmo;
        weaponCd = 0.0f;
    }
    void damage(float d) {
        if (!alive) return;
        health -= d;
        if (health <= 0) { health = 0; alive = false; }
    }
};
