#include "Log.h"
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace {
std::ofstream g_file;
std::mutex g_mutex;
std::chrono::steady_clock::time_point g_start;

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - g_start).count();
    std::ostringstream os;
    os << std::put_time(&tm, "%H:%M:%S") << " (+" << (elapsed / 1000.0) << "s)";
    return os.str();
}
}

namespace Log {

void init(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_start = std::chrono::steady_clock::now();
    g_file.open(path, std::ios::out | std::ios::trunc);
    if (g_file.is_open()) {
        g_file << "==== Adaptive Bot session log ====\n";
        g_file.flush();
    }
}

void write(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string out = "[" + timestamp() + "] " + line;
    if (g_file.is_open()) { g_file << out << "\n"; g_file.flush(); }
    std::printf("%s\n", out.c_str());
    std::fflush(stdout);
}

void section(const std::string& title) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string bar(60, '-');
    if (g_file.is_open()) {
        g_file << "\n" << bar << "\n" << title << "\n" << bar << "\n";
        g_file.flush();
    }
    std::printf("\n%s\n%s\n%s\n", bar.c_str(), title.c_str(), bar.c_str());
    std::fflush(stdout);
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file.is_open()) { g_file << "==== session end ====\n"; g_file.close(); }
}

}
