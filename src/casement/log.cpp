#include "casement/log.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <mutex>

namespace casement::log {

namespace {

std::atomic<int> current{static_cast<int>(Level::info)};
std::mutex write_lock;

const char* name(Level level) {
    switch (level) {
        case Level::error: return "ERROR";
        case Level::warn: return "WARN";
        case Level::info: return "INFO";
        case Level::debug: return "DEBUG";
        case Level::trace: return "TRACE";
    }
    return "?";
}

}  // namespace

void init(const char* variable) {
    char v[32];
    DWORD n = GetEnvironmentVariableA(variable, v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return;
    std::string s(v, n);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "error") current = static_cast<int>(Level::error);
    else if (s == "warn") current = static_cast<int>(Level::warn);
    else if (s == "info") current = static_cast<int>(Level::info);
    else if (s == "debug") current = static_cast<int>(Level::debug);
    else if (s == "trace") current = static_cast<int>(Level::trace);
}

bool enabled(Level level) { return static_cast<int>(level) <= current.load(); }

void write(Level level, std::string_view text) {
    std::string line = std::string(name(level)) + " " + std::string(text) + "\n";
    std::lock_guard g(write_lock);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == nullptr || err == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

}  // namespace casement::log
