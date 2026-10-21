#include "casement/protocol.h"

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>

#include "casement/log.h"
#include "casement/text.h"

namespace casement {

std::string_view to_string(State state) {
    switch (state) {
        case State::connected: return "connected";
        case State::auth_failed: return "auth_failed";
        case State::failed: return "failed";
        case State::closed: return "closed";
    }
    return "failed";
}

void Output::send(const json& msg) {
    if (!try_send(msg)) {
        log::error("can't write to Casement; exiting");
        ExitProcess(3);
    }
}

bool Output::try_send(const json& msg) {
    std::string body = msg.dump(-1, ' ', false, json::error_handler_t::replace);
    std::lock_guard g(lock_);
    return write_frame(handle_, body);
}

namespace {

bool write_all(HANDLE h, const void* data, std::size_t size) {
    auto p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) return false;
        p += written;
        size -= written;
    }
    return true;
}

/// Reads exactly `size` bytes. 0 at a clean end before any byte, 1 when
/// complete, -1 on an error or a truncated read.
int read_all(HANDLE h, void* data, std::size_t size) {
    auto p = static_cast<char*>(data);
    std::size_t got = 0;
    while (got < size) {
        DWORD n = 0;
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - got, 1u << 20));
        if (!ReadFile(h, p + got, chunk, &n, nullptr) || n == 0) {
            return got == 0 ? 0 : -1;
        }
        got += n;
    }
    return 1;
}

}  // namespace

bool write_frame(HANDLE out, std::string_view body) {
    if (body.size() > max_message) {
        log::error("message of {} bytes is over the 16 MiB limit", body.size());
        return false;
    }
    std::uint32_t n = static_cast<std::uint32_t>(body.size());
    std::uint8_t len[4] = {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                           static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 24)};
    std::string frame(reinterpret_cast<const char*>(len), 4);
    frame += body;
    return write_all(out, frame.data(), frame.size());
}

std::optional<std::string> read_frame(HANDLE in, std::string* error) {
    std::uint8_t len[4];
    int r = read_all(in, len, 4);
    if (r == 0) return std::nullopt;
    if (r < 0) {
        if (error) *error = "the stream ended inside a message";
        return std::nullopt;
    }
    std::size_t n = len[0] | (len[1] << 8) | (len[2] << 16) | (std::size_t{len[3]} << 24);
    if (n > max_message) {
        if (error) *error = std::format("message of {} bytes is over the 16 MiB limit", n);
        return std::nullopt;
    }
    std::string body(n, '\0');
    if (n > 0 && read_all(in, body.data(), n) != 1) {
        if (error) *error = "the stream ended inside a message";
        return std::nullopt;
    }
    return body;
}

std::optional<json> read_message(HANDLE in) {
    for (;;) {
        std::string error;
        auto body = read_frame(in, &error);
        if (!body) {
            if (!error.empty()) log::error("can't read from Casement: {}", error);
            return std::nullopt;
        }
        json msg = json::parse(*body, nullptr, false);
        if (msg.is_object() && msg.contains("type") && msg["type"].is_string()) return msg;
        log::warn("ignoring a message I can't read ({} bytes)", body->size());
    }
}

HANDLE take_protocol_output() {
    HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE);
    // Our own handle to the stream: the C runtime's stdout owns the original
    // and closes it when redirected below.
    HANDLE out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &out, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return original;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, err);
    // The C runtime's stdout too, for libraries that print.
    HANDLE dup = nullptr;
    if (err && err != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(dup), _O_TEXT);
        if (fd >= 0) {
            std::fflush(stdout);
            _dup2(fd, 1);
            _close(fd);
        } else {
            CloseHandle(dup);
        }
    }
    return out;
}

json hello_message(int protocol, std::string_view name, std::string_view version,
                   const std::vector<std::string>& capabilities) {
    return json{{"type", "hello"},
                {"protocol", protocol},
                {"name", name},
                {"version", version},
                {"capabilities", capabilities}};
}

json state_message(State state, std::string_view text) {
    json m{{"type", "state"}, {"state", to_string(state)}};
    if (!text.empty()) m["text"] = text;
    return m;
}

json surface_request(std::uint32_t width, std::uint32_t height) {
    return json{{"type", "surface_request"}, {"width", width}, {"height", height}};
}

json cursor_message(int width, int height, int hot_x, int hot_y, std::span<const std::uint8_t> rgba) {
    return json{{"type", "cursor"},   {"width", width}, {"height", height},
                {"hot_x", hot_x},     {"hot_y", hot_y}, {"rgba", base64_encode(rgba)}};
}

json default_cursor_message() { return json{{"type", "cursor"}, {"default", true}}; }

json cursor_pos_message(int x, int y, bool visible) {
    return json{{"type", "cursor_pos"}, {"x", x}, {"y", y}, {"visible", visible}};
}

json clipboard_message(std::string_view text) { return json{{"type", "clipboard"}, {"text", text}}; }

std::int64_t get_int(const json& msg, const char* key, std::int64_t fallback) {
    auto it = msg.find(key);
    if (it == msg.end()) return fallback;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number()) return static_cast<std::int64_t>(it->get<double>());
    return fallback;
}

std::string get_string(const json& msg, const char* key) {
    auto it = msg.find(key);
    return it != msg.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool get_bool(const json& msg, const char* key, bool fallback) {
    auto it = msg.find(key);
    return it != msg.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

}  // namespace casement
