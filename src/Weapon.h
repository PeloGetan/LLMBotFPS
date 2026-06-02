#pragma once

enum class WeaponType { Pistol, Rifle, Shotgun };

struct WeaponStats {
    const char* name;
    float damage;          // per projectile
    float fireInterval;    // seconds between shots
    float projectileSpeed; // px/sec
    int pellets;           // projectiles per shot (shotgun > 1)
    float spread;          // base angular spread (radians)
    int startAmmo;         // -1 = infinite (pistol)
    float projRadius;
    float life;            // seconds before the projectile expires (limits range)
};

inline WeaponStats weaponStats(WeaponType t) {
    switch (t) {
        case WeaponType::Rifle:
            // Fast automatic, low per-shot damage. Sustained pressure.
            return {"Rifle", 13.0f, 0.10f, 1150.0f, 1, 0.045f, 90, 4.0f, 1.2f};
        case WeaponType::Shotgun:
            // Many pellets, big spread, slow. Lethal up close, weak at range.
            return {"Shotgun", 9.0f, 0.80f, 760.0f, 9, 0.20f, 18, 4.0f, 0.55f};
        case WeaponType::Pistol:
        default:
            // Default sidearm: moderate damage, moderate rate, infinite ammo.
            return {"Pistol", 20.0f, 0.34f, 820.0f, 1, 0.02f, -1, 4.0f, 1.2f};
    }
}
