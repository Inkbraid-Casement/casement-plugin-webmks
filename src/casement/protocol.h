// Casement plugin protocol (docs/plugin-protocol.md): each message is a
// 4-byte little-endian length followed by that many bytes of UTF-8 JSON, one
// object with a "type" member. Unknown types and members are ignored.

#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace casement {

using json = nlohmann::json;

/// The largest message either side may send.
inline constexpr std::size_t max_message = std::size_t{16} << 20;

enum class State { connected, auth_failed, failed, closed };

std::string_view to_string(State state);

/// The protocol output stream. Writes are serialised, so messages from
/// different threads never interleave.
class Output {
public:
    explicit Output(HANDLE handle) : handle_(handle) {}

    /// Sends one message. If Casement has gone away there is nobody left to
    /// talk to, so a broken pipe ends the process.
    void send(const json& msg);

    /// Sends one message; false if the stream is broken.
    bool try_send(const json& msg);

private:
    HANDLE handle_;
    std::mutex lock_;
};

/// Writes one framed message body. False on a write error.
bool write_frame(HANDLE out, std::string_view body);

/// Reads one framed message body. Empty optional at the end of the stream
/// or when it breaks (with `error` set for the latter).
std::optional<std::string> read_frame(HANDLE in, std::string* error = nullptr);

/// Reads the next message that decodes as a JSON object, skipping others.
/// Empty optional at the end of the stream.
std::optional<json> read_message(HANDLE in);

/// Takes the process's standard output for the protocol and points the
/// standard output handle (and the C runtime's) at stderr, so nothing else
/// can write into the protocol stream.
HANDLE take_protocol_output();

// Messages the plugin sends.
json hello_message(int protocol, std::string_view name, std::string_view version,
                   const std::vector<std::string>& capabilities);
json state_message(State state, std::string_view text = {});
json surface_request(std::uint32_t width, std::uint32_t height);
json cursor_message(int width, int height, int hot_x, int hot_y, std::span<const std::uint8_t> rgba);
json default_cursor_message();
json cursor_pos_message(int x, int y, bool visible);
json clipboard_message(std::string_view text);

/// A member as an integer, or `fallback` when missing or not a number.
std::int64_t get_int(const json& msg, const char* key, std::int64_t fallback = 0);
/// A member as a string, or empty.
std::string get_string(const json& msg, const char* key);
/// A member as a boolean, or `fallback`.
bool get_bool(const json& msg, const char* key, bool fallback = false);

}  // namespace casement
