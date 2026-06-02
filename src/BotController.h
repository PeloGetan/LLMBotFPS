#pragma once
#include "Entity.h"
#include "Map.h"
#include "Strategy.h"
#include "AimModel.h"
#include "PlayerModel.h"
#include "Telemetry.h"
#include <vector>
#include <string>

// Deterministic in-round AI. The bot NEVER queries the LLM during a round and
// NEVER reads the player's real position unless it has line of sight inside its
// field of view (or a nearby sound). All "intelligence" gains come from
// positioning/timing chosen between rounds via StrategyDecision.
class BotController {
public:
    AimModel aim;
    StrategyDecision decision;

    // Knowledge limited to what the bot legitimately perceives.
    bool seesPlayer = false;
    bool everSeen = false;
    Vector2 lastSeenPos{0, 0};
    float timeSinceSeen = 999.0f;

    // Combat output (set true on the frame a shot is fired).
    bool wantsShoot = false;
    Vector2 shootFrom{0, 0};
    float shootAngle = 0.0f;

    void beginRound(const Map& map, const StrategyDecision& d, const PlayerModel& pm,
                    Vector2 spawn);

    void update(float dt, float roundTime, const Map& map, Entity& self,
                const Entity& player, Telemetry& tel, bool playerFiredRecently);

    // Called when the bot takes damage: gives it a fair, in-round reason to
    // react to a threat direction (e.g. a flank) without any wallhack.
    void onDamaged(Vector2 fromPos);

    const std::string& currentLocation() const { return locationName; }
    const char* phaseLabel() const;
    Vector2 watchAimPoint() const { return watchPoint; }

private:
    // A behaviour script: a sequence of waypoints, some of which are "holds".
    struct Step {
        Vector2 goal;
        float speedScale = 1.0f;
        bool hold = false;
        float holdTime = 6.0f; // seconds to hold before advancing
        bool loop = false;     // if true, looping back here when script ends
    };

    Vector2 spawnPos{0, 0};
    Vector2 watchPoint{0, 0};

    std::vector<Step> script;
    int stepIdx = 0;
    bool holding = false;
    float holdTimer = 0.0f;
    bool chasing = false;

    std::vector<int> path;
    int pathIdx = 0;
    Vector2 pathGoalPos{-9999, -9999};

    // In-round adaptation state.
    float alertTimer = 0.0f;        // > 0 while reacting to a recent threat
    Vector2 alertPos{0, 0};         // where the threat came from
    bool inRotateWatch = false;     // patrolling lanes after no contact
    Route rotateLane = Route::Mid;
    bool rotateMoving = false;
    float rotateTimer = 0.0f;

    // Combat movement / leading / prefire.
    Vector2 prevPlayerPos{0, 0};
    Vector2 playerVel{0, 0};
    bool havePrevPlayer = false;
    int strafeDir = 1;
    float strafeTimer = 0.0f;
    float prefireTimer = 0.0f;

    float roundClock = 0.0f;
    float scanPhase = 0.0f;
    bool loggedContact = false;
    bool loggedCover = false;
    std::string locationName = "bot_spawn";
    float moveSpeed = 200.0f;
    const char* phaseName = "setup";

    void buildScript(const Map& map, const PlayerModel& pm);
    bool moveTowards(float dt, const Map& map, Entity& self, Vector2 goal, float speedScale);
    void faceTowards(Entity& self, Vector2 target, float dt);
    void updateAimAndShoot(float dt, const Map& map, Entity& self, const Entity& player);
    Vector2 routeCenter(const Map& map, Route r) const;
    Vector2 routeEntry(const Map& map, Route r) const;
    Route nextLane(Route r) const;
    Vector2 nearestCover(const Map& map, Vector2 p) const;
    bool inFov(const Entity& self, Vector2 target) const;
    std::string nearestName(const Map& map, Vector2 p) const;
};
