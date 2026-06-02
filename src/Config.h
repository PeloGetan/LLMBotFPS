#pragma once
#include <string>

struct Config {
    bool llm_enabled = false;
    bool llm_autostart_server = false;
    bool llm_autodownload = true; // fetch the bundled server+model if missing
    std::string llm_server_path = "llm/llama-server.exe";
    std::string model_path = "llm/model.gguf";
    std::string server_url = "http://127.0.0.1:48291/v1/chat/completions";
    int llm_timeout_seconds = 30;
    int win_streak_goal = 10;
    float round_time_limit = 30.0f;

    static Config load(const std::string& path);
};
