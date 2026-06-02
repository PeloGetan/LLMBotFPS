#include "Game.h"
#include "Rng.h"
#include "Log.h"
#include "Rollout.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>

using json = nlohmann::json;

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;
static const int PANEL_X = 900;
static const int PANEL_W = SCREEN_W - PANEL_X;

static const float PLAYER_SPEED = 215.0f;
static const float PLAYER_FIRE_CD = 0.16f;
static const float PLAYER_DMG = 36.0f;
static const float PLAYER_RANGE = 780.0f;

Game::Game() {
    Log::init("llmbotfps_log.txt");
    config = Config::load("config.json");
    roundTimeLimit = config.round_time_limit;

    llm = std::make_unique<LLMService>(config);
    llm->startServerIfConfigured();
    llm->tryAutoDownload(); // fetch server+model in a visible window if missing
    if (config.llm_enabled && llm->modelPresent()) llmReachable = llm->serverReachable();

    Log::write(std::string("Config: llm_enabled=") + (config.llm_enabled ? "true" : "false") +
               " autostart=" + (config.llm_autostart_server ? "true" : "false") +
               " reachable=" + (llmReachable ? "true" : "false") +
               " server_url=" + config.server_url +
               " win_streak_goal=" + std::to_string(config.win_streak_goal));

    // Initial strategy: no data yet, hold a common mid angle.
    nextDecision = StrategyDecision{};
    nextDecision.type = StrategyType::HoldCommonAngle;
    nextDecision.params.hold_position = "mid_box";
    nextDecision.params.watch_route = Route::Mid;
    nextDecision.confidence = 0.0f;
    nextDecision.player_pattern = "unknown";
    nextDecision.reason = "Initial round: no data yet, holding a common mid angle.";
    nextDecision.source = "default";
    currentDecision = nextDecision;
}

std::string Game::nearestName(Vector2 p) const {
    std::string best = "unknown";
    float bestD = std::numeric_limits<float>::max();
    for (auto& kv : map.named) {
        float d = vDistSq(p, kv.second);
        if (d < bestD) { bestD = d; best = kv.first; }
    }
    return best;
}

std::string Game::decisionToJson(const StrategyDecision& d) const {
    json j;
    j["player_pattern"] = d.player_pattern;
    j["confidence"] = std::round(d.confidence * 100) / 100.0;
    j["chosen_strategy"] = strategyName(d.type);
    j["reason"] = d.reason;
    j["source"] = d.source;
    json p;
    p["hold_position"] = d.params.hold_position;
    p["watch_route"] = routeKey(d.params.watch_route);
    p["expected_contact_time_min"] = std::round(d.params.expected_contact_time_min * 10) / 10.0;
    p["expected_contact_time_max"] = std::round(d.params.expected_contact_time_max * 10) / 10.0;
    p["rotate_after_seconds"] = std::round(d.params.rotate_after_seconds * 10) / 10.0;
    p["aggression"] = std::round(d.params.aggression * 100) / 100.0;
    p["risk"] = std::round(d.params.risk * 100) / 100.0;
    p["scan"] = std::round(d.params.scan * 100) / 100.0;
    j["strategy_params"] = p;
    return j.dump(2);
}

void Game::startRound() {
    roundNumber++;
    currentDecision = nextDecision;

    player.reset(map.pos("player_spawn"));
    player.facing = PI / 2.0f; // facing down toward bot
    bot.reset(map.pos("bot_spawn"));
    bot.facing = -PI / 2.0f;

    botCtrl.beginRound(map, currentDecision, playerModel, map.pos("bot_spawn"));
    // Start the round already pre-aiming the spot the player usually attacks from.
    {
        Vector2 wp = botCtrl.watchAimPoint();
        if (vDistSq(wp, bot.pos) > 1.0f) bot.facing = vAngle(wp - bot.pos);
    }

    tel.begin();
    tel.add(EventType::RoundStart, 0.0f, player.pos, "player", "player_spawn");

    roundTime = 0.0f;
    sampleTimer = 0.0f;
    playerFireCd = 0.0f;
    playerFiredTimer = 0.0f;
    firstShotLogged = false;
    playerContactLogged = false;
    botWorstHitAngle = 0.0f;
    botHitFromOutside = false;
    engageCaptured = false;
    engagePos = {0, 0};
    tracers.clear();
    state = State::Playing;

    Log::section("ROUND " + std::to_string(roundNumber) + " START");
    Log::write("Bot strategy applied this round:\n" + decisionToJson(currentDecision));
}

