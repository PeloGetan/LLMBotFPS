#pragma once
#include "Map.h"
#include "Strategy.h"
#include "PlayerModel.h"
#include "RoundSummary.h"
#include <vector>
#include <string>

// "Mental rehearsal": between rounds the bot simulates the upcoming duel many
// times against a model of THIS player's habits, trying each candidate strategy,
// and measures how often the bot would win. The results narrow the choice; the
// LLM then makes the final pick / tuning (and the tuner guards on top).
//
// The simulation reuses the real BotController so it evaluates the bot's ACTUAL
// in-round behavior, not an approximation.

struct PlayerSimProfile {
    Route route = Route::Mid;
    float firstContactTime = 6.0f;
    Vector2 engagePos{0, 0};
    bool engageKnown = false;
    float aggression = 0.5f;
    float pushTendency = 0.5f;
};

struct RolloutResult {
    StrategyDecision decision;
    int wins = 0;
    int games = 0;
    float winRate = 0.0f;
};

class Rollout {
public:
    // Build a behavior profile for the player from the learned model + summary.
    static PlayerSimProfile profileFrom(const Map& map, const PlayerModel& model,
                                        const RoundSummary& summary);

    // Evaluate every candidate strategy with `trials` simulated duels each.
    // Returns results sorted by win rate (descending).
    static std::vector<RolloutResult> evaluate(const Map& map, const PlayerModel& model,
                                               const PlayerSimProfile& profile,
                                               int trials, float maxSeconds);

    // Evaluate an explicit candidate list against several weighted player
    // profiles (e.g. the data-driven profile + an LLM-anticipated profile).
    // The combined (weighted) win rate is stored in each result.
    static std::vector<RolloutResult> evaluateCandidates(
        const Map& map, const PlayerModel& model,
        const std::vector<StrategyDecision>& candidates,
        const std::vector<std::pair<PlayerSimProfile, float>>& profiles,
        int trials, float maxSeconds);

    // Candidate decisions (one canonical parameter set per strategy type).
    static std::vector<StrategyDecision> buildCandidates(const Map& map,
                                                         const PlayerModel& model,
                                                         const PlayerSimProfile& profile);

    // Simulate a single duel; returns "bot" / "player" / "timeout".
    static std::string simulateOne(const Map& map, const PlayerModel& model,
                                   const StrategyDecision& decision,
                                   const PlayerSimProfile& profile, float maxSeconds);
};
