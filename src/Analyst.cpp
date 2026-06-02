#include "Analyst.h"
#include "json.hpp"
#include "Rng.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Sample extraction helpers
// ---------------------------------------------------------------------------
namespace {
struct Sample { float t; Vector2 p; };

std::vector<Sample> samplesOf(const Telemetry& tel, EventType type) {
    std::vector<Sample> out;
    for (auto& e : tel.all())
        if (e.type == type) out.push_back({e.time, e.pos});
    return out;
}

Vector2 posAtTime(const std::vector<Sample>& s, float t) {
    if (s.empty()) return {0, 0};
    const Sample* best = &s[0];
    float bd = 1e9f;
    for (auto& x : s) {
        float d = std::fabs(x.t - t);
        if (d < bd) { bd = d; best = &x; }
    }
    return best->p;
}

float pathLength(const std::vector<Sample>& s) {
    float len = 0;
    for (size_t i = 1; i < s.size(); ++i) len += vDist(s[i - 1].p, s[i].p);
    return len;
}

float avgSpeed(const std::vector<Sample>& s) {
    if (s.size() < 2) return 0;
    float total = pathLength(s);
    float dt = s.back().t - s.front().t;
    return dt > 0.1f ? total / dt : 0;
}
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------
Route Analyst::detectRoute(const Map& map, const Telemetry& tel) const {
    int counts[4] = {0, 0, 0, 0};
    float fct = firstContactTime(tel);
    for (auto& e : tel.all()) {
        if (e.type != EventType::PlayerPositionSample) continue;
        if (fct > 0 && e.time > fct + 1.0f) continue; // weight pre-contact movement
        counts[(int)map.routeOf(e.pos)]++;
    }
    int best = (int)Route::Mid;
    int bestC = -1;
    for (int i = 1; i < 4; ++i)
        if (counts[i] > bestC) { bestC = counts[i]; best = i; }
    return (Route)best;
}

float Analyst::firstContactTime(const Telemetry& tel) const {
    for (auto& e : tel.all())
        if (e.type == EventType::FirstContact) return e.time;
    for (auto& e : tel.all())
        if (e.type == EventType::FirstShot) return e.time;
    return -1.0f;
}

bool Analyst::detectWidePeek(const Telemetry& tel) const {
    auto ps = samplesOf(tel, EventType::PlayerPositionSample);
    return avgSpeed(ps) > 150.0f; // moving fast while engaging => wide swing
}

std::string Analyst::detectReaction(const Map& map, const Telemetry& tel) const {
    float fct = firstContactTime(tel);
    if (fct < 0) return "no_contact";
    auto ps = samplesOf(tel, EventType::PlayerPositionSample);
    auto bs = samplesOf(tel, EventType::BotPositionSample);
    if (ps.empty() || bs.empty()) return "unknown";
    Vector2 p0 = posAtTime(ps, fct);
    Vector2 b0 = posAtTime(bs, fct);
    Vector2 p1 = posAtTime(ps, fct + 1.6f);
    Vector2 b1 = posAtTime(bs, fct + 1.6f);
    float d0 = vDist(p0, b0);
    float d1 = vDist(p1, b1);
    if (d1 < d0 - 35.0f) return "push";
    if (d1 > d0 + 35.0f) return "retreat";
    return "hold";
}

Route Analyst::dominantPreferredRoute(const PlayerModel& m) const {
    if (m.prefersLeftFlank >= m.prefersMid && m.prefersLeftFlank >= m.prefersRightFlank)
        return Route::LeftFlank;
    if (m.prefersRightFlank >= m.prefersMid && m.prefersRightFlank >= m.prefersLeftFlank)
        return Route::RightFlank;
    return Route::Mid;
}

// ---------------------------------------------------------------------------
// Analysis + gradual model update
// ---------------------------------------------------------------------------
RoundSummary Analyst::analyzeRound(const Map& map, const Telemetry& tel,
                                   const std::string& winner, float duration,
                                   int roundNumber, const std::string& botStrategy,
                                   PlayerModel& model,
                                   bool engageKnown, Vector2 engagePos) {
    RoundSummary s;
    s.round_number = roundNumber;
    s.winner = winner;
    s.duration_seconds = duration;
    s.bot_strategy = botStrategy;

    Route route = detectRoute(map, tel);
    s.player_route = routeName(route);
    s.first_contact_time = firstContactTime(tel);
    s.peek_style = detectWidePeek(tel) ? "wide" : "narrow";
    s.contact_reaction = detectReaction(map, tel);

    for (auto& e : tel.all()) {
        if (e.type == EventType::FirstContact && s.first_contact_location == "none")
            s.first_contact_location = e.location;
        if (e.type == EventType::KillEvent) {
            if (e.actor == "player") s.player_kill_location = e.location;
            if (e.actor == "bot") s.bot_death_location = e.location;
        }
    }

    // ---- Update history & repeatability ----
    routeHistory.push_back(route);
    int N = (int)routeHistory.size();
    int window = std::min(4, N);
    int matchDom = 0;
    for (int i = N - window; i < N; ++i)
        if (routeHistory[i] == route) matchDom++;
    float repeat = (float)matchDom / (float)window;

    // ---- Overfitting / bait detection ----
    bool brokePattern = false;
    if (lastPredictedRoute != Route::Unknown && route != lastPredictedRoute &&
        lastPredictedConfidence > 0.5f) {
        brokePattern = true;
    }

    // ---- Gradual EMA updates ----
    const float a = 0.4f;
    PlayerModel::ema(model.prefersMid, route == Route::Mid ? 1.f : 0.f, a);
    PlayerModel::ema(model.prefersLeftFlank, route == Route::LeftFlank ? 1.f : 0.f, a);
    PlayerModel::ema(model.prefersRightFlank, route == Route::RightFlank ? 1.f : 0.f, a);
    float sum = model.prefersMid + model.prefersLeftFlank + model.prefersRightFlank;
    if (sum > 1e-4f) {
        model.prefersMid /= sum;
        model.prefersLeftFlank /= sum;
        model.prefersRightFlank /= sum;
    }

    float fct = s.first_contact_time;
    if (fct >= 0) {
        PlayerModel::ema(model.avgFirstContactTime, fct, a);
        PlayerModel::ema(model.fastRushTendency, clampf((9.0f - fct) / 6.0f, 0, 1), a);
        PlayerModel::ema(model.slowClearTendency, clampf((fct - 7.0f) / 6.0f, 0, 1), a);
    }

    auto ps = samplesOf(tel, EventType::PlayerPositionSample);
    float plen = pathLength(ps);
    PlayerModel::ema(model.campingTendency,
                     (plen < 260.0f && duration > 6.0f) ? 1.f : 0.f, a);
    PlayerModel::ema(model.widePeekTendency, s.peek_style == "wide" ? 1.f : 0.f, a);
    PlayerModel::ema(model.pushAfterContactTendency, s.contact_reaction == "push" ? 1.f : 0.f, a);
    PlayerModel::ema(model.retreatAfterContactTendency, s.contact_reaction == "retreat" ? 1.f : 0.f, a);

    float aggSample = s.contact_reaction == "push" ? 0.85f
                    : s.contact_reaction == "retreat" ? 0.2f : 0.5f;
    aggSample = std::max(aggSample, model.fastRushTendency * 0.8f);
    PlayerModel::ema(model.aggression, aggSample, a);

    PlayerModel::ema(model.routeRepeatability, repeat, a);

    // Learn WHERE the player engages/kills from so the bot can pre-aim it.
    if (engageKnown) {
        if (model.engageSamples == 0) {
            model.favEngageX = engagePos.x;
            model.favEngageY = engagePos.y;
        } else {
            PlayerModel::ema(model.favEngageX, engagePos.x, a);
            PlayerModel::ema(model.favEngageY, engagePos.y, a);
        }
        model.engageSamples++;
    }

    // Confidence grows with #observations and consistency; bait resets it down.
    float base = (float)std::min(N, 4) / 4.0f;
    model.confidence = clampf(base * (0.4f + 0.6f * model.routeRepeatability), 0, 1);
    if (brokePattern) model.confidence *= 0.4f;

    // ---- Human-readable observed patterns ----
    int domCount = 0;
    for (int i = N - window; i < N; ++i)
        if (routeHistory[i] == route) domCount++;
    {
        std::ostringstream os;
        os << "Player used the " << routeName(route) << " route in " << domCount
           << " of the last " << window << " rounds";
        s.observed_patterns.push_back(os.str());
    }
    if (fct >= 0) {
        std::ostringstream os;
        os << "Player usually reaches first contact around "
           << (int)std::round(model.avgFirstContactTime) << " seconds";
        s.observed_patterns.push_back(os.str());
    }
    if (model.widePeekTendency > 0.55f)
        s.observed_patterns.push_back("Player tends to wide-peek corners");
    if (model.pushAfterContactTendency > 0.55f)
        s.observed_patterns.push_back("Player tends to push after first contact");
    if (model.retreatAfterContactTendency > 0.55f)
        s.observed_patterns.push_back("Player tends to retreat after first contact");
    if (model.campingTendency > 0.55f)
        s.observed_patterns.push_back("Player plays slowly / holds position");
    if (winner == "player" && botStrategy == "HoldCommonAngle")
        s.observed_patterns.push_back("Bot lost while holding a predictable common angle");
    if (model.engageSamples >= 2) {
        // Nearest named location of the learned engagement spot.
        std::string best = "unknown";
        float bestD = 1e18f;
        for (auto& kv : map.named) {
            float d = vDistSq({model.favEngageX, model.favEngageY}, kv.second);
            if (d < bestD) { bestD = d; best = kv.first; }
        }
        s.observed_patterns.push_back("Player repeatedly engages the bot from near " + best);
    }
    if (brokePattern) {
        std::ostringstream os;
        os << "Player broke the predicted " << routeName(lastPredictedRoute)
           << " pattern (possible bait)";
        s.observed_patterns.push_back(os.str());
    }

    model.confidence = clampf(model.confidence, 0, 1);
    s.model_snapshot = model;
    return s;
}

// ---------------------------------------------------------------------------
// Rule-based counter-strategy (fallback / no-LLM mode)
// ---------------------------------------------------------------------------
StrategyDecision Analyst::ruleBasedDecision(const PlayerModel& model,
                                            const RoundSummary& last) {
    StrategyDecision d;
    d.source = "rules";

    Route pref = dominantPreferredRoute(model);
    float conf = model.confidence;
    d.confidence = conf;
    d.params.watch_route = pref;
    d.params.expected_contact_time_min = std::max(2.0f, model.avgFirstContactTime - 2.0f);
    d.params.expected_contact_time_max = model.avgFirstContactTime + 3.0f;
    d.params.rotate_after_seconds = model.avgFirstContactTime + 5.0f;

    const char* rk = routeKey(pref);

    auto offAngleFor = [&](Route r) -> std::string {
        if (r == Route::LeftFlank) return "left_flank";
        if (r == Route::RightFlank) return "long_angle";
        return "crate_off_angle";
    };
    auto laneHold = [&](Route r) -> std::string {
        if (r == Route::LeftFlank) return "left_flank";
        if (r == Route::RightFlank) return "right_flank";
        return "mid_box";
    };

    float flankPref = std::max(model.prefersLeftFlank, model.prefersRightFlank);
    float roll = rng().f01(); // mix-up factor: keeps the bot unpredictable

    // Helper to configure an aggressive push / rush toward a lane.
    auto makeRush = [&](StrategyType t, Route r, const std::string& pat,
                        const std::string& why) {
        d.type = t;
        d.params.watch_route = r;
        d.params.hold_position = laneHold(r);
        d.params.aggression = 0.9f;
        d.params.risk = 0.7f;
        d.params.scan = 0.3f;
        d.player_pattern = pat;
        d.reason = why;
    };

    // The bot lost a lot recently if the player is on a streak; bias toward
    // taking initiative (rushing) rather than passively holding.
    bool pressure = (last.winner == "player");

    if (conf < 0.3f) {
        // Not enough info: alternate between probing rushes and rotating holds
        // so the player can't settle into a free pattern unopposed.
        if (roll < 0.45f) {
            makeRush(StrategyType::FastRush, pref, "probe_rush",
                     "Low confidence; rushing to probe the player and force early "
                     "contact instead of giving free space.");
        } else {
            d.type = StrategyType::RotateAfterNoContact;
            d.player_pattern = "exploring";
            d.reason = "Low confidence; rotating angles to gather information.";
            d.params.hold_position = laneHold(pref);
            d.params.aggression = 0.55f;
            d.params.risk = 0.45f;
            d.params.scan = 0.55f;
        }
    } else if (model.campingTendency > 0.5f || model.slowClearTendency > 0.55f) {
        // Player is passive: punish it by taking space aggressively.
        makeRush(roll < 0.5f ? StrategyType::FastRush : StrategyType::AggressivePush,
                 pref, "player_camps",
                 "Player plays slow/passive; rushing to take map control and deny "
                 "the comfortable hold.");
    } else if (pref != Route::Mid && flankPref > 0.45f && conf >= 0.5f) {
        if (roll < 0.5f) {
            d.type = StrategyType::AntiFlankTrap;
            d.player_pattern = std::string(rk) + "_flank";
            d.reason = std::string("Player keeps taking the ") + rk +
                       " flank; setting an anti-flank trap there.";
            d.params.hold_position = laneHold(pref);
            d.params.aggression = 0.55f;
            d.params.risk = 0.5f;
            d.params.scan = 0.5f;
        } else {
            makeRush(StrategyType::FastRush, pref, std::string(rk) + "_flank",
                     std::string("Player favours the ") + rk +
                     " flank; rushing it to catch them in the lane.");
        }
    } else if (model.fastRushTendency > 0.55f && conf >= 0.45f) {
        // Player rushes early: either hold an off-angle to punish the peek,
        // or counter-rush to meet them head-on and trade on the bot's terms.
        if (roll < 0.45f) {
            makeRush(StrategyType::AggressivePush, pref,
                     std::string("counter_") + rk + "_rush",
                     std::string("Player rushes ") + rk +
                     " every round; counter-rushing to contest the lane early.");
        } else {
            d.type = StrategyType::HoldOffAngle;
            d.player_pattern = std::string("fast_") + rk + "_rush";
            d.reason = std::string("Player rushes ") + rk +
                       " early; holding an off-angle to beat the predictable peek.";
            d.params.hold_position = offAngleFor(pref);
            d.params.aggression = 0.5f;
            d.params.risk = 0.5f;
            d.params.scan = 0.45f;
            d.params.rotate_after_seconds = model.avgFirstContactTime + 4.0f;
        }
    } else if (conf >= 0.6f && model.routeRepeatability > 0.6f) {
        // Strong read: intercept by rushing the lane or mirroring.
        if (roll < 0.55f) {
            makeRush(StrategyType::FastRush, pref, std::string(rk) + "_repeat",
                     std::string("Player strongly repeats the ") + rk +
                     " route; rushing to intercept it on contact.");
        } else {
            d.type = StrategyType::MirrorPlayerRoute;
            d.player_pattern = std::string(rk) + "_repeat";
            d.reason = std::string("Player strongly repeats the ") + rk +
                       " route; mirroring it to intercept.";
            d.params.hold_position = laneHold(pref);
            d.params.aggression = 0.7f;
            d.params.risk = 0.55f;
        }
    } else {
        // Moderate read: mostly take initiative, occasionally hold.
        if (roll < 0.5f || pressure) {
            makeRush(StrategyType::AggressivePush, pref, "mixed_push",
                     std::string("Moderate read; pushing ") + rk +
                     " to pressure the player.");
        } else {
            d.type = StrategyType::HoldCommonAngle;
            d.player_pattern = "mixed";
            d.reason = std::string("Moderate read; holding a solid angle on ") + rk + ".";
            d.params.hold_position = "mid_box";
            d.params.aggression = 0.55f;
            d.params.risk = 0.5f;
            d.params.scan = 0.45f;
        }
    }

    // Don't repeat a losing predictable hold.
    if (last.winner == "player" && last.bot_strategy == "HoldCommonAngle" &&
        d.type == StrategyType::HoldCommonAngle) {
        d.type = StrategyType::HoldOffAngle;
        d.params.hold_position = offAngleFor(pref);
        d.reason += " Switching to an off-angle after losing a predictable hold.";
    }

    d.params.clamp();
    return d;
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------
std::string Analyst::summaryToJson(const RoundSummary& s) {
    json j;
    j["round_number"] = s.round_number;
    j["winner"] = s.winner;
    j["duration_seconds"] = std::round(s.duration_seconds * 10) / 10.0;
    j["player_route"] = s.player_route;
    j["bot_strategy"] = s.bot_strategy;
    j["first_contact_time"] = std::round(s.first_contact_time * 10) / 10.0;
    j["first_contact_location"] = s.first_contact_location;
    j["player_kill_location"] = s.player_kill_location;
    j["bot_death_location"] = s.bot_death_location;
    j["peek_style"] = s.peek_style;
    j["contact_reaction"] = s.contact_reaction;
    j["bot_was_flanked"] = s.bot_was_flanked;
    j["bot_death_cause"] = s.bot_death_cause;
    j["observed_patterns"] = s.observed_patterns;
    j["current_player_model"] = json::parse(modelToJson(s.model_snapshot));
    return j.dump(2);
}

std::string Analyst::modelToJson(const PlayerModel& m) {
    auto r2 = [](float v) { return std::round(v * 100) / 100.0; };
    json j;
    j["aggression"] = r2(m.aggression);
    j["routeRepeatability"] = r2(m.routeRepeatability);
    j["prefersMid"] = r2(m.prefersMid);
    j["prefersLeftFlank"] = r2(m.prefersLeftFlank);
    j["prefersRightFlank"] = r2(m.prefersRightFlank);
    j["campingTendency"] = r2(m.campingTendency);
    j["fastRushTendency"] = r2(m.fastRushTendency);
    j["slowClearTendency"] = r2(m.slowClearTendency);
    j["widePeekTendency"] = r2(m.widePeekTendency);
    j["pushAfterContactTendency"] = r2(m.pushAfterContactTendency);
    j["retreatAfterContactTendency"] = r2(m.retreatAfterContactTendency);
    j["avgFirstContactTime"] = r2(m.avgFirstContactTime);
    j["confidence"] = r2(m.confidence);
    j["engageSamples"] = m.engageSamples;
    if (m.engageSamples > 0)
        j["favEngage"] = {std::round(m.favEngageX), std::round(m.favEngageY)};
    return j.dump(2);
}

// ---------------------------------------------------------------------------
// LLM prompt building / response parsing
// ---------------------------------------------------------------------------
std::string Analyst::buildSystemPrompt() const {
    std::ostringstream os;
    os << "You are the tactical STRATEGIST for a 1v1 bot in a small top-down shooter. "
          "You are NOT the bot controller and you do NOT pick the final move directly. "
          "A fast self-play SIMULATOR will test your ideas and choose the best one. Your "
          "job is to (1) PREDICT what the player will do NEXT round, and (2) PROPOSE a few "
          "candidate counter-strategies for the simulator to try. You must NOT invent new "
          "actions and must NOT make the bot cheat (no perfect aim, no lower reaction "
          "time, no wallhack).\n\n"
          "Think about the META-GAME, which the simulator cannot: the player may be "
          "BAITING. If the player repeated a route and you have been countering it, they "
          "may switch/flank next - predict that. If they just broke a pattern, they may "
          "return to it.\n\n"
          "Allowed strategies for candidates:\n"
          "HoldCommonAngle, HoldOffAngle, FastRush, SlowClear, DelayedPush, "
          "FakeSoundBait, AntiFlankTrap, RotateAfterNoContact, DefensiveHold, "
          "AggressivePush, RetreatAndRepeek, MirrorPlayerRoute.\n"
          "Allowed hold_position: player_spawn, bot_spawn, mid_entry, mid_box, "
          "left_flank, right_flank, long_angle, crate_off_angle, back_site.\n"
          "Routes: mid, left_flank, right_flank.\n"
          "Params per candidate: hold_position, watch_route, expected_contact_time_min, "
          "expected_contact_time_max, rotate_after_seconds, aggression(0..1), risk(0..1), "
          "scan(0..1 = how much the bot checks its flanks/back).\n\n"
          "Propose 2-3 VARIED candidates (e.g. one that counters the repeated route and "
          "one that anticipates a bait/flank), so the simulator has good options.\n\n"
          "Respond with STRICT JSON ONLY in this schema:\n"
          "{\n"
          "  \"player_pattern\": \"string\",\n"
          "  \"confidence\": 0.0,\n"
          "  \"reason\": \"short explanation of your prediction\",\n"
          "  \"predicted_next\": { \"route\": \"mid|left_flank|right_flank\", "
          "\"will_bait\": false, \"expected_first_contact\": 6.0 },\n"
          "  \"candidates\": [\n"
          "    { \"chosen_strategy\": \"OneOfAllowed\", \"strategy_params\": { "
          "\"hold_position\": \"oneOfAllowed\", \"watch_route\": \"mid\", "
          "\"rotate_after_seconds\": 10.0, \"aggression\": 0.5, \"risk\": 0.5, "
          "\"scan\": 0.5 } }\n"
          "  ]\n"
          "}";
    return os.str();
}

std::string Analyst::buildResponseSchema() const {
    json strategies = json::array();
    for (int i = 0; i < (int)StrategyType::COUNT; ++i)
        strategies.push_back(strategyName((StrategyType)i));

    json params = {
        {"type", "object"},
        {"properties", {
            {"hold_position", {{"type", "string"}}},
            {"watch_route", {{"type", "string"}, {"enum", {"mid", "left_flank", "right_flank"}}}},
            {"expected_contact_time_min", {{"type", "number"}}},
            {"expected_contact_time_max", {{"type", "number"}}},
            {"rotate_after_seconds", {{"type", "number"}}},
            {"aggression", {{"type", "number"}}},
            {"risk", {{"type", "number"}}},
            {"scan", {{"type", "number"}}}
        }},
        {"required", {"hold_position", "watch_route", "aggression", "risk", "scan"}}
    };

    json candidate = {
        {"type", "object"},
        {"properties", {
            {"chosen_strategy", {{"type", "string"}, {"enum", strategies}}},
            {"strategy_params", params}
        }},
        {"required", {"chosen_strategy", "strategy_params"}}
    };

    json predicted = {
        {"type", "object"},
        {"properties", {
            {"route", {{"type", "string"}, {"enum", {"mid", "left_flank", "right_flank"}}}},
            {"will_bait", {{"type", "boolean"}}},
            {"expected_first_contact", {{"type", "number"}}}
        }},
        {"required", {"route"}}
    };

    json schema = {
        {"type", "object"},
        {"properties", {
            {"player_pattern", {{"type", "string"}}},
            {"confidence", {{"type", "number"}}},
            {"reason", {{"type", "string"}}},
            {"predicted_next", predicted},
            {"candidates", {{"type", "array"}, {"items", candidate}}}
        }},
        {"required", {"predicted_next", "candidates", "reason", "confidence", "player_pattern"}}
    };
    return schema.dump();
}

std::string Analyst::buildUserPrompt(const RoundSummary& s, const PlayerModel& model) const {
    std::ostringstream os;
    os << "Round summary:\n" << summaryToJson(s)
       << "\n\nChoose the counter-strategy for the NEXT round. Strict JSON only.";
    return os.str();
}

bool Analyst::parseLLMResponse(const std::string& content, const Map& map,
                               StrategyDecision& out, std::string& error) const {
    // Extract the JSON object even if the model wrapped it in prose / code fences.
    size_t a = content.find('{');
    size_t b = content.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a) {
        error = "no JSON object found";
        return false;
    }
    std::string body = content.substr(a, b - a + 1);

    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        error = std::string("json parse error: ") + e.what();
        return false;
    }

