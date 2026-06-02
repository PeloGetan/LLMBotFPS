#pragma once
#include "Strategy.h"
#include "RoundSummary.h"
#include <array>
#include <string>

// Learns which strategies / parameter profiles actually work for THIS player and
// nudges future decisions accordingly. Example: if the bot keeps losing while
// holding because it got flanked, the tuner raises its "scan" (look-around) and
// shortens "rotate_after_seconds"; it also remembers the parameter set that won
// with a given strategy and biases back toward it.
class StrategyTuner {
public:
    struct Stat {
        int used = 0, wins = 0, losses = 0, timeouts = 0;
        int flankDeaths = 0, duelDeaths = 0;
        bool hasWinParams = false;
        StrategyParams winParams;
    };

    void record(StrategyType type, const StrategyParams& used,
                const std::string& winner, const RoundSummary& summary);

    // Adjust a freshly chosen decision in-place. Returns human-readable notes.
    std::string tune(StrategyDecision& d) const;

    float flankPressure() const; // 0..1, recent tendency to die from flanks
    std::string report() const;  // compact effectiveness summary

    std::array<Stat, (int)StrategyType::COUNT> stats{};
    int recentFlankDeaths = 0;
    int recentDuelDeaths = 0;
    int totalRounds = 0;

private:
    static bool isHoldType(StrategyType t);
    StrategyType pickAlternative(StrategyType cur) const;
};
