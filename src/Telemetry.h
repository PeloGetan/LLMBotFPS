#pragma once
#include "Math.h"
#include "Map.h"
#include <string>
#include <vector>

enum class EventType {
    RoundStart, PlayerPositionSample, BotPositionSample, FirstContact,
    FirstShot, DamageEvent, KillEvent, RouteDetected, CoverUsed,
    LineOfSightGained, LineOfSightLost, SoundHeard, RoundEnd
};

struct TelemetryEvent {
    EventType type;
    float time = 0.0f;
    Vector2 pos{0, 0};
    std::string actor;     // "player" / "bot"
    std::string location;  // nearest named location
    float value = 0.0f;    // damage amount, etc.
};

// Records meaningful gameplay events for a single round.
class Telemetry {
public:
    void begin() { events.clear(); }
    void add(EventType t, float time, Vector2 p, const std::string& actor,
             const std::string& loc = "", float value = 0.0f) {
        events.push_back({t, time, p, actor, loc, value});
    }
    const std::vector<TelemetryEvent>& all() const { return events; }

private:
    std::vector<TelemetryEvent> events;
};