    if (!j.contains("chosen_strategy") || !j["chosen_strategy"].is_string()) {
        error = "missing chosen_strategy";
        return false;
    }
    StrategyType type;
    if (!strategyFromName(j["chosen_strategy"].get<std::string>(), type)) {
        error = "unknown strategy: " + j["chosen_strategy"].get<std::string>();
        return false;
    }

    out = StrategyDecision{};
    out.source = "llm";
    out.type = type;
    out.player_pattern = j.value("player_pattern", std::string("unknown"));
    out.confidence = clampf((float)j.value("confidence", 0.5), 0.0f, 1.0f);
    out.reason = j.value("reason", std::string(""));

    StrategyParams p;
    if (j.contains("strategy_params") && j["strategy_params"].is_object()) {
        const auto& sp = j["strategy_params"];
        p.hold_position = sp.value("hold_position", std::string("mid_box"));
        p.expected_contact_time_min = (float)sp.value("expected_contact_time_min", 5.0);
        p.expected_contact_time_max = (float)sp.value("expected_contact_time_max", 10.0);
        p.rotate_after_seconds = (float)sp.value("rotate_after_seconds", 12.0);
        p.aggression = (float)sp.value("aggression", 0.5);
        p.risk = (float)sp.value("risk", 0.5);
        p.scan = (float)sp.value("scan", 0.3);
        p.watch_route = routeFromKey(sp.value("watch_route", std::string("mid")));
    }
    // Validate hold_position against allowed named locations; fall back if unknown.
    if (map.named.find(p.hold_position) == map.named.end())
        p.hold_position = "mid_box";
    p.clamp();
    out.params = p;
    return true;
}

