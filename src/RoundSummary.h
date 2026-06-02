#pragma once
#include "Map.h"
#include "PlayerModel.h"
#include <string>
#include <vector>

// Compact, human-readable summary of a round. This (not raw ticks) is what the
// LLM analyst receives.
struct RoundSummary {
    int round_number = 0;
    std::string winner = "none";          // "player" / "bot" / "timeout"
    float duration_seconds = 0.0f;
    std::string player_route = "unknown";
    std::string bot_strategy = "HoldCommonAngle";
    float first_contact_time = -1.0f;
    std::string first_contact_location = "none";
    std::string player_kill_location = "none";
    std::string bot_death_location = "none";
    std::string peek_style = "unknown";    // "wide" / "narrow"
    std::string contact_reaction = "unknown"; // "push" / "retreat" / "hold"
    std::string player_weapon = "Pistol";  // weapon the player used this round
    std::vector<std::string> observed_patterns;
    PlayerModel model_snapshot;

    // How the round went for the bot (used by the parameter tuner).
    bool bot_was_flanked = false;          // bot took damage from outside its view
    std::string bot_death_cause = "none";  // "flanked" / "duel" / "none"
    float bot_worst_hit_angle = 0.0f;      // largest off-view angle the bot was hit from (rad)
};
