#pragma once
#include <string>
#include <array>
#include "Map.h"

// Fixed library of allowed bot strategies. The LLM may CHOOSE from this list
// and tune parameters, but may never invent new actions.
enum class StrategyType {
    HoldCommonAngle,
    HoldOffAngle,
    FastRush,
    SlowClear,
    DelayedPush,
    FakeSoundBait,
    AntiFlankTrap,
    RotateAfterNoContact,
    DefensiveHold,
    AggressivePush,
    RetreatAndRepeek,
    MirrorPlayerRoute,
    COUNT
};

inline const char* strategyName(StrategyType s) {
    switch (s) {
        case StrategyType::HoldCommonAngle:      return "HoldCommonAngle";
        case StrategyType::HoldOffAngle:         return "HoldOffAngle";
        case StrategyType::FastRush:             return "FastRush";
        case StrategyType::SlowClear:            return "SlowClear";
        case StrategyType::DelayedPush:          return "DelayedPush";
        case StrategyType::FakeSoundBait:        return "FakeSoundBait";
        case StrategyType::AntiFlankTrap:        return "AntiFlankTrap";
        case StrategyType::RotateAfterNoContact: return "RotateAfterNoContact";
        case StrategyType::DefensiveHold:        return "DefensiveHold";
        case StrategyType::AggressivePush:       return "AggressivePush";
        case StrategyType::RetreatAndRepeek:     return "RetreatAndRepeek";
        case StrategyType::MirrorPlayerRoute:    return "MirrorPlayerRoute";
        default:                                 return "Unknown";
    }
}

inline bool strategyFromName(const std::string& n, StrategyType& out) {
    for (int i = 0; i < (int)StrategyType::COUNT; ++i) {
        if (n == strategyName((StrategyType)i)) { out = (StrategyType)i; return true; }
    }
    return false;
}

// Tunable parameters chosen between rounds. All values are clamped on validation.
struct StrategyParams {
    std::string hold_position = "mid_box";
    float expected_contact_time_min = 5.0f;
    float expected_contact_time_max = 10.0f;
    float rotate_after_seconds = 12.0f;
    float aggression = 0.5f;   // 0 = passive hold, 1 = push hard
    float risk = 0.5f;         // willingness to take exposed angles
    float scan = 0.3f;         // how much the bot sweeps its view to check angles/flanks
    Route watch_route = Route::Mid; // lane the bot expects the player on

    void clamp() {
        expected_contact_time_min = clampf(expected_contact_time_min, 0.0f, 30.0f);
        expected_contact_time_max = clampf(expected_contact_time_max, expected_contact_time_min, 30.0f);
        rotate_after_seconds = clampf(rotate_after_seconds, 2.0f, 30.0f);
        aggression = clampf(aggression, 0.0f, 1.0f);
        risk = clampf(risk, 0.0f, 1.0f);
        scan = clampf(scan, 0.0f, 1.0f);
    }
};

inline const char* routeKey(Route r) {
    switch (r) {
        case Route::Mid: return "mid";
        case Route::LeftFlank: return "left_flank";
        case Route::RightFlank: return "right_flank";
        default: return "mid";
    }
}

inline Route routeFromKey(const std::string& k) {
    if (k == "left_flank" || k == "left") return Route::LeftFlank;
    if (k == "right_flank" || k == "right") return Route::RightFlank;
    return Route::Mid;
}

struct StrategyDecision {
    StrategyType type = StrategyType::HoldCommonAngle;
    StrategyParams params;
    std::string player_pattern = "unknown";
    float confidence = 0.0f;
    std::string reason = "default strategy";
    std::string source = "default"; // "llm", "rules", "default", "fallback"
};
