#include "Config.h"
#include "json.hpp"
#include <fstream>

using json = nlohmann::json;

Config Config::load(const std::string& path) {
    Config c;
    std::ifstream f(path);
    if (!f.is_open()) return c;
    try {
        json j;
        f >> j;
        c.llm_enabled = j.value("llm_enabled", c.llm_enabled);
        c.llm_autostart_server = j.value("llm_autostart_server", c.llm_autostart_server);
        c.llm_autodownload = j.value("llm_autodownload", c.llm_autodownload);
        c.llm_server_path = j.value("llm_server_path", c.llm_server_path);
        c.model_path = j.value("model_path", c.model_path);
        c.server_url = j.value("server_url", c.server_url);
        c.llm_timeout_seconds = j.value("llm_timeout_seconds", c.llm_timeout_seconds);
        c.win_streak_goal = j.value("win_streak_goal", c.win_streak_goal);
        c.round_time_limit = j.value("round_time_limit", c.round_time_limit);
    } catch (...) {
        // Keep defaults on malformed config.
    }
    return c;
}
