# Adaptive 1v1 Bot Prototype

A small, top-down 1v1 tactical-duel prototype where you must **win 10 rounds in a
row** against a bot that **analyzes your behavior between rounds and adapts its
strategy**. The bot does not cheat (no wallhack, no growing aim accuracy, no
shrinking reaction time) — it gets *more specific* about you: your routes, your
timing, your peek style, and whether you repeat or bait.

This is an **AI-behavior prototype**, not a full shooter.

---

## Core idea

* During a round the bot uses only **deterministic in-round AI** with a **fair,
  fixed aim model**. It only "sees" you through line-of-sight inside its field
  of view (so it can be flanked) plus nearby sound cues.
* Between rounds a **post-round analyst** (rule-based by default, optionally a
  **local LLM**) reads a compact round summary, refines a persistent
  **player model**, updates a **confidence** value, and picks **one strategy**
  from a fixed library, tuning its parameters.
* Learning is **gradual**: a habit seen once gives low confidence; if you break
  a predicted pattern, the bot's confidence drops — so you can **bait it**.

---

## Build

Requirements (already verified on this machine): **CMake ≥ 3.20**, a **C++20**
compiler (GCC/MinGW), and **Ninja**. Dependencies (raylib, nlohmann/json,
cpp-httplib) are **vendored** in `third_party/` — no network needed to build.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the game:

```powershell
cd build\bin
.\LLMBotFPS.exe
```

Run the headless adaptation self-test (no window, no LLM):

```powershell
.\build\bin\sim_test.exe
```

---

## Controls

| Key | Action |
|-----|--------|
| `WASD` | Move |
| Mouse | Aim |
| Left mouse | Shoot |
| `SPACE` | Start the next round (from the intro screen) |
| `X` | Toggle **X-ray** (see the bot through walls — a debug cheat) |
| `L` | Re-check whether the LLM server is reachable |
| `Esc` | Quit |

**Fog of war:** by default you only see the bot when you have line of sight to
it (no wallhack). When it's hidden you'll see a "bot not in sight" hint. Press
`X` to toggle X-ray and reveal it through walls (shown tinted pink when seen only
via X-ray).

**In-round adaptation:** the bot now also changes tactics *during* a round
(deterministically, no LLM): if it gets no contact it stops camping and patrols
other lanes (`reposition`/`rotate`); if it's shot from a direction it wasn't
watching it turns to investigate or pushes to clear that angle (`investigate`).
It only scans the plausible threat arc (last-seen / shot-from direction / watched
lane) — it no longer spins to check its back for no reason. The current in-round
phase is shown in the debug panel next to the strategy.

The window is **resizable** (drag any edge / maximize). The view is rendered at
a fixed 1280×720 virtual resolution and letterbox-scaled to the window, so the
layout and aiming stay correct at any size.

The right-hand **debug panel** shows the current bot strategy, the live player
model (as bars), detected patterns, confidence, and the last raw analysis /
LLM response — so you can see *why* the bot changed behavior.

### Logs

Every session writes a detailed, timestamped log to **`llmbotfps_log.txt`** next
to the executable (`build/bin/llmbotfps_log.txt`). It records, per round:

* the strategy + parameters the bot applied,
* the round result, duration and win streak,
* the full round summary JSON,
* the rule-based candidate decision,
* (when the LLM is on) the prompt sent, the raw LLM response, validation result
  or error, and the final decision used.

---

## How it maps to the design

| Pillar | Where |
|--------|-------|
| Map: 3 routes, cover, named positions, line-of-sight | `src/Map.{h,cpp}` |
| Telemetry recorder | `src/Telemetry.h` |
| Player model (persistent) | `src/PlayerModel.h` |
| Strategy library + params | `src/Strategy.h` |
| In-round deterministic AI + fair aim + view scanning | `src/BotController.{h,cpp}`, `src/AimModel.h` |
| Round summary, pattern detection, gradual model update, rule-based counter | `src/Analyst.{h,cpp}` |
| Self-tuning of strategy parameters from outcomes | `src/StrategyTuner.{h,cpp}` |
| LLM sidecar launch + HTTP + grammar-constrained JSON + fallback | `src/LLMService.{h,cpp}` |
| Game loop, combat, debug UI, win streak | `src/Game.{h,cpp}` |

The strategy library (the LLM may only choose from this list):
`HoldCommonAngle, HoldOffAngle, FastRush, SlowClear, DelayedPush, FakeSoundBait,
AntiFlankTrap, RotateAfterNoContact, DefensiveHold, AggressivePush,
RetreatAndRepeek, MirrorPlayerRoute`.

---

## Self-tuning parameters (`StrategyTuner`)

On top of choosing *which* strategy to use, the system learns *how to tune* a
strategy's parameters from what actually happened:

* It records, per strategy, wins/losses and **how the bot died** — `flanked`
  (shot from outside its limited field of view) vs `duel` (lost a straight
  fight).
