#include "StrategyTuner.h"
#include <algorithm>
#include <sstream>
#include <cmath>

bool StrategyTuner::isHoldType(StrategyType t) {
    switch (t) {
        case StrategyType::HoldCommonAngle:
        case StrategyType::HoldOffAngle:
        case StrategyType::DefensiveHold:
        case StrategyType::AntiFlankTrap:
        case StrategyType::RotateAfterNoContact:
        case StrategyType::RetreatAndRepeek:
            return true;
        default:
            return false;
    }
}

void StrategyTuner::record(StrategyType type, const StrategyParams& used,
                           const std::string& winner, const RoundSummary& summary) {
    totalRounds++;
    Stat& s = stats[(int)type];
    s.used++;

    // Decay recent pressures each round so the bot doesn't over-correct forever.
    recentFlankDeaths = std::max(0, recentFlankDeaths - (recentFlankDeaths > 0 ? 0 : 0));

    if (winner == "bot") {
        s.wins++;
        s.hasWinParams = true;
        s.winParams = used;
        recentFlankDeaths = std::max(0, recentFlankDeaths - 1);
        recentDuelDeaths = std::max(0, recentDuelDeaths - 1);
    } else if (winner == "player") {
        s.losses++;
        if (summary.bot_was_flanked) {
            s.flankDeaths++;
            recentFlankDeaths = std::min(4, recentFlankDeaths + 1);
        } else {
            s.duelDeaths++;
            recentDuelDeaths = std::min(4, recentDuelDeaths + 1);
        }
    } else {
        s.timeouts++;
    }
}

float StrategyTuner::flankPressure() const {
    return clampf(recentFlankDeaths / 3.0f, 0.0f, 1.0f);
}

StrategyType StrategyTuner::pickAlternative(StrategyType cur) const {
    // Prefer a strategy that has actually been winning; otherwise pick a
    // contrasting style that isn't itself a proven loser.
    int best = -1;
    int bestScore = -1000;
    for (int i = 0; i < (int)StrategyType::COUNT; ++i) {
        if (i == (int)cur) continue;
        const Stat& st = stats[i];
        int score = st.wins * 3 - st.losses + (st.used == 0 ? 1 : 0);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0 && bestScore > 0) return (StrategyType)best;

    // Nothing has a winning record: flip the style.
    StrategyType contrast = isHoldType(cur) ? StrategyType::AggressivePush
                                            : StrategyType::HoldOffAngle;
    const Stat& cs = stats[(int)contrast];
    if (cs.used >= 2 && cs.wins == 0) contrast = StrategyType::RotateAfterNoContact;
    return contrast;
}

std::string StrategyTuner::tune(StrategyDecision& d) const {
    std::ostringstream notes;

    // 0) If this strategy keeps losing, abandon it and switch styles entirely.
    {
        const Stat& cur = stats[(int)d.type];
        if (cur.used >= 2 && cur.wins == 0 && cur.losses >= 2) {
            StrategyType alt = pickAlternative(d.type);
            if (alt != d.type) {
                notes << strategyName(d.type) << " kept losing (" << cur.losses
                      << "L/0W) -> switching to " << strategyName(alt) << "; ";
                d.reason = std::string("Switched off ") + strategyName(d.type) +
                           " (it kept losing) to try " + strategyName(alt) + ". " + d.reason;
                d.type = alt;
                d.source = d.source + "+tuner_switch";
                // Sensible default hold spot for the new strategy.
                if (alt == StrategyType::HoldOffAngle) d.params.hold_position = "crate_off_angle";
                else if (alt == StrategyType::HoldCommonAngle) d.params.hold_position = "mid_box";
                else if (alt == StrategyType::DefensiveHold) d.params.hold_position = "back_site";
            }
        }
    }

    const Stat& s = stats[(int)d.type];
    float fp = flankPressure();
    bool hold = isHoldType(d.type);

    // 1) Raise scanning with flank pressure (holds need it the most).
    if (fp > 0.01f) {
        float add = fp * (hold ? 0.6f : 0.35f);
        d.params.scan = clampf(d.params.scan + add, 0.0f, 1.0f);
        notes << "flankPressure=" << std::round(fp * 100) / 100.0
              << " -> scan+=" << std::round(add * 100) / 100.0 << "; ";
        if (fp > 0.3f) {
            float old = d.params.rotate_after_seconds;
            d.params.rotate_after_seconds = clampf(old * lerpf(1.0f, 0.5f, fp), 2.0f, 30.0f);
            notes << "rotate_after " << std::round(old * 10) / 10.0 << "s->"
                  << std::round(d.params.rotate_after_seconds * 10) / 10.0 << "s; ";
        }
    }

    // 2) This specific strategy keeps getting flanked: commit to looking around.
    if (s.used >= 2 && s.flankDeaths > s.wins) {
        d.params.scan = std::max(d.params.scan, 0.75f);
        d.params.rotate_after_seconds = std::min(d.params.rotate_after_seconds, 7.0f);
        notes << strategyName(d.type) << " was repeatedly flanked -> force scan>=0.75; ";
    }

    // 3) This strategy keeps losing straight duels: take safer off-angles.
    if (s.used >= 2 && s.duelDeaths > s.wins && s.flankDeaths == 0) {
        d.params.risk = clampf(d.params.risk + 0.15f, 0.0f, 1.0f);
        d.params.scan = clampf(d.params.scan + 0.2f, 0.0f, 1.0f);
        notes << strategyName(d.type) << " lost head-on duels -> risk+0.15, scan+0.2; ";
    }

    // 4) Reinforce a parameter profile that has won with this strategy before.
    if (s.hasWinParams && s.wins > 0) {
        const StrategyParams& w = s.winParams;
        d.params.aggression = lerpf(d.params.aggression, w.aggression, 0.4f);
        d.params.risk = lerpf(d.params.risk, w.risk, 0.3f);
        d.params.expected_contact_time_min =
            lerpf(d.params.expected_contact_time_min, w.expected_contact_time_min, 0.3f);
        d.params.expected_contact_time_max =
            lerpf(d.params.expected_contact_time_max, w.expected_contact_time_max, 0.3f);
        // Keep the freshly raised scan (don't lower it back) but never below the winner's.
        d.params.scan = std::max(d.params.scan, w.scan * 0.8f);
        notes << "reusing winning profile for " << strategyName(d.type)
              << " (aggr~" << std::round(w.aggression * 100) / 100.0 << "); ";
    }

    d.params.clamp();
    if (notes.str().empty()) return "no tuning (not enough history)";
    return notes.str();
}

std::string StrategyTuner::report() const {
    std::ostringstream os;
    os << "rounds=" << totalRounds << " flankPressure="
       << std::round(flankPressure() * 100) / 100.0 << "\n";
    for (int i = 0; i < (int)StrategyType::COUNT; ++i) {
        const Stat& s = stats[i];
        if (s.used == 0) continue;
        os << "  " << strategyName((StrategyType)i) << ": used=" << s.used
           << " W=" << s.wins << " L=" << s.losses
           << " (flank=" << s.flankDeaths << " duel=" << s.duelDeaths << ")\n";
    }
    return os.str();
}
