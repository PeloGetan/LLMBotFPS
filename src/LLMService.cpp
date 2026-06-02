#include "httplib.h"
#include "LLMService.h"
#include "json.hpp"
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

bool LLMService::parseUrl(const std::string& url, Url& out) {
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    out.scheme = url.substr(0, schemeEnd);
    std::string rest = url.substr(schemeEnd + 3);
    auto slash = rest.find('/');
    std::string hostPort = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = hostPort.find(':');
    if (colon == std::string::npos) {
        out.host = hostPort;
        out.port = (out.scheme == "https") ? 443 : 80;
    } else {
        out.host = hostPort.substr(0, colon);
        out.port = std::stoi(hostPort.substr(colon + 1));
    }
    return !out.host.empty();
}

LLMService::~LLMService() {
#ifdef _WIN32
    if (processHandle) {
        TerminateProcess((HANDLE)processHandle, 0);
        CloseHandle((HANDLE)processHandle);
        processHandle = nullptr;
    }
#endif
}

bool LLMService::modelPresent() const {
    return std::filesystem::exists(config.model_path) &&
           std::filesystem::exists(config.llm_server_path);
}

void LLMService::tryAutoDownload() {
    if (downloadSpawned) return;
    if (!config.llm_enabled || !config.llm_autodownload) return;
    if (modelPresent()) return;
    if (!std::filesystem::exists("fetch_llm.ps1")) return; // shipped next to the exe

#ifdef _WIN32
    std::string cmd =
        "powershell -ExecutionPolicy Bypass -NoProfile -File fetch_llm.ps1 -Dest llm";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Visible console so the user can see the ~1 GB download progress.
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        downloadSpawned = true;
    }
#endif
}

void LLMService::startServerIfConfigured() {
    if (serverProcessStarted) return;
    if (!config.llm_enabled || !config.llm_autostart_server) return;
    if (!std::filesystem::exists(config.llm_server_path)) return;

    Url u;
    if (!parseUrl(config.server_url, u)) return;

#ifdef _WIN32
    std::ostringstream cmd;
    cmd << "\"" << config.llm_server_path << "\""
        << " -m \"" << config.model_path << "\""
        << " --host " << u.host
        << " --port " << u.port
        << " -c 4096";
    std::string cmdline = cmd.str();
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        processHandle = pi.hProcess;
        CloseHandle(pi.hThread);
        serverProcessStarted = true;
    }
#endif
}

bool LLMService::serverReachable() {
    Url u;
    if (!parseUrl(config.server_url, u)) return false;
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);
    auto res = cli.Get("/health");
    if (res && res->status == 200) return true;
    auto res2 = cli.Get("/v1/models");
    return res2 && res2->status == 200;
}

bool LLMService::chat(const std::string& systemPrompt, const std::string& userPrompt,
                      std::string& outContent, std::string& error,
                      const std::string& jsonSchema) {
    Url u;
    if (!parseUrl(config.server_url, u)) {
        error = "bad server_url";
        return false;
    }

    json payload = {
        {"model", "local"},
        {"temperature", 0.3},
        {"max_tokens", 512},
        {"stream", false},
        {"messages", json::array({
            {{"role", "system"}, {"content", systemPrompt}},
            {{"role", "user"}, {"content", userPrompt}}
        })}
    };

    // Constrain the output so even tiny models return well-formed JSON.
    if (!jsonSchema.empty()) {
        try {
            json schema = json::parse(jsonSchema);
            payload["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {{"name", "strategy"}, {"strict", true}, {"schema", schema}}}
            };
        } catch (...) {
            payload["response_format"] = {{"type", "json_object"}};
        }
    } else {
        payload["response_format"] = {{"type", "json_object"}};
    }

    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(config.llm_timeout_seconds, 0);
    cli.set_read_timeout(config.llm_timeout_seconds, 0);
    cli.set_write_timeout(config.llm_timeout_seconds, 0);

    auto res = cli.Post(u.path.c_str(), payload.dump(), "application/json");
    if (!res) {
        error = "no response from LLM server";
        return false;
    }
    if (res->status != 200) {
        error = "HTTP status " + std::to_string(res->status);
        return false;
    }
    try {
        json j = json::parse(res->body);
        if (j.contains("choices") && !j["choices"].empty()) {
            outContent = j["choices"][0]["message"]["content"].get<std::string>();
            return true;
        }
        error = "unexpected response shape";
    } catch (const std::exception& e) {
        error = std::string("parse error: ") + e.what();
    }
    return false;
}
