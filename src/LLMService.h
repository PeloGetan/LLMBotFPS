#pragma once
#include "Config.h"
#include <string>

// Manages the optional local LLM sidecar process and HTTP communication with it.
// The game must run fine without a model (mock/fallback handled by the caller).
class LLMService {
public:
    explicit LLMService(const Config& cfg) : config(cfg) {}
    ~LLMService();

    // Launch the bundled llama-server if configured. Safe to call repeatedly;
    // it will only start once and only when the server/model files exist.
    void startServerIfConfigured();

    // True when both the server binary and the model file are present.
    bool modelPresent() const;

    // If the model is missing, spawn the bundled fetch script (visible console)
    // to download the server + model. Best-effort, non-blocking, runs once.
    void tryAutoDownload();
    bool downloadStarted() const { return downloadSpawned; }

    // Quick reachability probe.
    bool serverReachable();

    // Send a chat-completion request. Returns true and fills outContent on
    // success; false with an error message otherwise. If jsonSchema is non-empty
    // it is sent as a response_format json_schema (grammar-constrained output);
    // otherwise a plain json_object format is requested.
    bool chat(const std::string& systemPrompt, const std::string& userPrompt,
              std::string& outContent, std::string& error,
              const std::string& jsonSchema = "");

    bool enabled() const { return config.llm_enabled; }
    bool serverStarted() const { return serverProcessStarted; }

private:
    Config config;
    bool serverProcessStarted = false;
    bool downloadSpawned = false;
    void* processHandle = nullptr; // HANDLE on Windows

    struct Url { std::string scheme, host; int port; std::string path; };
    static bool parseUrl(const std::string& url, Url& out);
};
