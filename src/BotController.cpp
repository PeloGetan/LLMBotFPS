#include "BotController.h"
#include "Rng.h"
#include <algorithm>
#include <limits>

Vector2 BotController::routeCenter(const Map& map, Route r) const {
    switch (r) {
        case Route::LeftFlank:  return map.pos("left_flank");
        case Route::RightFlank: return map.pos("right_flank");
        default:                return map.pos("mid_box");
    }
}

Vector2 BotController::routeEntry(const Map& map, Route r) const {
    switch (r) {
        case Route::LeftFlank:  return map.pos("top_left");
        case Route::RightFlank: return map.pos("top_right");
        default:                return map.pos("mid_entry");
    }
}

Route BotController::nextLane(Route r) const {
    switch (r) {
        case Route::Mid: return Route::LeftFlank;
        case Route::LeftFlank: return Route::RightFlank;
        default: return Route::Mid;
    }
}

Vector2 BotController::nearestCover(const Map& map, Vector2 p) const {
    Vector2 best = p;
    float bd = 1e9f;
    for (auto& c : map.coverPoints) {
        float d = vDistSq(p, c);
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

void BotController::onDamaged(Vector2 fromPos) {
    // Getting shot is a legitimate cue: remember the direction and react.
    alertPos = fromPos;
    alertTimer = 2.6f;
    inRotateWatch = false; // stop idle patrol; deal with the threat
}

std::string BotController::nearestName(const Map& map, Vector2 p) const {
    std::string best = "unknown";
    float bestD = std::numeric_limits<float>::max();
    for (auto& kv : map.named) {
        float d = vDistSq(p, kv.second);
        if (d < bestD) { bestD = d; best = kv.first; }
    }
    return best;
}

bool BotController::inFov(const Entity& self, Vector2 target) const {
    Vector2 to = target - self.pos;
    if (vLenSq(to) < 1.0f) return true;
    float a = vAngle(to);
    return std::fabs(angleDiff(self.facing, a)) < aim.fovHalf;
}

void BotController::beginRound(const Map& map, const StrategyDecision& d,
                               const PlayerModel& pm, Vector2 spawn) {
    decision = d;
    spawnPos = spawn;
    aim = AimModel{}; // reset runtime state, keep FIXED fairness limits
    seesPlayer = everSeen = false;
    timeSinceSeen = 999.0f;
    lastSeenPos = spawn;
    wantsShoot = false;
    stepIdx = 0;
    holding = false;
    holdTimer = 0.0f;
    chasing = false;
    roundClock = 0.0f;
    scanPhase = 0.0f;
    alertTimer = 0.0f;
    inRotateWatch = false;
    rotateMoving = false;
    rotateTimer = 0.0f;
    rotateLane = d.params.watch_route;
    loggedContact = loggedCover = false;
    path.clear();
    pathIdx = 0;
    pathGoalPos = {-9999, -9999};
    moveSpeed = 175.0f + d.params.aggression * 70.0f;

    // Pre-aim the learned engage spot ONLY if it lies in the lane we now watch;
    // otherwise (e.g. anticipating a flank) watch that lane's entry instead.
    Vector2 fav = {pm.favEngageX, pm.favEngageY};
    if (pm.engageSamples >= 1 && map.routeOf(fav) == d.params.watch_route)
        watchPoint = fav;
    else
        watchPoint = routeEntry(map, d.params.watch_route);
    buildScript(map, pm);
}

void BotController::buildScript(const Map& map, const PlayerModel& pm) {
    script.clear();
    const auto& p = decision.params;
    Route wr = p.watch_route;
    Vector2 hold = map.pos(p.hold_position);
    float rotateT = p.rotate_after_seconds;

    auto holdStep = [&](Vector2 g, float t) {
        script.push_back({g, 0.9f, true, t, false});
    };
    auto moveStep = [&](Vector2 g, float s) {
        script.push_back({g, s, false, 0.0f, false});
    };

    // Optionally fetch a weapon first (a tactical detour the rollout evaluates).
    WeaponType gw;
    Vector2 gpos;
    if (grabWeaponType(p.grab_weapon, gw) && map.weaponSpawnPos(gw, gpos))
        moveStep(gpos, 1.0f);

    switch (decision.type) {
        case StrategyType::HoldCommonAngle:
            holdStep(map.pos("mid_box"), 999.0f);
            break;
        case StrategyType::HoldOffAngle:
            holdStep(hold, 999.0f);
            break;
        case StrategyType::DefensiveHold:
            holdStep(map.pos("back_site"), 999.0f);
            break;
        case StrategyType::RotateAfterNoContact:
            holdStep(routeCenter(map, wr), rotateT);
            holdStep(routeCenter(map, wr == Route::Mid ? Route::LeftFlank : Route::Mid), rotateT);
            holdStep(routeCenter(map, wr == Route::RightFlank ? Route::Mid : Route::RightFlank), rotateT);
            script.back().loop = true;
            break;
        case StrategyType::AntiFlankTrap: {
            // Set up watching the flank the player favours, off to the side.
            Route fr = (pm.prefersLeftFlank >= pm.prefersRightFlank) ? Route::LeftFlank
                                                                     : Route::RightFlank;
            if (p.watch_route != Route::Mid) fr = p.watch_route;
            holdStep(routeCenter(map, fr), 999.0f);
            break;
        }
        case StrategyType::FastRush:
            moveStep(routeCenter(map, wr), 1.0f);
            moveStep(routeEntry(map, wr), 1.0f);
            moveStep(map.pos("player_spawn"), 1.0f);
            break;
        case StrategyType::AggressivePush:
            moveStep(routeCenter(map, wr), 1.0f);
            holdStep(routeCenter(map, wr), 3.0f);
            moveStep(routeEntry(map, wr), 1.0f);
            break;
        case StrategyType::SlowClear:
            moveStep(routeCenter(map, wr), 0.55f);
            holdStep(routeCenter(map, wr), 2.5f);
            moveStep(routeEntry(map, wr), 0.55f);
            break;
        case StrategyType::DelayedPush:
            holdStep(map.pos("bot_spawn"), std::max(2.0f, p.expected_contact_time_min));
            moveStep(routeCenter(map, wr), 1.0f);
            moveStep(routeEntry(map, wr), 1.0f);
            break;
        case StrategyType::FakeSoundBait: {
            // Make noise pushing one lane, then quietly hold an off-angle elsewhere.
            Route bait = wr;
            Route other = (wr == Route::Mid) ? Route::RightFlank : Route::Mid;
            moveStep(routeCenter(map, bait), 1.0f);
            moveStep(map.pos("crate_off_angle"), 1.0f);
            holdStep(routeCenter(map, other), 999.0f);
            break;
        }
        case StrategyType::RetreatAndRepeek:
            holdStep(routeCenter(map, wr), 3.0f);
            holdStep(map.pos("back_site"), 2.5f);
            script.back().loop = true;
            break;
        case StrategyType::MirrorPlayerRoute:
            moveStep(routeCenter(map, wr), 0.85f);
            holdStep(routeCenter(map, wr), 999.0f);
            break;
        default:
            holdStep(map.pos("mid_box"), 999.0f);
            break;
    }
    if (script.empty()) holdStep(map.pos("mid_box"), 999.0f);
}

const char* BotController::phaseLabel() const { return phaseName; }

// Move toward a goal, pathfinding around walls via the nav graph when needed.
bool BotController::moveTowards(float dt, const Map& map, Entity& self,
                                Vector2 goal, float speedScale) {
    float spd = moveSpeed * speedScale;
    Vector2 immediate = goal;

    if (!map.hasLineOfSight(self.pos, goal)) {
        // Recompute a path if the goal changed meaningfully.
        if (vDistSq(goal, pathGoalPos) > 40.0f * 40.0f || path.empty()) {
            int s = map.nearestNode(self.pos);
            int g = map.nearestNode(goal);
            path = map.findPath(s, g);
            pathIdx = 0;
            pathGoalPos = goal;
        }
        if (!path.empty()) {
            while (pathIdx < (int)path.size() &&
                   vDist(self.pos, map.nodes[path[pathIdx]].pos) < 26.0f)
                ++pathIdx;
            if (pathIdx < (int)path.size())
                immediate = map.nodes[path[pathIdx]].pos;
        }
    } else {
        path.clear();
        pathGoalPos = {-9999, -9999};
    }

    Vector2 dir = immediate - self.pos;
    float dlen = vLen(dir);
    if (dlen < 1.0f) return vDist(self.pos, goal) < 22.0f;
    dir = dir / dlen;

    Vector2 step = dir * (spd * dt);
    Vector2 next = self.pos + step;
    if (!map.collides(next, self.radius)) {
        self.pos = next;
    } else {
        // Try sliding along each axis.
        Vector2 nx = {self.pos.x + step.x, self.pos.y};
        Vector2 ny = {self.pos.x, self.pos.y + step.y};
        if (!map.collides(nx, self.radius)) self.pos = nx;
        else if (!map.collides(ny, self.radius)) self.pos = ny;
    }
    return vDist(self.pos, goal) < 22.0f;
}

void BotController::faceTowards(Entity& self, Vector2 target, float dt) {
    Vector2 to = target - self.pos;
    if (vLenSq(to) < 1.0f) return;
    float want = vAngle(to);
    float d = angleDiff(self.facing, want);
    float maxTurn = aim.turnSpeed * dt;
    self.facing += clampf(d, -maxTurn, maxTurn);
}

void BotController::updateAimAndShoot(float dt, const Map& map, Entity& self,
                                      const Entity& player) {
    wantsShoot = false;
    if (!seesPlayer) { aim.spotTimer = 0.0f; return; }

    Vector2 to = player.pos - self.pos;
    float trueAngle = vAngle(to);
    float dist = vLen(to);

    // On the first frame of seeing the player, check whether the bot was already
    // pre-aiming that spot (i.e. it learned where the player peeks). A held,
    // pre-aimed angle reacts faster than swinging onto a fresh target.
    if (aim.spotTimer == 0.0f)
        aim.preAimed = std::fabs(angleDiff(self.facing, trueAngle)) < aim.fireConeRad * 3.0f;

    aim.spotTimer += dt;

    // Converge aim onto target (turn speed limited).
    float d = angleDiff(self.facing, trueAngle);
    float maxTurn = aim.turnSpeed * dt;
    self.facing += clampf(d, -maxTurn, maxTurn);
    aim.currentAim = self.facing;

    float effReaction = aim.preAimed ? aim.reactionTime * aim.reactionPreaimFactor
                                     : aim.reactionTime;
    if (aim.spotTimer < effReaction) return; // fair reaction delay
    if (dist > aim.maxRange) return;
    if (std::fabs(angleDiff(self.facing, trueAngle)) > aim.fireConeRad) return;

    // Aim is on target & reaction satisfied. The weapon's fire rate / damage /
    // projectile are applied by the combat layer (Game / Rollout), which also
    // adds the bot's aim error via spawnShot. We just request a shot.
    wantsShoot = true;
    shootFrom = self.pos;
    shootAngle = trueAngle;
}

void BotController::update(float dt, float roundTime, const Map& map, Entity& self,
                           const Entity& player, Telemetry& tel,
                           bool playerFiredRecently) {
    roundClock = roundTime;
    locationName = nearestName(map, self.pos);

    // ---- Perception (fair: FOV + line of sight only) ----
    bool los = player.alive && self.alive &&
               map.hasLineOfSight(self.pos, player.pos) &&
               vDist(self.pos, player.pos) <= aim.maxRange &&
               inFov(self, player.pos);
    seesPlayer = los;
    if (los) {
        if (!everSeen || !loggedContact) {
            tel.add(EventType::FirstContact, roundTime, player.pos, "bot",
                    nearestName(map, player.pos));
            tel.add(EventType::LineOfSightGained, roundTime, self.pos, "bot", locationName);
            loggedContact = true;
        }
        everSeen = true;
        lastSeenPos = player.pos;
        timeSinceSeen = 0.0f;
    } else {
        timeSinceSeen += dt;
        // Sound cue: nearby shots/footsteps give a coarse last-known position.
        float pd = vDist(self.pos, player.pos);
        if ((playerFiredRecently && pd < 480.0f) || pd < 150.0f) {
            lastSeenPos = player.pos;
            timeSinceSeen = std::min(timeSinceSeen, 0.6f);
            tel.add(EventType::SoundHeard, roundTime, player.pos, "bot",
                    nearestName(map, player.pos));
        }
    }

    if (!loggedCover && vDist(self.pos, map.pos(decision.params.hold_position)) < 40.0f) {
        tel.add(EventType::CoverUsed, roundTime, self.pos, "bot", decision.params.hold_position);
        loggedCover = true;
    }

    // ---- In-round alert handling ----
    if (alertTimer > 0.0f) alertTimer -= dt;
    if (seesPlayer) alertTimer = 0.0f; // threat acquired directly

    // Aggression override: chase the last seen position when confident & seen.
    chasing = (seesPlayer || timeSinceSeen < 1.5f) &&
              decision.params.aggression > 0.6f && everSeen;

    // ---- Choose an in-round mode (this is where the bot adapts on the fly) ----
    enum class Mode { Engage, Chase, Investigate, Rotate, Script };
    Mode mode;
    if (seesPlayer) mode = chasing ? Mode::Chase : Mode::Engage;
    else if (chasing) mode = Mode::Chase;
    else if (alertTimer > 0.0f) mode = Mode::Investigate;
    else if (inRotateWatch) mode = Mode::Rotate;
    else mode = Mode::Script;

    Vector2 faceThreat = watchPoint; // where to look when not seeing the player
    float scanScale = 1.0f;

    switch (mode) {
        case Mode::Engage:
            // See the player but not aggressive: hold ground and shoot.
            phaseName = "engage";
            break;

        case Mode::Chase:
            moveTowards(dt, map, self, lastSeenPos, 1.0f);
            faceThreat = lastSeenPos;
            phaseName = "push";
            break;

        case Mode::Investigate: {
            // Reacting to being shot from somewhere: turn to it and either push
            // to clear the angle (aggressive) or fall back to cover (passive).
            faceThreat = alertPos;
            scanScale = 0.25f; // look right at the threat, don't sweep wildly
            Vector2 dest = decision.params.aggression >= 0.45f
                               ? alertPos
                               : nearestCover(map, self.pos);
            if (vDist(self.pos, dest) > 26.0f) moveTowards(dt, map, self, dest, 1.0f);
            phaseName = "investigate";
            break;
        }

        case Mode::Rotate: {
            // Patrol lanes: relocate and re-check angles instead of dying in place.
            Vector2 center = routeCenter(map, rotateLane);
            watchPoint = routeEntry(map, rotateLane);
            faceThreat = watchPoint;
            if (rotateMoving) {
                bool reached = moveTowards(dt, map, self, center, 0.95f);
                if (reached) { rotateMoving = false; rotateTimer = 0.0f; }
                phaseName = "reposition";
            } else {
                rotateTimer += dt;
                if (rotateTimer >= decision.params.rotate_after_seconds) {
                    rotateLane = nextLane(rotateLane);
                    rotateMoving = true;
                }
                phaseName = "rotate";
            }
            break;
        }

        case Mode::Script: {
            if (stepIdx < (int)script.size()) {
                const Step& s = script[stepIdx];
                bool terminal = (stepIdx == (int)script.size() - 1) && !s.loop;
                if (holding) {
                    faceThreat = watchPoint;
                    holdTimer += dt;
                    float effHold = terminal ? std::min(s.holdTime,
                                       decision.params.rotate_after_seconds) : s.holdTime;
                    if (holdTimer >= effHold && !seesPlayer) {
                        holding = false;
                        holdTimer = 0.0f;
                        if (terminal) {
                            // No contact here: start patrolling other lanes.
                            inRotateWatch = true;
                            rotateLane = nextLane(decision.params.watch_route);
                            rotateMoving = true;
                            rotateTimer = 0.0f;
                        } else if (s.loop) {
                            stepIdx = 0;
                        } else {
                            ++stepIdx;
                        }
                    }
                    phaseName = "hold";
                } else {
                    bool reached = moveTowards(dt, map, self, s.goal, s.speedScale);
                    faceThreat = s.goal;
                    phaseName = s.hold ? "advance" : "clear";
                    if (reached) {
                        if (s.hold) { holding = true; holdTimer = 0.0f; }
                        else if (s.loop) stepIdx = 0;
                        else ++stepIdx;
                    }
                }
            } else {
                phaseName = "hold";
            }
            break;
        }
    }

    // ---- Keep spacing: never crowd the player closer than two player-diameters.
    {
        float minSep = player.radius * 4.0f;
        Vector2 away = self.pos - player.pos;
        float d = vLen(away);
        if (d > 0.001f && d < minSep) {
            Vector2 desired = player.pos + (away / d) * minSep;
            if (!map.collides(desired, self.radius)) self.pos = desired;
        }
    }

    // ---- Facing ----
    if (seesPlayer) {
        // aim handles facing
    } else if (timeSinceSeen < 1.2f && mode != Mode::Investigate) {
        scanPhase = 0.0f;
        faceTowards(self, lastSeenPos, dt);
    } else {
        // Sweep ONLY around the plausible threat direction (last seen / where we
        // were shot from / the lane we watch). No blind 360-degree spinning.
        float scan = decision.params.scan;
        Vector2 toBase = faceThreat - self.pos;
        float baseAng = (vLenSq(toBase) > 1.0f) ? vAngle(toBase) : self.facing;
        scanPhase += dt * (1.0f + scan * 2.0f);
        float amp = lerpf(0.1f, 0.9f, scan) * scanScale;
        float target = baseAng + std::sin(scanPhase) * amp;
        float d = angleDiff(self.facing, target);
        float maxTurn = aim.turnSpeed * dt;
        self.facing += clampf(d, -maxTurn, maxTurn);
    }

    // ---- Combat ----
    updateAimAndShoot(dt, map, self, player);
}
