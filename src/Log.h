#pragma once
#include <string>

// Thread-safe session logger. Writes timestamped lines to a file (and stdout).
// Used to record every round, the analysis, the chosen strategy, the player
// model and raw LLM responses so the bot's behavior is fully traceable.
namespace Log {
    void init(const std::string& path);
    void write(const std::string& line);
    void section(const std::string& title);
    void shutdown();
}