void Game::endRound(const std::string& winner) {
    lastWinner = winner;
    tel.add(EventType::RoundEnd, roundTime,
            winner == "player" ? bot.pos : player.pos, winner, "");

    if (winner != "timeout") {
        tel.add(EventType::KillEvent, roundTime, player.pos, "player", nearestName(player.pos));
        tel.add(EventType::KillEvent, roundTime, bot.pos, "bot", nearestName(bot.pos));
    }

    lastSummary = analyst.analyzeRound(map, tel, winner, roundTime, roundNumber,
                                       strategyName(currentDecision.type), playerModel,
                                       engageCaptured, engagePos);

    // Record how the round went for the bot (used by the parameter tuner / LLM).
    lastSummary.bot_worst_hit_angle = botWorstHitAngle;
    lastSummary.bot_was_flanked = botHitFromOutside;
    if (winner == "player")
        lastSummary.bot_death_cause = botHitFromOutside ? "flanked" : "duel";
    else
        lastSummary.bot_death_cause = "none";
    if (winner == "player" && botHitFromOutside)
        lastSummary.observed_patterns.push_back("Bot was flanked / shot from outside its view");

    tuner.record(currentDecision.type, currentDecision.params, winner, lastSummary);

    if (winner == "player") winStreak++;
    else if (winner == "bot") winStreak = 0;

    Log::section("ROUND " + std::to_string(roundNumber) + " END");
    Log::write("Winner=" + winner + "  duration=" + std::to_string(roundTime) +
               "s  win_streak=" + std::to_string(winStreak) + "/" +
               std::to_string(config.win_streak_goal));
    Log::write("Round summary:\n" + Analyst::summaryToJson(lastSummary));

    if (winStreak >= config.win_streak_goal) {
        Log::write("PLAYER REACHED THE WIN STREAK GOAL - VICTORY");
        state = State::Victory;
        return;
    }
    beginAnalysis();
}

void Game::beginAnalysis() {
    analysisRunning = true;
    state = State::Analyzing;
    analysisFuture = std::async(std::launch::async,
        [this, s = lastSummary, m = playerModel]() {
            return runAnalysis(s, m);
        });
}

