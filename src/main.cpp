#include "Game.h"
#include "Config.h"
#include "LLMService.h"
#include "Analyst.h"
#include "Map.h"
#include "Log.h"
#include <cstring>
#include <cstdio>
#include <thread>
#include <chrono>

// End-to-end check of the REAL local LLM: starts the sidecar (if configured),
// waits until it is reachable, sends a sample round summary, prints the raw
// response and the validated strategy. Run with:  LLMBotFPS.exe --llmtest
static int llmTest() {
    Log::init("llmtest_log.txt");
    Config cfg = Config::load("config.json");
    cfg.llm_enabled = true; // force on for this manual test

    LLMService llm(cfg);
    printf("Starting LLM server (if configured)...\n");
    llm.startServerIfConfigured();

    bool ok = false;
    for (int i = 0; i < 90; ++i) {
        if (llm.serverReachable()) { ok = true; break; }
        printf("  waiting for server to come up... %ds\n", i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!ok) {
        printf("ERROR: LLM server not reachable at %s\n", cfg.server_url.c_str());
        return 2;
    }
    printf("Server reachable. Sending sample round summary...\n");

    Map map;
    Analyst an;
    RoundSummary s;
    s.round_number = 4;
    s.winner = "player";
    s.duration_seconds = 12.3f;
    s.player_route = "mid";
    s.bot_strategy = "HoldCommonAngle";
    s.first_contact_time = 6.5f;
    s.first_contact_location = "mid_box";
    s.peek_style = "wide";
    s.contact_reaction = "push";
    s.bot_was_flanked = true;
    s.bot_death_cause = "flanked";
    s.observed_patterns = {"Player used mid route in 3 of last 4 rounds",
                           "Player tends to wide-peek corners",
                           "Bot was flanked / shot from outside its view"};
    PlayerModel m;
    m.prefersMid = 0.78f;
    m.fastRushTendency = 0.64f;
    m.widePeekTendency = 0.69f;
    m.routeRepeatability = 0.72f;
    m.confidence = 0.7f;
    s.model_snapshot = m;

    std::string sys = an.buildSystemPrompt();
    std::string usr = an.buildUserPrompt(s, m);
    std::string content, err;
    bool got = llm.chat(sys, usr, content, err, an.buildResponseSchema());

    printf("\n--- chat ok=%d err=%s ---\nRAW RESPONSE:\n%s\n", got, err.c_str(),
           content.c_str());
    Log::write(std::string("RAW:\n") + content + "\nERR:" + err);

    if (got) {
        Analyst::LLMPlan plan;
        std::string perr;
        bool valid = an.parseLLMPlan(content, map, plan, perr);
        printf("\n--- parsed valid=%d perr=%s ---\n", valid, perr.c_str());
        if (valid) {
            printf("predicted_next: route=%s will_bait=%d expected_contact=%.1f\n",
                   routeKey(plan.predicted.route), (int)plan.predicted.willBait,
                   plan.predicted.expectedContact);
            printf("pattern=%s confidence=%.2f\nreason=%s\n", plan.player_pattern.c_str(),
                   plan.confidence, plan.reason.c_str());
            printf("candidates (%d):\n", (int)plan.candidates.size());
            for (auto& c : plan.candidates)
                printf("  - %s watch=%s hold=%s aggr=%.2f scan=%.2f\n",
                       strategyName(c.type), routeKey(c.params.watch_route),
                       c.params.hold_position.c_str(), c.params.aggression, c.params.scan);
        }
        return valid ? 0 : 1;
    }
    return 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--llmtest") == 0)
        return llmTest();
    Game game;
    game.run();
    return 0;
}
