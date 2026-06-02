// Headless test of the adaptation pipeline (no rendering / no LLM).
// Feeds synthetic telemetry across rounds and checks that the bot's model and
// confidence behave as the design requires:
//   * repeating a route raises preference + confidence and shifts strategy,
//   * breaking a predicted pattern (a bait) lowers confidence (anti-overfit).
#include "Map.h"
#include "Analyst.h"
#include "Telemetry.h"
#include "PlayerModel.h"
#include "StrategyTuner.h"
#include "Rollout.h"
#include <cstdio>
#include <cmath>
#include <string>

static Vector2 laneX(Route r) {
    switch (r) {
        case Route::LeftFlank: return {150, 0};
        case Route::RightFlank: return {750, 0};
        default: return {450, 0};
    }
}

// Build a plausible round of telemetry for a given player route + contact time.
static Telemetry makeRound(const Map& map, Route route, float contactTime,
                           const std::string& reaction) {
    Telemetry tel;
    tel.begin();
    tel.add(EventType::RoundStart, 0, map.pos("player_spawn"), "player", "player_spawn");

    float x = laneX(route).x;
    float total = contactTime + 2.0f;
    // Player advances down the lane with some zig-zag (so it isn't read as camping).
    Vector2 botPos = map.pos("bot_spawn");
    for (float t = 0; t <= total; t += 0.12f) {
        float prog = t / total;
        float y = 55.0f + prog * 545.0f;
        float zig = std::sin(t * 6.0f) * 40.0f;
        Vector2 pp = {x + zig, y};
        tel.add(EventType::PlayerPositionSample, t, pp, "player", "lane");
        // Bot reacts: pushes or retreats relative to player after contact.
        if (t >= contactTime) {
            float dir = reaction == "push" ? -1.0f : (reaction == "retreat" ? 1.0f : 0.0f);
            botPos.y += dir * 8.0f;
        }
        tel.add(EventType::BotPositionSample, t, botPos, "bot", "bot");
    }
    tel.add(EventType::FirstContact, contactTime, {x, 360}, "player", "mid_box");
    tel.add(EventType::RoundEnd, total, {x, 360}, "player", "");
    return tel;
}

static int failures = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { printf("  [FAIL] %s\n", msg.c_str()); failures++; }
    else printf("  [ok]   %s\n", msg.c_str());
}

