#include "Rollout.h"
#include "BotController.h"
#include "Entity.h"
#include "Telemetry.h"
#include "Combat.h"
#include "Rng.h"
#include <algorithm>
#include <cmath>

namespace {

Route dominantRoute(const PlayerModel& m) {
    if (m.prefersLeftFlank >= m.prefersMid && m.prefersLeftFlank >= m.prefersRightFlank)
        return Route::LeftFlank;
    if (m.prefersRightFlank >= m.prefersMid && m.prefersRightFlank >= m.prefersLeftFlank)
        return Route::RightFlank;
    return Route::Mid;
}

std::string laneHoldName(Route r) {
    if (r == Route::LeftFlank) return "left_flank";
    if (r == Route::RightFlank) return "right_flank";
    return "mid_box";
}

Vector2 laneCenter(const Map& map, Route r) {
    return map.pos(laneHoldName(r));
}

// Lightweight pathfinding move (mirrors BotController::moveTowards) for the
// simulated player.
struct PlayerNav {
    std::vector<int> path;
    int idx = 0;
    Vector2 goalCache{-9999, -9999};
};

bool moveToward(const Map& map, Entity& e, Vector2 goal, float speed, float dt,
                PlayerNav& nav) {
    Vector2 immediate = goal;
    if (!map.hasLineOfSight(e.pos, goal)) {
        if (vDistSq(goal, nav.goalCache) > 40.0f * 40.0f || nav.path.empty()) {
            nav.path = map.findPath(map.nearestNode(e.pos), map.nearestNode(goal));
            nav.idx = 0;
            nav.goalCache = goal;
        }
        if (!nav.path.empty()) {
            while (nav.idx < (int)nav.path.size() &&
                   vDist(e.pos, map.nodes[nav.path[nav.idx]].pos) < 26.0f)
                ++nav.idx;
            if (nav.idx < (int)nav.path.size())
                immediate = map.nodes[nav.path[nav.idx]].pos;
        }
    } else {
        nav.path.clear();
        nav.goalCache = {-9999, -9999};
    }

    Vector2 d = immediate - e.pos;
    float len = vLen(d);
    if (len < 1.0f) return vDist(e.pos, goal) < 22.0f;
    d = d / len;
    Vector2 step = d * (speed * dt);
    Vector2 next = e.pos + step;
    if (!map.collides(next, e.radius)) e.pos = next;
    else {
        Vector2 nx = {e.pos.x + step.x, e.pos.y};
        Vector2 ny = {e.pos.x, e.pos.y + step.y};
        if (!map.collides(nx, e.radius)) e.pos = nx;
        else if (!map.collides(ny, e.radius)) e.pos = ny;
    }
    return vDist(e.pos, goal) < 22.0f;
}

} // namespace

PlayerSimProfile Rollout::profileFrom(const Map& map, const PlayerModel& model,
                                      const RoundSummary& summary) {
    PlayerSimProfile p;
    p.route = dominantRoute(model);
    p.firstContactTime = clampf(model.avgFirstContactTime, 2.0f, 18.0f);
    p.aggression = model.aggression;
    p.pushTendency = model.pushAfterContactTendency;
    p.engageKnown = model.engageSamples >= 1;
    if (p.engageKnown) p.engagePos = {model.favEngageX, model.favEngageY};
    else p.engagePos = laneCenter(map, p.route);
    return p;
}

std::vector<StrategyDecision> Rollout::buildCandidates(const Map& map,
                                                       const PlayerModel& model,
                                                       const PlayerSimProfile& profile) {
    Route r = profile.route;
    std::vector<StrategyDecision> out;
    for (int i = 0; i < (int)StrategyType::COUNT; ++i) {
        StrategyDecision d;
        d.type = (StrategyType)i;
        d.source = "rollout";
        d.confidence = model.confidence;
        d.params.watch_route = r;
        d.params.rotate_after_seconds = clampf(model.avgFirstContactTime + 4.0f, 4.0f, 16.0f);
        d.params.expected_contact_time_min = std::max(2.0f, model.avgFirstContactTime - 2.0f);
        d.params.expected_contact_time_max = model.avgFirstContactTime + 3.0f;

        switch (d.type) {
            case StrategyType::HoldOffAngle:
                d.params.hold_position = (r == Route::RightFlank) ? "long_angle"
                                       : (r == Route::LeftFlank) ? "left_flank" : "crate_off_angle";
                d.params.aggression = 0.4f; d.params.scan = 0.5f; break;
            case StrategyType::DefensiveHold:
                d.params.hold_position = "back_site"; d.params.aggression = 0.3f;
                d.params.scan = 0.55f; break;
            case StrategyType::AntiFlankTrap:
                d.params.hold_position = laneHoldName(r); d.params.aggression = 0.5f;
                d.params.scan = 0.55f; break;
            case StrategyType::RotateAfterNoContact:
                d.params.hold_position = laneHoldName(r); d.params.aggression = 0.5f;
                d.params.scan = 0.55f; break;
            case StrategyType::FastRush:
            case StrategyType::AggressivePush:
                d.params.hold_position = laneHoldName(r); d.params.aggression = 0.9f;
                d.params.scan = 0.35f; break;
            case StrategyType::MirrorPlayerRoute:
                d.params.hold_position = laneHoldName(r); d.params.aggression = 0.6f;
                d.params.scan = 0.4f; break;
            case StrategyType::SlowClear:
            case StrategyType::DelayedPush:
                d.params.hold_position = laneHoldName(r); d.params.aggression = 0.7f;
                d.params.scan = 0.45f; break;
            default:
                d.params.hold_position = "mid_box"; d.params.aggression = 0.5f;
                d.params.scan = 0.45f; break;
        }
        d.params.clamp();
        out.push_back(d);
    }
    return out;
}