// Shared: parse a strategy_params object (clamped, validated).
static StrategyParams parseParamsObject(const json& sp, const Map& map) {
    StrategyParams p;
    p.hold_position = sp.value("hold_position", std::string("mid_box"));
    p.expected_contact_time_min = (float)sp.value("expected_contact_time_min", 5.0);
    p.expected_contact_time_max = (float)sp.value("expected_contact_time_max", 10.0);
    p.rotate_after_seconds = (float)sp.value("rotate_after_seconds", 12.0);
    p.aggression = (float)sp.value("aggression", 0.5);
    p.risk = (float)sp.value("risk", 0.5);
    p.scan = (float)sp.value("scan", 0.4);
    p.watch_route = routeFromKey(sp.value("watch_route", std::string("mid")));
    if (map.named.find(p.hold_position) == map.named.end())
        p.hold_position = "mid_box";
    p.clamp();
    return p;
}

bool Analyst::parseLLMPlan(const std::string& content, const Map& map, LLMPlan& out,
                           std::string& error) const {
    size_t a = content.find('{');
    size_t b = content.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b <= a) {
        error = "no JSON object found";
        return false;
    }
    json j;
    try {
        j = json::parse(content.substr(a, b - a + 1));
    } catch (const std::exception& e) {
        error = std::string("json parse error: ") + e.what();
        return false;
    }

    out = LLMPlan{};
    out.player_pattern = j.value("player_pattern", std::string("unknown"));
    out.confidence = clampf((float)j.value("confidence", 0.5), 0.0f, 1.0f);
    out.reason = j.value("reason", std::string(""));

    if (j.contains("predicted_next") && j["predicted_next"].is_object()) {
        const auto& pn = j["predicted_next"];
        out.predicted.route = routeFromKey(pn.value("route", std::string("mid")));
        out.predicted.willBait = pn.value("will_bait", false);
        out.predicted.expectedContact = (float)pn.value("expected_first_contact", 6.0);
    } else {
        error = "missing predicted_next";
        return false;
    }

    if (j.contains("candidates") && j["candidates"].is_array()) {
        for (const auto& c : j["candidates"]) {
            if (!c.is_object() || !c.contains("chosen_strategy")) continue;
            StrategyType t;
            if (!strategyFromName(c.value("chosen_strategy", std::string("")), t)) continue;
            StrategyDecision d;
            d.type = t;
            d.source = "llm_candidate";
            d.player_pattern = out.player_pattern;
            d.confidence = out.confidence;
            d.reason = out.reason;
            if (c.contains("strategy_params") && c["strategy_params"].is_object())
                d.params = parseParamsObject(c["strategy_params"], map);
            out.candidates.push_back(d);
        }
    }
    return true; // candidates may be empty; the prediction alone is still useful
}