AnalysisResult Game::runAnalysis(RoundSummary summary, PlayerModel model) {
    AnalysisResult r;
    StrategyDecision ruleDec = analyst.ruleBasedDecision(model, summary);
    r.fromLLM = false;

    Log::section("ANALYSIS for next round (after round " +
                 std::to_string(summary.round_number) + ")");

    // ---- Score the PREVIOUS LLM prediction against what actually happened ----
    if (lastLLMPredValid) {
        Route actual = routeFromKey(summary.player_route);
        bool correct = (actual == lastLLMPredRoute);
        PlayerModel::ema(llmTrust, correct ? 1.0f : 0.0f, 0.5f);
        Log::write(std::string("LLM previous prediction: ") + routeKey(lastLLMPredRoute) +
                   " | actual: " + summary.player_route + " -> " +
                   (correct ? "CORRECT" : "WRONG") + ". llmTrust=" +
                   std::to_string((int)std::round(llmTrust * 100)) + "%");
        lastLLMPredValid = false;
    }

    // ---- Data-driven profile of the player (from history) ----
    PlayerSimProfile profileData = Rollout::profileFrom(map, model, summary);

    // ---- Ask the LLM strategist: predict next move + propose candidates ----
    // If an auto-download finished during play, start the server now.
    if (llm && llm->enabled() && !llm->serverStarted() && llm->modelPresent())
        llm->startServerIfConfigured();
    bool reachable = llmReachable;
    if (llm && llm->enabled() && !reachable && llm->serverStarted()) {
        reachable = llm->serverReachable();
        if (reachable) llmReachable = true;
    }

    Analyst::LLMPlan plan;
    bool havePlan = false;
    if (llm && llm->enabled() && reachable) {
        std::string sys = analyst.buildSystemPrompt();
        std::string usr = analyst.buildUserPrompt(summary, model) +
                          "\n\nYour past choices vs this player:\n" + tuner.report() +
                          "\nYour recent prediction accuracy (llmTrust) is " +
                          std::to_string((int)std::round(llmTrust * 100)) + "%.";
        Log::write("LLM strategist - requesting prediction + candidates.");
        Log::write("LLM user prompt:\n" + usr);
        std::string content, err, perr;
        if (llm->chat(sys, usr, content, err, analyst.buildResponseSchema())) {
            r.raw = content;
            Log::write("LLM raw response:\n" + content);
            if (analyst.parseLLMPlan(content, map, plan, perr)) {
                havePlan = true;
                std::ostringstream ps;
                ps << "predict route=" << routeKey(plan.predicted.route)
                   << " bait=" << (plan.predicted.willBait ? "yes" : "no")
                   << " | candidates: ";
                for (auto& c : plan.candidates) ps << strategyName(c.type) << " ";
                lastPrediction = ps.str() + " | trust=" +
                                 std::to_string((int)std::round(llmTrust * 100)) + "%";
                Log::write("LLM plan: " + lastPrediction + "\nreason: " + plan.reason);
            } else {
                r.error = "Invalid LLM plan: " + perr + " -> rollout-only.";
                Log::write(r.error);
            }
        } else {
            r.error = "LLM call failed: " + err + " -> rollout-only.";
            Log::write(r.error);
        }
    } else {
        Log::write("LLM disabled/unreachable - rollout-only.");
    }

    // ---- Build profiles + candidate list for the simulator (the JUDGE) ----
    std::vector<std::pair<PlayerSimProfile, float>> profiles;
    std::vector<StrategyDecision> candidates;

    if (havePlan) {
        // Anticipated profile from the LLM's prediction.
        PlayerSimProfile profLLM = profileData;
        profLLM.route = plan.predicted.route;
        if (plan.predicted.expectedContact > 1.0f)
            profLLM.firstContactTime = clampf(plan.predicted.expectedContact, 2.0f, 18.0f);
        Vector2 fav = {model.favEngageX, model.favEngageY};
        if (model.engageSamples >= 1 && map.routeOf(fav) == plan.predicted.route) {
            profLLM.engageKnown = true;
            profLLM.engagePos = fav;
        } else {
            profLLM.engageKnown = false;
            profLLM.engagePos = map.pos(plan.predicted.route == Route::LeftFlank ? "left_flank"
                                       : plan.predicted.route == Route::RightFlank ? "right_flank"
                                                                                   : "mid_box");
        }
        // Trust-weighted: a reliable LLM steers the bot toward its prediction.
        float wLLM = clampf(0.35f + 0.45f * llmTrust, 0.2f, 0.8f);
        profiles.push_back({profLLM, wLLM});
        profiles.push_back({profileData, 1.0f - wLLM});

        candidates = Rollout::buildCandidates(map, model, profLLM); // baseline (anticipated lane)
        for (auto& c : plan.candidates) candidates.push_back(c);     // + LLM hypotheses

        lastLLMPredRoute = plan.predicted.route;
        lastLLMPredValid = true;
    } else {
        profiles.push_back({profileData, 1.0f});
        candidates = Rollout::buildCandidates(map, model, profileData);
    }

    auto roll = Rollout::evaluateCandidates(map, model, candidates, profiles, 10, 18.0f);

    // ---- Rehearsal summary (debug/log) ----
    std::ostringstream rh;
    if (havePlan)
        rh << "Anticipating route=" << routeKey(plan.predicted.route)
           << " (llmTrust " << (int)std::round(llmTrust * 100) << "%). ";
    rh << "Rehearsal win-rates:\n";
    for (int i = 0; i < (int)roll.size() && i < 6; ++i) {
        rh << "  " << strategyName(roll[i].decision.type) << ": "
           << (int)std::round(roll[i].winRate * 100) << "%"
           << (roll[i].decision.source == "llm_candidate" ? " [LLM]" : "")
           << "  (watch=" << routeKey(roll[i].decision.params.watch_route) << ")\n";
    }
    r.rehearsal = rh.str();
    Log::write(r.rehearsal);

    // ---- Pick the winner (random among equally-best) ----
    StrategyDecision best = ruleDec;
    if (!roll.empty()) {
        float top = roll.front().winRate;
        std::vector<int> ties;
        for (int i = 0; i < (int)roll.size(); ++i)
            if (roll[i].winRate >= top - 0.001f) ties.push_back(i);
        int pick = ties[(int)(rng().f01() * ties.size()) % (int)ties.size()];
        best = roll[pick].decision;
        best.confidence = havePlan ? plan.confidence : model.confidence;
        bool fromLLMcand = (best.source == "llm_candidate");
        r.fromLLM = havePlan; // the LLM shaped the search even if a baseline won
        std::ostringstream br;
        br << (fromLLMcand ? "LLM-proposed " : "baseline ") << strategyName(best.type)
           << " won the rehearsal (" << (int)std::round(top * 100) << "%).";
        if (havePlan) br << " Anticipating you go " << routeKey(plan.predicted.route)
                         << (plan.predicted.willBait ? " (bait)." : ".");
        if (!plan.reason.empty()) br << " LLM: " << plan.reason;
        best.reason = br.str();
        best.source = fromLLMcand ? "llm_candidate"
                    : (havePlan ? "rollout(llm-anticipated)" : "rollout");
    }
    r.decision = best;
    if (!havePlan) r.raw = r.raw.empty() ? decisionToJson(best) : r.raw;

    // Parameter tuner guards on top.
    r.tuneNotes = tuner.tune(r.decision);
    Log::write("Parameter tuner: " + r.tuneNotes);
    Log::write("Final decision for next round (source=" + r.decision.source + "):\n" +
               decisionToJson(r.decision));
    return r;
}