std::string Rollout::simulateOne(const Map& map, const PlayerModel& model,
                                 const StrategyDecision& decision,
                                 const PlayerSimProfile& profile, float maxSeconds) {
    const float dt = 1.0f / 60.0f;

    Entity bot, player;
    bot.reset(map.pos("bot_spawn"));
    player.reset(map.pos("player_spawn"));

    BotController botCtrl;
    botCtrl.beginRound(map, decision, model, map.pos("bot_spawn"));
    Vector2 wp = botCtrl.watchAimPoint();
    if (vDistSq(wp, bot.pos) > 1.0f) bot.facing = vAngle(wp - bot.pos);

    Telemetry dummy;
    dummy.begin();

    // Both sides use the default pistol in the rehearsal (the bot always does;
    // the player's picked-up weapon is treated as a real-game wildcard).
    const float pReaction = 0.16f, pError = 0.05f, pSpeed = 215.0f;
    const float pRange = 780.0f;
    float pSpot = 0.0f;
    PlayerNav nav;
    std::vector<Projectile> proj;

    Vector2 engageGoal = profile.engageKnown ? profile.engagePos
                                             : laneCenter(map, profile.route);
    bool reached = false;
    float pLastFire = 99.0f;

    for (float t = 0.0f; t < maxSeconds; t += dt) {
        bool canSee = player.alive && bot.alive &&
                      map.hasLineOfSight(player.pos, bot.pos) &&
                      vDist(player.pos, bot.pos) < pRange;
        pLastFire += dt;
        player.weaponCd -= dt;

        // ---- Simulated player ----
        if (!reached && vDist(player.pos, engageGoal) > 24.0f &&
            !(canSee && t > profile.firstContactTime * 0.5f)) {
            moveToward(map, player, engageGoal, pSpeed, dt, nav);
        } else {
            reached = true;
            if (canSee) {
                float trueAng = vAngle(bot.pos - player.pos);
                player.facing = trueAng;
                pSpot += dt;
                if (pSpot >= pReaction && player.weaponCd <= 0.0f) {
                    spawnShot(proj, player.pos, trueAng + rng().range(-pError, pError),
                              WeaponType::Pistol, 0, 0.0f, rng());
                    player.weaponCd = weaponStats(WeaponType::Pistol).fireInterval;
                    pLastFire = 0.0f;
                }
                if (profile.pushTendency > 0.5f && vDist(player.pos, bot.pos) > 90.0f)
                    moveToward(map, player, bot.pos, pSpeed * 0.8f, dt, nav);
            } else {
                pSpot = 0.0f;
                moveToward(map, player, engageGoal, pSpeed, dt, nav);
            }
        }

        // ---- Real bot AI ----
        botCtrl.update(dt, t, map, bot, player, dummy, pLastFire < 0.25f);
        bot.weaponCd -= dt;
        if (botCtrl.wantsShoot && bot.alive && bot.weaponCd <= 0.0f) {
            float err = botCtrl.aim.preAimed ? botCtrl.aim.aimErrorRadSteady
                                             : botCtrl.aim.aimErrorRad;
            spawnShot(proj, botCtrl.shootFrom, botCtrl.shootAngle, bot.weapon, 1, err, rng());
            bot.weaponCd = weaponStats(bot.weapon).fireInterval;
        }

        // ---- Projectiles ----
        auto hits = stepProjectiles(proj, map, player, bot, dt);
        for (const auto& h : hits)
            if (h.victimIsBot) botCtrl.onDamaged(h.origin);

        if (!player.alive) return "bot";
        if (!bot.alive) return "player";
    }
    return "timeout";
}

std::vector<RolloutResult> Rollout::evaluate(const Map& map, const PlayerModel& model,
                                             const PlayerSimProfile& profile,
                                             int trials, float maxSeconds) {
    std::vector<StrategyDecision> candidates = buildCandidates(map, model, profile);
    std::vector<RolloutResult> results;
    for (auto& c : candidates) {
        RolloutResult r;
        r.decision = c;
        for (int i = 0; i < trials; ++i) {
            std::string w = simulateOne(map, model, c, profile, maxSeconds);
            if (w == "bot") r.wins++;
            r.games++;
        }
        r.winRate = r.games ? (float)r.wins / r.games : 0.0f;
        results.push_back(r);
    }
    std::sort(results.begin(), results.end(),
              [](const RolloutResult& a, const RolloutResult& b) {
                  return a.winRate > b.winRate;
              });
    return results;
}

std::vector<RolloutResult> Rollout::evaluateCandidates(
    const Map& map, const PlayerModel& model,
    const std::vector<StrategyDecision>& candidates,
    const std::vector<std::pair<PlayerSimProfile, float>>& profiles,
    int trials, float maxSeconds) {
    float wsum = 0.0f;
    for (auto& pr : profiles) wsum += pr.second;
    if (wsum < 1e-4f) wsum = 1.0f;

    std::vector<RolloutResult> results;
    for (auto& c : candidates) {
        RolloutResult r;
        r.decision = c;
        float weighted = 0.0f;
        for (auto& pr : profiles) {
            int wins = 0;
            for (int i = 0; i < trials; ++i) {
                if (simulateOne(map, model, c, pr.first, maxSeconds) == "bot") wins++;
                r.games++;
            }
            r.wins += wins;
            weighted += pr.second * ((float)wins / std::max(1, trials));
        }
        r.winRate = weighted / wsum; // weighted combined win-rate
        results.push_back(r);
    }
    std::sort(results.begin(), results.end(),
              [](const RolloutResult& a, const RolloutResult& b) {
                  return a.winRate > b.winRate;
              });
    return results;
}
