// Casement's side of the protocol, for tests and the casement-sim tool:
// starts a plugin, exchanges messages with it and provides surfaces the way
// Casement does (a file mapping duplicated into the plugin's process).

#pragma once

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace casement::sim {

using json = nlohmann::json;
using std::chrono::milliseconds;

/// Casement's view of a surface.
class HostSurface {
public:
    ~HostSurface();
    HostSurface(const HostSurface&) = delete;
    HostSurface& operator=(const HostSurface&) = delete;

    const std::uint8_t* pixels() const { return view_; }
    /// The BGRX pixel at (x, y).
    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;

private:
    friend class Casement;
    HostSurface() = default;
    HANDLE mapping_ = nullptr;
    std::uint8_t* view_ = nullptr;
};

class Casement {
public:
    /// Starts `program` with piped standard streams, `env` added to this
    /// process's environment. With `echo_stderr` the plugin's log is copied
    /// to this process's stderr, prefixed with "plugin: ".
    static std::unique_ptr<Casement> spawn(const std::wstring& program, const std::vector<std::wstring>& args = {},
                                           bool echo_stderr = true,
                                           const std::vector<std::pair<std::string, std::string>>& env = {});
    ~Casement();
    Casement(const Casement&) = delete;
    Casement& operator=(const Casement&) = delete;

    bool send(const json& msg);
    /// The next message from the plugin.
    std::optional<json> recv(milliseconds timeout);
    /// The first message `pick` accepts (returns a value for), passing over
    /// others.
    template <class F>
    auto expect(milliseconds timeout, F pick) -> decltype(pick(std::declval<const json&>())) {
        auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            auto left = std::chrono::duration_cast<milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() < 0) return {};
            auto m = recv(left);
            if (!m) return {};
            if (auto v = pick(*m)) return v;
        }
    }
    /// The first message of type `type`.
    std::optional<json> expect_type(milliseconds timeout, std::string_view type);

    /// Sends `hello` and returns the plugin's.
    std::optional<json> hello(int protocol = 1, milliseconds timeout = milliseconds(5000));
    void connect(std::string_view address, std::string_view password = {}, std::string_view username = {},
                 const json& extra = json::object());

    void close_stdin();
    /// The exit code, once the plugin has exited within `timeout`.
    std::optional<DWORD> wait(milliseconds timeout);
    std::vector<std::string> stderr_lines();
    HANDLE process() const { return process_; }

    /// Creates a surface for the plugin and the `surface` message to send.
    /// Rows get `pad` extra bytes, to exercise strides.
    std::pair<std::unique_ptr<HostSurface>, json> create_surface(std::uint32_t id, std::uint32_t width,
                                                                 std::uint32_t height, std::uint32_t pad = 0);

private:
    Casement() = default;
    void read_stdout();
    void read_stderr(bool echo);

    HANDLE process_ = nullptr;
    HANDLE stdin_ = nullptr;
    HANDLE stdout_ = nullptr;
    HANDLE stderr_ = nullptr;
    std::thread out_thread_;
    std::thread err_thread_;
    std::mutex lock_;
    std::condition_variable ready_;
    std::deque<json> messages_;
    bool closed_ = false;
    std::vector<std::string> stderr_lines_;
};

/// Writes BGRX pixels as a binary PPM file.
bool write_ppm(const std::wstring& path, const std::uint8_t* bgrx, std::uint32_t width, std::uint32_t height,
               std::uint32_t stride);

}  // namespace casement::sim
