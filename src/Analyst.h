#pragma once
#include "Telemetry.h"
#include "RoundSummary.h"
#include "PlayerModel.h"
#include "Strategy.h"
#include "Map.h"
#include <string>
#include <vector>

// Post-round analysis. Turns telemetry into a compact summary, updates the
// persistent player model GRADUALLY, detects patterns, and produces a
// code-based counter-strategy (used as fallback and when the LLM is disabled).
// Also builds/parses the LLM request & response.
class Analyst {
public:
    // Build the compact summary and refine the player model.
    RoundSummary analyzeRound(const Map& map, const Telemetry& tel,
                              const std::string& winner, float duration,
                              int roundNumber, const std::string& botStrategy,
                              PlayerModel& model,
                              bool engageKnown = false, Vector2 engagePos = {0, 0});

    // Deterministic counter-strategy from the current model (no LLM).
    StrategyDecision ruleBasedDecision(const PlayerModel& model,
                                       const RoundSummary& last);

    // ---- LLM glue ----
    std::string buildSystemPrompt() const;
    std::string buildUserPrompt(const RoundSummary& s, const PlayerModel& model) const;
    // JSON schema used to grammar-constrain the LLM output.
    std::string buildResponseSchema() const;
    // Parse + validate a raw LLM JSON content string. Returns false if invalid.
    bool parseLLMResponse(const std::string& content, const Map& map,
                          StrategyDecision& out, std::string& error) const;

    // ---- Hybrid v2: LLM as strategist (predicts next move + proposes candidates) ----
    struct PredictedNext {
        Route route = Route::Mid;
        bool willBait = false;
        float expectedContact = 6.0f;
    };
    struct LLMPlan {
        std::string player_pattern = "unknown";
        float confidence = 0.5f;
        std::string reason;
        PredictedNext predicted;
        std::vector<StrategyDecision> candidates; // hypotheses to be simulated
    };
    bool parseLLMPlan(const std::string& content, const Map& map, LLMPlan& out,
                      std::string& error) const;

    static std::string summaryToJson(const RoundSummary& s);
    static std::string modelToJson(const PlayerModel& m);

    // History for repeatability / confidence.
    std::vector<Route> routeHistory;
    Route lastPredictedRoute = Route::Unknown;
    float lastPredictedConfidence = 0.0f;

private:
    Route detectRoute(const Map& map, const Telemetry& tel) const;
    float firstContactTime(const Telemetry& tel) const;
    bool detectWidePeek(const Telemetry& tel) const;
    std::string detectReaction(const Map& map, const Telemetry& tel) const;
    Route dominantPreferredRoute(const PlayerModel& m) const;
};