int main() {
    Map map;
    Analyst analyst;
    PlayerModel model;

    auto runRound = [&](int n, Route route, float contact, const std::string& reaction,
                        const std::string& winner) {
        Telemetry tel = makeRound(map, route, contact, reaction);
        RoundSummary s = analyst.analyzeRound(map, tel, winner, contact + 2.0f, n,
                                              "HoldCommonAngle", model);
        StrategyDecision d = analyst.ruleBasedDecision(model, s);
        // Record prediction for next-round bait detection (as the game does).
        analyst.lastPredictedRoute = d.params.watch_route;
        analyst.lastPredictedConfidence = d.confidence;
        printf("Round %d: route=%-11s detected=%-11s conf=%.2f prefMid=%.2f "
               "fastRush=%.2f -> %s (watch=%s)\n",
               n, routeName(route), s.player_route.c_str(), model.confidence,
               model.prefersMid, model.fastRushTendency, strategyName(d.type),
               routeKey(d.params.watch_route));
        return d;
    };

    printf("=== Phase A: player repeats fast mid rush ===\n");
    StrategyDecision d1 = runRound(1, Route::Mid, 5.0f, "push", "player");
    StrategyDecision d2 = runRound(2, Route::Mid, 5.0f, "push", "player");
    StrategyDecision d3 = runRound(3, Route::Mid, 5.5f, "push", "bot");
    StrategyDecision d4 = runRound(4, Route::Mid, 5.0f, "push", "bot");
    float confAfter4 = model.confidence;
    float prefMid4 = model.prefersMid;

    printf("\n--- assertions after repeated mid ---\n");
    check(prefMid4 > 0.7f, "prefersMid > 0.7 after repeating mid");
    check(confAfter4 > 0.5f, "confidence > 0.5 after consistent mid");
    check(d4.params.watch_route == Route::Mid, "bot now watches mid");
    check(model.fastRushTendency > 0.5f, "fastRushTendency detected");

    printf("\n=== Phase B: player baits - switches to right flank ===\n");
    StrategyDecision d5 = runRound(5, Route::RightFlank, 7.0f, "hold", "player");
    float confAfter5 = model.confidence;

    printf("\n--- assertions after bait ---\n");
    check(confAfter5 < confAfter4, "confidence dropped after broken pattern (anti-overfit)");

    printf("\n=== Phase C: player commits to right flank ===\n");
    runRound(6, Route::RightFlank, 7.0f, "hold", "player");
    StrategyDecision d7 = runRound(7, Route::RightFlank, 7.0f, "hold", "player");
    check(model.prefersRightFlank > 0.45f, "prefersRightFlank rising after flank commits");

    // ---------------------------------------------------------------------
    printf("\n=== Phase D: parameter tuner self-correction ===\n");
    StrategyTuner tuner;

    // Build a baseline AggressivePush decision and capture its starting params.
    StrategyDecision base;
    base.type = StrategyType::AggressivePush;
    base.params.scan = 0.3f;
    base.params.rotate_after_seconds = 12.0f;

    // Simulate the bot winning once with AggressivePush (records a winning profile).
    {
        RoundSummary win; win.winner = "bot"; win.bot_was_flanked = false;
        tuner.record(StrategyType::AggressivePush, base.params, "bot", win);
    }
    // Then losing several rounds specifically by being flanked.
    for (int i = 0; i < 3; ++i) {
        RoundSummary loss; loss.winner = "player"; loss.bot_was_flanked = true;
        loss.bot_death_cause = "flanked";
        tuner.record(StrategyType::AggressivePush, base.params, "player", loss);
    }
    printf("Tuner report:\n%s", tuner.report().c_str());
    printf("flankPressure=%.2f\n", tuner.flankPressure());

    StrategyDecision tuned = base;
    std::string notes = tuner.tune(tuned);
    printf("Tune notes: %s\n", notes.c_str());
    printf("scan: %.2f -> %.2f   rotate_after: %.1f -> %.1f\n",
           base.params.scan, tuned.params.scan,
           base.params.rotate_after_seconds, tuned.params.rotate_after_seconds);

    check(tuner.flankPressure() > 0.0f, "flank pressure registered after flank deaths");
    check(tuned.params.scan > base.params.scan + 0.2f,
          "tuner raised scan after repeated flank deaths");
    check(tuned.params.rotate_after_seconds < base.params.rotate_after_seconds,
          "tuner shortened rotate_after_seconds after flank deaths");

    // ---------------------------------------------------------------------
    printf("\n=== Phase E: abandon a strategy that keeps losing ===\n");
    StrategyTuner t2;
    StrategyParams hp; // HoldCommonAngle that always loses head-on
    for (int i = 0; i < 3; ++i) {
        RoundSummary loss; loss.winner = "player"; loss.bot_was_flanked = false;
        t2.record(StrategyType::HoldCommonAngle, hp, "player", loss);
    }
    StrategyDecision keepLosing;
    keepLosing.type = StrategyType::HoldCommonAngle;
    std::string n2 = t2.tune(keepLosing);
    printf("Tune notes: %s\n", n2.c_str());
    printf("strategy after tune: %s\n", strategyName(keepLosing.type));
    check(keepLosing.type != StrategyType::HoldCommonAngle,
          "tuner switched away from a strategy that kept losing");

    // ---------------------------------------------------------------------
    printf("\n=== Phase F: bot learns where the player engages from ===\n");
    Analyst an2;
    PlayerModel m2;
    Vector2 peekSpot = {380, 470}; // player always peeks from the same cover
    for (int i = 0; i < 3; ++i) {
        Telemetry tel = makeRound(map, Route::Mid, 5.0f, "push");
        an2.analyzeRound(map, tel, "player", 7.0f, i + 1, "HoldCommonAngle", m2,
                         true, peekSpot);
        printf("Round %d: engageSamples=%d favEngage=(%.0f,%.0f)\n", i + 1,
               m2.engageSamples, m2.favEngageX, m2.favEngageY);
    }
    check(m2.engageSamples == 3, "engage samples recorded");
    check(std::fabs(m2.favEngageX - peekSpot.x) < 40.0f &&
          std::fabs(m2.favEngageY - peekSpot.y) < 40.0f,
          "learned engage position converged to the player's peek spot");

    // ---------------------------------------------------------------------
    printf("\n=== Phase G: self-play rehearsal evaluates strategies ===\n");
    PlayerModel m3 = m2; // knows the player prefers mid + engage spot
    m3.prefersMid = 0.8f; m3.prefersLeftFlank = 0.1f; m3.prefersRightFlank = 0.1f;
    m3.avgFirstContactTime = 6.0f; m3.confidence = 0.7f;
    RoundSummary dummyS; dummyS.player_route = "mid";
    PlayerSimProfile prof = Rollout::profileFrom(map, m3, dummyS);
    auto roll = Rollout::evaluate(map, m3, prof, 12, 18.0f);
    printf("Rehearsal results (sorted):\n");
    for (auto& r : roll)
        printf("  %-20s winRate=%.2f (%d/%d)\n", strategyName(r.decision.type),
               r.winRate, r.wins, r.games);
    check(roll.size() >= (size_t)StrategyType::COUNT, "all strategies were simulated");
    bool sorted = true;
    for (size_t i = 1; i < roll.size(); ++i)
        if (roll[i].winRate > roll[i - 1].winRate) sorted = false;
    check(sorted, "rehearsal results sorted by win rate");
    bool ranged = true;
    for (auto& r : roll) if (r.winRate < 0.0f || r.winRate > 1.0f) ranged = false;
    check(ranged, "win rates within [0,1]");

    printf("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
           failures);
    return failures == 0 ? 0 : 1;
}
