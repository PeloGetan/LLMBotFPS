#pragma once
#include "Projectile.h"
#include "Weapon.h"
#include "Entity.h"
#include "Map.h"
#include "Rng.h"
#include "Math.h"
#include <vector>
#include <algorithm>

// Shared projectile combat used by both the live game and the rollout simulator,
// so the simulation reflects the real (projectile-based) duel.

struct Hit {
    int owner = 0;          // who fired (0 player, 1 bot)
    bool victimIsBot = false;
    Vector2 origin{0, 0};   // where the shot came from
    float damage = 0.0f;
};

// Spawn a shot's projectiles. aimError is the shooter's aiming inaccuracy
// (0 for the human player; the bot's skill error otherwise).
inline void spawnShot(std::vector<Projectile>& out, Vector2 origin, float baseAngle,
                      WeaponType w, int owner, float aimError, Rng& rg) {
    WeaponStats s = weaponStats(w);
    for (int i = 0; i < s.pellets; ++i) {
        float sp = (s.pellets > 1 ? s.spread : s.spread * 0.5f) + aimError;
        float ang = baseAngle + rg.range(-sp, sp);
        Projectile p;
        p.pos = origin;
        p.origin = origin;
        p.vel = vFromAngle(ang) * s.projectileSpeed;
        p.damage = s.damage;
        p.radius = s.projRadius;
        p.life = s.life;
        p.owner = owner;
        p.alive = true;
        out.push_back(p);
    }
}

// Advance all projectiles; resolve wall and entity collisions; apply damage.
// Returns the hits that landed this frame (for damage events / flank reactions).
inline std::vector<Hit> stepProjectiles(std::vector<Projectile>& ps, const Map& map,
                                        Entity& player, Entity& bot, float dt) {
    std::vector<Hit> hits;
    for (auto& p : ps) {
        if (!p.alive) continue;
        p.life -= dt;
        if (p.life <= 0.0f) { p.alive = false; continue; }
        p.pos += p.vel * dt;
        if (map.collides(p.pos, p.radius)) { p.alive = false; continue; }

        Entity& target = (p.owner == 0) ? bot : player;
        if (target.alive &&
            vDist(p.pos, target.pos) <= target.radius + p.radius) {
            target.damage(p.damage);
            p.alive = false;
            Hit h;
            h.owner = p.owner;
            h.victimIsBot = (p.owner == 0);
            h.origin = p.origin;
            h.damage = p.damage;
            hits.push_back(h);
        }
    }
    ps.erase(std::remove_if(ps.begin(), ps.end(),
             [](const Projectile& p) { return !p.alive; }), ps.end());
    return hits;
}