void Game::pollAnalysis() {
    if (!analysisRunning) return;
    if (analysisFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    AnalysisResult r = analysisFuture.get();
    nextDecision = r.decision;
    analysisSource = nextDecision.source;
    validatedStrategyJson = decisionToJson(nextDecision);
    llmRaw = r.raw.empty() ? "(no raw content)" : r.raw;
    llmError = r.error;
    lastTuneNotes = r.tuneNotes;
    lastRehearsal = r.rehearsal;

    // Record what we now expect, so a broken pattern can be detected next round.
    analyst.lastPredictedRoute = nextDecision.params.watch_route;
    analyst.lastPredictedConfidence = nextDecision.confidence;

    analysisRunning = false;
    state = State::Intro;
}

bool Game::hitscan(Vector2 origin, float angle, const Entity& target, float maxRange,
                   Vector2& outHit) const {
    if (!target.alive) return false;
    Vector2 dir = vFromAngle(angle);
    Vector2 to = target.pos - origin;
    float along = vDot(to, dir);
    if (along < 0 || along > maxRange) return false;
    Vector2 closest = origin + dir * along;
    float perp = vDist(closest, target.pos);
    if (perp > target.radius) return false;
    if (!map.hasLineOfSight(origin, target.pos)) return false;
    outHit = target.pos;
    return true;
}

void Game::handlePlayerCombat(float dt) {
    playerFireCd -= dt;
    if (playerFiredTimer > 0) playerFiredTimer -= dt;

    bool fired = IsMouseButtonDown(MOUSE_BUTTON_LEFT) && playerFireCd <= 0.0f && player.alive;
    if (!fired) return;

    playerFireCd = PLAYER_FIRE_CD;
    playerFiredTimer = 0.25f;

    if (!firstShotLogged) {
        tel.add(EventType::FirstShot, roundTime, player.pos, "player", nearestName(player.pos));
        firstShotLogged = true;
    }

    Vector2 hit;
    Vector2 endPoint = player.pos + vFromAngle(player.facing) * PLAYER_RANGE;
    bool didHit = hitscan(player.pos, player.facing, bot, PLAYER_RANGE, hit);
    if (didHit) endPoint = hit;
    tracers.push_back({player.pos, endPoint, 0.06f, SKYBLUE});

    if (didHit) {
        // Was the bot hit from outside its field of view (i.e. flanked)?
        float angOff = std::fabs(angleDiff(bot.facing, vAngle(player.pos - bot.pos)));
        botWorstHitAngle = std::max(botWorstHitAngle, angOff);
        if (angOff > botCtrl.aim.fovHalf) botHitFromOutside = true;

        if (!engageCaptured) { engagePos = player.pos; engageCaptured = true; }
        bot.damage(PLAYER_DMG);
        botCtrl.onDamaged(player.pos); // bot reacts to the threat direction
        tel.add(EventType::DamageEvent, roundTime, bot.pos, "player",
                nearestName(bot.pos), PLAYER_DMG);
        if (!bot.alive) endRound("player");
    }
}

void Game::updatePlaying(float dt) {
    roundTime += dt;

    // ---- Player movement ----
    Vector2 mv{0, 0};
    if (IsKeyDown(KEY_W)) mv.y -= 1;
    if (IsKeyDown(KEY_S)) mv.y += 1;
    if (IsKeyDown(KEY_A)) mv.x -= 1;
    if (IsKeyDown(KEY_D)) mv.x += 1;
    if (mv.x != 0 || mv.y != 0) {
        mv = vNorm(mv);
        Vector2 step = mv * (PLAYER_SPEED * dt);
        Vector2 next = player.pos + step;
        if (!map.collides(next, player.radius)) player.pos = next;
        else {
            Vector2 nx = {player.pos.x + step.x, player.pos.y};
            Vector2 ny = {player.pos.x, player.pos.y + step.y};
            if (!map.collides(nx, player.radius)) player.pos = nx;
            else if (!map.collides(ny, player.radius)) player.pos = ny;
        }
    }

    // Aim toward mouse (only when cursor is over the play area).
    Vector2 m = virtualMouse;
    if (m.x < PANEL_X) {
        Vector2 aimDir = m - player.pos;
        if (vLenSq(aimDir) > 1.0f) player.facing = vAngle(aimDir);
    }

    // ---- Telemetry sampling ----
    sampleTimer += dt;
    if (sampleTimer >= 0.12f) {
        sampleTimer = 0.0f;
        tel.add(EventType::PlayerPositionSample, roundTime, player.pos, "player",
                nearestName(player.pos));
        tel.add(EventType::BotPositionSample, roundTime, bot.pos, "bot",
                nearestName(bot.pos));
    }

    // Player "first contact" (sees the bot).
    if (!playerContactLogged && player.alive && bot.alive &&
        map.hasLineOfSight(player.pos, bot.pos) &&
        vDist(player.pos, bot.pos) < PLAYER_RANGE) {
        tel.add(EventType::FirstContact, roundTime, bot.pos, "player", nearestName(bot.pos));
        playerContactLogged = true;
    }

    handlePlayerCombat(dt);
    if (state != State::Playing) return; // round may have ended

    // ---- Bot update ----
    botCtrl.update(dt, roundTime, map, bot, player, tel, playerFiredTimer > 0.0f);
    if (botCtrl.wantsShoot && bot.alive) {
        botFiredFx = 0.06f;
        Vector2 hit;
        Vector2 endPoint = botCtrl.shootFrom + vFromAngle(botCtrl.shootAngle) * botCtrl.aim.maxRange;
        bool didHit = hitscan(botCtrl.shootFrom, botCtrl.shootAngle, player,
                              botCtrl.aim.maxRange, hit);
        if (didHit) endPoint = hit;
        tracers.push_back({botCtrl.shootFrom, endPoint, 0.06f, ORANGE});
        if (didHit) {
            player.damage(botCtrl.aim.damagePerShot);
            tel.add(EventType::DamageEvent, roundTime, player.pos, "bot",
                    nearestName(player.pos), botCtrl.aim.damagePerShot);
            if (!player.alive) { endRound("bot"); return; }
        }
    }
    if (botFiredFx > 0) botFiredFx -= dt;

    // ---- Time limit ----
    if (roundTime >= roundTimeLimit) endRound("timeout");
}

void Game::update(float dt) {
    for (auto& t : tracers) t.life -= dt;
    tracers.erase(std::remove_if(tracers.begin(), tracers.end(),
                  [](const Tracer& t) { return t.life <= 0; }), tracers.end());

    switch (state) {
        case State::Intro:
            if (IsKeyPressed(KEY_SPACE)) startRound();
            break;
        case State::Playing:
            updatePlaying(dt);
            break;
        case State::Analyzing:
            pollAnalysis();
            break;
        case State::Victory:
            if (IsKeyPressed(KEY_R)) {
                playerModel = PlayerModel{};
                analyst = Analyst{};
                tuner = StrategyTuner{};
                llmTrust = 0.5f;
                lastLLMPredValid = false;
                winStreak = 0;
                roundNumber = 0;
                lastWinner = "none";
                nextDecision = StrategyDecision{};
                nextDecision.type = StrategyType::HoldCommonAngle;
                nextDecision.params.hold_position = "mid_box";
                nextDecision.reason = "Restart: no data yet.";
                nextDecision.source = "default";
                state = State::Intro;
            }
            break;
        default:
            break;
    }

    if (IsKeyPressed(KEY_X)) xray = !xray;

    if (IsKeyPressed(KEY_L) && config.llm_enabled)
        llmReachable = llm->serverReachable();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void Game::drawEntity(const Entity& e, Color c, bool drawFov) {
    if (!e.alive) {
        DrawCircleLines((int)e.pos.x, (int)e.pos.y, e.radius, Fade(c, 0.5f));
        DrawText("X", (int)e.pos.x - 4, (int)e.pos.y - 6, 14, Fade(c, 0.6f));
        return;
    }
    if (drawFov) {
        float fov = botCtrl.aim.fovHalf;
        Vector2 l = e.pos + vFromAngle(e.facing - fov) * 220.0f;
        Vector2 r = e.pos + vFromAngle(e.facing + fov) * 220.0f;
        DrawTriangle(e.pos, r, l, Fade(c, 0.08f));
        DrawLineV(e.pos, l, Fade(c, 0.18f));
        DrawLineV(e.pos, r, Fade(c, 0.18f));
    }
    DrawCircleV(e.pos, e.radius, c);
    Vector2 nose = e.pos + vFromAngle(e.facing) * (e.radius + 8);
    DrawLineEx(e.pos, nose, 3.0f, WHITE);

    // Health bar
    float w = 36.0f;
    float hp = e.health / 100.0f;
    DrawRectangle((int)(e.pos.x - w / 2), (int)(e.pos.y - e.radius - 12), (int)w, 5, DARKGRAY);
    DrawRectangle((int)(e.pos.x - w / 2), (int)(e.pos.y - e.radius - 12), (int)(w * hp), 5,
                  hp > 0.5f ? GREEN : (hp > 0.25f ? YELLOW : RED));
}

void Game::drawWorld() {
    DrawRectangle(0, 0, PANEL_X, SCREEN_H, (Color){24, 26, 32, 255});
    DrawRectangleLinesEx({0, 0, (float)PANEL_X, (float)SCREEN_H}, 2, (Color){60, 64, 74, 255});

    // Crates (cover) drawn distinct from solid walls.
    for (auto& w : map.walls) {
        bool isCrate = false;
        for (auto& c : map.crates)
            if (c.x == w.x && c.y == w.y) { isCrate = true; break; }
        Color col = isCrate ? (Color){120, 95, 60, 255} : (Color){70, 75, 88, 255};
        DrawRectangleRec(w, col);
        DrawRectangleLinesEx(w, 1, (Color){40, 44, 52, 255});
    }

    // Named positions (faint).
    for (auto& kv : map.named) {
        DrawCircle((int)kv.second.x, (int)kv.second.y, 2.5f, Fade(GRAY, 0.5f));
    }

    // Where the bot has LEARNED the player tends to attack from (it pre-aims here).
    if (playerModel.engageSamples >= 2) {
        Vector2 e = {playerModel.favEngageX, playerModel.favEngageY};
        DrawCircleLines((int)e.x, (int)e.y, 16, Fade(YELLOW, 0.7f));
        DrawCircleLines((int)e.x, (int)e.y, 11, Fade(YELLOW, 0.4f));
        DrawText("bot expects you", (int)e.x - 40, (int)e.y + 18, 10, Fade(YELLOW, 0.7f));
    }

    // Tracers (gunfire is always visible/audible)
    for (auto& t : tracers)
        DrawLineEx(t.a, t.b, 2.0f, t.color);

    // Fog of war: the player only sees the bot with line of sight (no wallhack),
    // unless X-ray is toggled on.
    botVisibleNow = xray || (bot.alive && map.hasLineOfSight(player.pos, bot.pos)) ||
                    !bot.alive;
    if (botVisibleNow) {
        drawEntity(bot, xray && !(map.hasLineOfSight(player.pos, bot.pos)) ?
                            (Color){200, 80, 120, 255} : RED, true);
    } else {
        // Hint where the bot was last seen by the PLAYER (fades out).
        // (We simply don't draw the bot; the player must infer its position.)
    }
    drawEntity(player, SKYBLUE, false);

    // X-ray indicator overlay.
    if (xray) {
        DrawText("X-RAY ON", 10, SCREEN_H - 26, 18, (Color){255, 120, 160, 255});
    } else if (state == State::Playing && !botVisibleNow) {
        DrawText("bot not in sight", 10, SCREEN_H - 26, 16, (Color){140, 145, 155, 255});
    }
}

static int drawWrapped(const std::string& text, int x, int y, int width, int fontSize,
                       Color color, int maxLines = 1000) {
    std::string line;
    int lines = 0;
    auto flush = [&]() {
        DrawText(line.c_str(), x, y, fontSize, color);
        y += fontSize + 2;
        line.clear();
        lines++;
    };
    std::string word;
    auto pushWord = [&](const std::string& w) {
        std::string trial = line.empty() ? w : line + " " + w;
        if (MeasureText(trial.c_str(), fontSize) > width && !line.empty()) {
            flush();
            line = w;
        } else {
            line = trial;
        }
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        char ch = (i < text.size()) ? text[i] : ' ';
        if (ch == '\n') {
            pushWord(word); word.clear();
            if (lines >= maxLines) return y;
            flush();
        } else if (ch == ' ' || ch == '\t') {
            if (!word.empty()) { pushWord(word); word.clear(); }
            if (lines >= maxLines) return y;
        } else {
            word.push_back(ch);
        }
    }
    if (!line.empty()) flush();
    return y;
}

static void drawBar(const char* label, float v, int x, int y, int w, Color c) {
    DrawText(label, x, y, 11, (Color){170, 175, 185, 255});
    int bx = x + 130;
    int bw = w - 130;
    DrawRectangle(bx, y, bw, 10, (Color){45, 48, 56, 255});
    DrawRectangle(bx, y, (int)(bw * clampf(v, 0, 1)), 10, c);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", v);
    DrawText(buf, bx + bw + 4, y, 11, (Color){150, 155, 165, 255});
}

void Game::drawDebugPanel() {
    int x = PANEL_X + 10;
    int w = PANEL_W - 20;
    int y = 8;
    DrawRectangle(PANEL_X, 0, PANEL_W, SCREEN_H, (Color){16, 17, 22, 255});

    Color hdr = (Color){120, 200, 255, 255};
    DrawText("ADAPTIVE BOT - DEBUG", x, y, 16, hdr); y += 22;

    char buf[256];
    snprintf(buf, sizeof(buf), "Round %d   Win streak %d / %d",
             roundNumber + (state == State::Intro ? 1 : 0), winStreak, config.win_streak_goal);
    DrawText(buf, x, y, 13, WHITE); y += 18;

    const char* st = state == State::Intro ? "INTRO (press SPACE)"
                   : state == State::Playing ? "PLAYING"
                   : state == State::Analyzing ? "ANALYZING..."
                   : state == State::Victory ? "VICTORY" : "RESULT";
    snprintf(buf, sizeof(buf), "State: %s", st);
    DrawText(buf, x, y, 12, (Color){200, 220, 160, 255}); y += 16;

    const char* llmState = !config.llm_enabled ? "disabled (mock analyst)"
                         : llmReachable ? "enabled + reachable" : "enabled but UNREACHABLE";
    snprintf(buf, sizeof(buf), "LLM: %s", llmState);
    DrawText(buf, x, y, 12, (Color){200, 180, 140, 255}); y += 16;
    snprintf(buf, sizeof(buf), "Last decision source: %s", analysisSource.c_str());
    DrawText(buf, x, y, 12, (Color){200, 180, 140, 255}); y += 14;
    snprintf(buf, sizeof(buf), "LLM trust: %d%%", (int)std::round(llmTrust * 100));
    DrawText(buf, x, y, 12, (Color){200, 180, 140, 255}); y += 14;
    y = drawWrapped("LLM " + lastPrediction, x, y, w, 10, (Color){180, 190, 220, 255}, 3);
    y += 4;

    DrawLine(x, y, x + w, y, (Color){50, 54, 62, 255}); y += 8;

    // ---- Current bot strategy ----
    DrawText("CURRENT BOT STRATEGY", x, y, 13, hdr); y += 18;
    snprintf(buf, sizeof(buf), "%s  [%s]", strategyName(currentDecision.type),
             currentDecision.source.c_str());
    DrawText(buf, x, y, 13, (Color){255, 230, 150, 255}); y += 16;
    snprintf(buf, sizeof(buf), "watch=%s  hold=%s  phase=%s",
             routeKey(currentDecision.params.watch_route),
             currentDecision.params.hold_position.c_str(),
             botCtrl.phaseLabel());
    DrawText(buf, x, y, 11, WHITE); y += 14;
    snprintf(buf, sizeof(buf), "aggr=%.2f risk=%.2f scan=%.2f rotate=%.1fs conf=%.2f",
             currentDecision.params.aggression, currentDecision.params.risk,
             currentDecision.params.scan, currentDecision.params.rotate_after_seconds,
             currentDecision.confidence);
    DrawText(buf, x, y, 11, WHITE); y += 14;
    y = drawWrapped("Reason: " + currentDecision.reason, x, y, w, 11,
                    (Color){180, 200, 180, 255}, 3); y += 2;
    snprintf(buf, sizeof(buf), "Tuner: flankPressure=%.2f", tuner.flankPressure());
    DrawText(buf, x, y, 11, (Color){230, 170, 120, 255}); y += 13;
    y = drawWrapped(lastTuneNotes, x, y, w, 10, (Color){200, 160, 120, 255}, 3); y += 4;

    DrawText("REHEARSAL (sim win-rates)", x, y, 12, hdr); y += 15;
    y = drawWrapped(lastRehearsal, x, y, w, 10, (Color){150, 200, 200, 255}, 7); y += 4;

    DrawLine(x, y, x + w, y, (Color){50, 54, 62, 255}); y += 8;

    // ---- Player model ----
    DrawText("PLAYER MODEL", x, y, 13, hdr); y += 18;
    const PlayerModel& m = playerModel;
    Color bc = (Color){90, 170, 230, 255};
    drawBar("prefersMid", m.prefersMid, x, y, w, bc); y += 14;
    drawBar("prefersLeft", m.prefersLeftFlank, x, y, w, bc); y += 14;
    drawBar("prefersRight", m.prefersRightFlank, x, y, w, bc); y += 14;
    drawBar("routeRepeat", m.routeRepeatability, x, y, w, (Color){230, 180, 90, 255}); y += 14;
    drawBar("fastRush", m.fastRushTendency, x, y, w, (Color){230, 130, 90, 255}); y += 14;
    drawBar("slowClear", m.slowClearTendency, x, y, w, (Color){150, 150, 200, 255}); y += 14;
    drawBar("camping", m.campingTendency, x, y, w, (Color){150, 150, 200, 255}); y += 14;
    drawBar("widePeek", m.widePeekTendency, x, y, w, (Color){200, 120, 200, 255}); y += 14;
    drawBar("pushAfter", m.pushAfterContactTendency, x, y, w, (Color){120, 200, 120, 255}); y += 14;
    drawBar("retreatAfter", m.retreatAfterContactTendency, x, y, w, (Color){120, 200, 120, 255}); y += 14;
    drawBar("aggression", m.aggression, x, y, w, (Color){230, 90, 90, 255}); y += 14;
    drawBar("confidence", m.confidence, x, y, w, (Color){90, 230, 160, 255}); y += 14;
    snprintf(buf, sizeof(buf), "avgFirstContact: %.1fs", m.avgFirstContactTime);
    DrawText(buf, x, y, 11, (Color){170, 175, 185, 255}); y += 13;
    if (m.engageSamples >= 2)
        snprintf(buf, sizeof(buf), "learned engage spot: (%.0f,%.0f) x%d  -> bot pre-aims it",
                 m.favEngageX, m.favEngageY, m.engageSamples);
    else
        snprintf(buf, sizeof(buf), "learning your engage spot... (%d/2)", m.engageSamples);
    DrawText(buf, x, y, 11, (Color){230, 220, 120, 255}); y += 16;

    DrawLine(x, y, x + w, y, (Color){50, 54, 62, 255}); y += 8;

    // ---- Detected patterns ----
    DrawText("DETECTED PATTERNS", x, y, 13, hdr); y += 18;
    if (lastSummary.observed_patterns.empty()) {
        DrawText("(none yet)", x, y, 11, GRAY); y += 14;
    } else {
        for (auto& p : lastSummary.observed_patterns) {
            y = drawWrapped("- " + p, x, y, w, 11, (Color){200, 205, 210, 255}, 2);
        }
    }
    y += 4;

    // ---- LLM / analysis output ----
    DrawLine(x, y, x + w, y, (Color){50, 54, 62, 255}); y += 8;
    DrawText("LAST ANALYSIS (raw)", x, y, 13, hdr); y += 18;
    if (!llmError.empty()) {
        y = drawWrapped(llmError, x, y, w, 11, (Color){240, 130, 120, 255}, 3);
    }
    int remaining = (SCREEN_H - 30 - y);
    int maxLines = std::max(2, remaining / 13);
    y = drawWrapped(llmRaw, x, y, w, 10, (Color){150, 200, 150, 255}, maxLines);

    // Controls footer
    DrawText("WASD move | Mouse aim | LMB shoot | SPACE start",
             x, SCREEN_H - 26, 10, (Color){120, 125, 135, 255});
    DrawText("X x-ray bot | L recheck LLM | Esc quit",
             x, SCREEN_H - 14, 10, (Color){120, 125, 135, 255});
}

void Game::draw() {
    BeginTextureMode(renderTarget);
    ClearBackground((Color){12, 13, 16, 255});

    drawWorld();
    drawDebugPanel();

    // Overlays
    if (state == State::Intro) {
        const char* t1 = TextFormat("ROUND %d", roundNumber + 1);
        const char* t2 = lastWinner == "player" ? "You won the last round! Keep the streak."
                       : lastWinner == "bot" ? "Bot won. Streak reset - it adapted to you."
                       : lastWinner == "timeout" ? "Timeout - no kill. Replaying."
                       : "Win 10 rounds in a row. The bot learns you each round.";
        int tw = MeasureText(t1, 40);
        DrawRectangle(0, SCREEN_H / 2 - 70, PANEL_X, 150, Fade(BLACK, 0.65f));
        DrawText(t1, PANEL_X / 2 - tw / 2, SCREEN_H / 2 - 55, 40, WHITE);
        int tw2 = MeasureText(t2, 18);
        DrawText(t2, PANEL_X / 2 - tw2 / 2, SCREEN_H / 2 - 5, 18, (Color){210, 210, 160, 255});
        const char* t3 = TextFormat("Bot will use: %s", strategyName(nextDecision.type));
        int tw3 = MeasureText(t3, 18);
        DrawText(t3, PANEL_X / 2 - tw3 / 2, SCREEN_H / 2 + 22, 18, (Color){255, 200, 120, 255});
        const char* t4 = "Press SPACE to start";
        int tw4 = MeasureText(t4, 18);
        DrawText(t4, PANEL_X / 2 - tw4 / 2, SCREEN_H / 2 + 48, 18, (Color){150, 220, 255, 255});
    } else if (state == State::Analyzing) {
        const char* t1 = "ANALYZING ROUND...";
        int tw = MeasureText(t1, 30);
        DrawRectangle(0, SCREEN_H / 2 - 30, PANEL_X, 60, Fade(BLACK, 0.6f));
        DrawText(t1, PANEL_X / 2 - tw / 2, SCREEN_H / 2 - 15, 30, (Color){150, 220, 255, 255});
    } else if (state == State::Victory) {
        DrawRectangle(0, 0, PANEL_X, SCREEN_H, Fade(BLACK, 0.75f));
        const char* t1 = "VICTORY - 10 ROUND STREAK!";
        int tw = MeasureText(t1, 36);
        DrawText(t1, PANEL_X / 2 - tw / 2, SCREEN_H / 2 - 40, 36, GOLD);
        const char* t2 = "You out-adapted the bot. Press R to play again.";
        int tw2 = MeasureText(t2, 18);
        DrawText(t2, PANEL_X / 2 - tw2 / 2, SCREEN_H / 2 + 10, 18, WHITE);
    }

    if (state == State::Playing) {
        DrawText(TextFormat("Time %.1f / %.0f", roundTime, roundTimeLimit), 10, 10, 18, WHITE);
    }

    EndTextureMode();

    // Letterbox-scale the virtual frame to the actual (resizable) window.
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    float scale = std::min((float)sw / SCREEN_W, (float)sh / SCREEN_H);
    float dw = SCREEN_W * scale;
    float dh = SCREEN_H * scale;
    float ox = (sw - dw) * 0.5f;
    float oy = (sh - dh) * 0.5f;

    BeginDrawing();
    ClearBackground(BLACK);
    Rectangle src = {0, 0, (float)SCREEN_W, -(float)SCREEN_H}; // flip Y
    Rectangle dst = {ox, oy, dw, dh};
    DrawTexturePro(renderTarget.texture, src, dst, {0, 0}, 0.0f, WHITE);
    EndDrawing();
}

void Game::run() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Adaptive 1v1 Bot Prototype");
    SetWindowMinSize(800, 450);
    SetTargetFPS(60);

    renderTarget = LoadRenderTexture(SCREEN_W, SCREEN_H);
    SetTextureFilter(renderTarget.texture, TEXTURE_FILTER_BILINEAR);

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f; // clamp big hitches

        // Map the OS cursor into virtual (1280x720) coordinates.
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        float scale = std::min((float)sw / SCREEN_W, (float)sh / SCREEN_H);
        float ox = (sw - SCREEN_W * scale) * 0.5f;
        float oy = (sh - SCREEN_H * scale) * 0.5f;
        Vector2 mp = GetMousePosition();
        virtualMouse = {clampf((mp.x - ox) / scale, 0, (float)SCREEN_W),
                        clampf((mp.y - oy) / scale, 0, (float)SCREEN_H)};

        update(dt);
        draw();
    }

    UnloadRenderTexture(renderTarget);
    Log::shutdown();
    CloseWindow();
}
