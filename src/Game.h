#pragma once
#include "Config.h"
#include "Map.h"
#include "Entity.h"
#include "BotController.h"
#include "Telemetry.h"
#include "Analyst.h"
#include "PlayerModel.h"
#include "LLMService.h"
#include "Strategy.h"
#include "StrategyTuner.h"
#include "Projectile.h"
#include <memory>
#include <future>
#include <string>
#include <vector>

struct AnalysisResult {
    StrategyDecision decision;
    std::string raw;        // raw LLM content or mock JSON
    std::string error;      // validation/transport error (if any)
    std::string tuneNotes;  // parameter-tuner adjustments applied
    std::string rehearsal;  // self-play simulation win-rates summary
    bool fromLLM = false;
};

class Game {
public:
    Game();
    void run();

private:
    enum class State { Intro, Playing, Analyzing, Result, Victory };

    Config config;
    Map map;
    Entity player, bot;
    BotController botCtrl;
    Telemetry tel;
    Analyst analyst;
    PlayerModel playerModel;
    StrategyTuner tuner;
    std::unique_ptr<LLMService> llm;

    State state = State::Intro;
    int roundNumber = 0;
    int winStreak = 0;
    float roundTime = 0.0f;
    float roundTimeLimit = 30.0f;

    StrategyDecision currentDecision;  // applied this round
    StrategyDecision nextDecision;     // chosen for next round
    RoundSummary lastSummary;
    std::string lastWinner = "none";

    // Debug UI strings
    std::string llmRaw = "(no analysis yet)";
    std::string llmError;
    std::string validatedStrategyJson = "(default)";
    std::string analysisSource = "default";
    std::string lastTuneNotes = "(no tuning yet)";
    std::string lastRehearsal = "(no rehearsal yet)";
    std::string lastPrediction = "(no LLM prediction yet)";

    // Self-correcting trust in the LLM's predictions: rises when its forecast of
    // the player's next route is correct, falls when wrong. Weights how much the
    // anticipated profile influences the rollout.
    float llmTrust = 0.5f;
    Route lastLLMPredRoute = Route::Mid;
    bool lastLLMPredValid = false;

    // Flank tracking for the current round (was the bot hit from outside its view?).
    float botWorstHitAngle = 0.0f;
    bool botHitFromOutside = false;

    // Where the player was when it first damaged the bot this round (peek spot).
    Vector2 engagePos{0, 0};
    bool engageCaptured = false;

    // Player can only see the bot with line of sight, unless X-ray is toggled.
    bool xray = false;
    bool botVisibleNow = false;

    // Combat / fx
    std::vector<Projectile> projectiles;
    std::vector<char> pickupTaken;   // per map.weaponSpawns: consumed this round
    float botFiredFx = 0.0f;
    float playerFiredTimer = 0.0f; // for bot sound cue
    float sampleTimer = 0.0f;
    bool firstShotLogged = false;
    bool playerContactLogged = false;
    bool llmReachable = false;

    std::future<AnalysisResult> analysisFuture;
    bool analysisRunning = false;

    // Resizable-window support: render to a fixed virtual resolution, then
    // letterbox-scale it to the actual window.
    RenderTexture2D renderTarget{};
    Vector2 virtualMouse{0, 0};

    // First-person 3D rendering. Gameplay stays on the 2D ground plane; the
    // scene is rendered in 3D and the camera sits at the player's eyes.
    RenderTexture2D sceneTex{};
    Camera3D cam{};
    float camPitch = 0.0f;

    void startRound();
    void endRound(const std::string& winner);
    void beginAnalysis();
    void pollAnalysis();

    void update(float dt);
    void updatePlaying(float dt);
    void handlePlayerCombat(float dt);
    void handlePickups();

    void draw();
    void drawScene3D();
    void drawMinimap();
    void drawHud();
    void updateCamera();
    void drawDebugPanel();

    std::string decisionToJson(const StrategyDecision& d) const;
    std::string nearestName(Vector2 p) const;
    AnalysisResult runAnalysis(RoundSummary summary, PlayerModel model);
};