* If the bot keeps getting **flanked**, the tuner raises `scan` (how much the
  bot sweeps its view / checks its back) and shortens `rotate_after_seconds`
  (re-checks angles sooner) — e.g. an AggressivePush that worked offensively but
  died to a flank gets the same aggression *plus* more looking around.
* If it keeps losing **head-on duels**, it nudges `risk` up (take safer
  off-angles) and raises `scan`.
* It remembers the **parameter profile that won** with a strategy and biases
  back toward it.

The bot's field of view is intentionally limited (~120°), so it really can be
flanked — which makes the scan/rotate tuning meaningful. All of this is shown
live in the debug panel (`flankPressure`, `scan`, and the tuner notes) and in
the log.

## Self-play rehearsal (`Rollout`)

Between rounds the bot also runs a fast **headless "mental rehearsal"**: it builds
a behavior profile of *this* player (preferred route, first-contact timing, the
learned engage spot, aggression) and simulates the upcoming duel many times for
**every** candidate strategy, reusing the **real `BotController`** so it tests the
bot's actual behavior. It measures the bot's win-rate per strategy.

The pipeline is a hybrid where the **LLM is the strategist** and the **simulator
is the judge**:

1. **LLM (strategist)** predicts what the player will do *next* round (route,
   whether they're likely baiting/flanking) and proposes a few candidate
   counter-strategies with parameters. This is the meta-reasoning the simulator
   can't do (anticipating a switch, not just countering the last move).
2. **Rollout (judge)** simulates the LLM's candidates *and* the baseline
   strategies against the **anticipated** player profile (plus the historical
   one), reusing the real `BotController`, and measures win-rates. The best wins.
   This grounds the LLM so a weak model can't pick something that loses.
3. **Tuner** applies learned safety adjustments on top (scan/rotate/strategy
   switch).

**Self-correcting trust:** each round the bot checks whether the LLM's route
prediction came true and updates an `llmTrust` value (EMA). The more accurate the
LLM, the more its anticipated profile is weighted in the rollout; if it keeps
mispredicting, the system leans on the historical data instead. Without an LLM,
the rollout simply uses the historical profile. The prediction, trust, and
rehearsal win-rates (with `[LLM]` tags for LLM-proposed candidates) are shown in
the debug panel and the log.

## LLM integration

By default `config.json` has `"llm_enabled": true` with `llm_autostart_server`.
The game **runs fine without any model**: if the server/model is missing or
unreachable, or returns invalid JSON, it **logs the error and falls back** to
the built-in rule-based analyst — the duel never breaks.

### The LLM is NOT committed to the repo — it auto-downloads

The `llm/` runtime (server binary + GGUF model, ~1 GB) is **not** stored in git.
On first launch, if `llm_enabled` and the model is missing, the game **spawns the
bundled `fetch_llm.ps1` in a console window** to download it automatically (the
game meanwhile runs on its self-play AI, and starts using the LLM as soon as the
download finishes). Set `"llm_autodownload": false` to disable this.

You can also run it manually (from the repo root or next to the exe):

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_llm.ps1 -Dest build\bin\llm
```

This fetches the latest `llama-server` (win-cpu-x64) and
`Qwen2.5-1.5B-Instruct` (GGUF, ~1 GB). Swap `-ModelUrl` for a different model
(e.g. a 0.5B for faster, weaker responses). Layout:

```
build/bin/
  LLMBotFPS.exe
  config.json
  llm/
    llama-server.exe   (+ ggml/llama DLLs)
    model.gguf
```

With `llm_autostart_server: true` the game launches the server itself and talks
to it over localhost HTTP. The reachability is re-probed between rounds, so an
auto-started server that is still loading the model is picked up automatically.

### Strict JSON via grammar constraint

The request sends a **JSON schema** (`Analyst::buildResponseSchema`) as
`response_format`, so `llama-server` grammar-constrains the output — even a tiny
0.5B model returns structurally valid JSON with `chosen_strategy` restricted to
the allowed enum. The game still independently validates the response
(`Analyst::parseLLMResponse`) and clamps every parameter. The LLM can only:
choose one allowed strategy, set known parameters (incl. `scan`), update
confidence and give a short reason — it can never set perfect aim, lower
reaction time, or control the bot frame-by-frame.

### End-to-end LLM check

```powershell
cd build\bin
.\LLMBotFPS.exe --llmtest
```

This starts the sidecar, waits until it's reachable, sends a sample round
summary, and prints the raw response + the validated strategy.

---

## Fairness notes

`AimModel` (in `src/AimModel.h`) holds the **fixed** difficulty ceiling:
reaction time, aim spread, turn speed, fire rate, damage and range. These are
**not** tunable by the analyst or the LLM. Between rounds only *positioning,
timing, route prediction, off-angles, baiting and strategy choice* change.
