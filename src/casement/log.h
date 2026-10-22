// Logging to stderr, which Casement copies into its log, one line per
// message. The level comes from an environment variable named after the
// plugin (for example CASEMENT_MESHCENTRAL_LOG=debug); the default is info.

#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace casement::log {

enum class Level { error, warn, info, debug, trace };

/// Reads the level from the environment variable `variable`.
void init(const char* variable);

bool enabled(Level level);

/// Writes one line to stderr.
void write(Level level, std::string_view text);

template <class... Args>
void error(std::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::error)) write(Level::error, std::format(f, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::warn)) write(Level::warn, std::format(f, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::info)) write(Level::info, std::format(f, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::debug)) write(Level::debug, std::format(f, std::forward<Args>(args)...));
}
template <class... Args>
void trace(std::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::trace)) write(Level::trace, std::format(f, std::forward<Args>(args)...));
}

}  // namespace casement::log
