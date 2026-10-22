// The plugin's life: `hello`, `connect`, then input and surfaces until
// `disconnect` or the end of stdin. A plugin provides its description and a
// function that starts a session for a `connect`.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "casement/display.h"
#include "casement/protocol.h"

namespace casement {

struct Info {
    /// Upper-case name for the log level variable: CASEMENT_<log_name>_LOG.
    std::string log_name;
    std::string name;
    std::string version;
    std::vector<std::string> capabilities;
    /// The protocol versions this plugin speaks.
    int min_protocol = 1;
    int max_protocol = 1;
};

/// The `connect` message.
struct Connect {
    std::string address;
    std::string username;
    std::string password;
    std::string title;
    /// The protocol version agreed in `hello`.
    int protocol = 1;
    json message;
};

/// Everything a session needs to talk to Casement.
class Host {
public:
    explicit Host(HANDLE out, std::size_t max_rects = 16) : out_(out), display_(out_, max_rects) {}

    void send(const json& msg) { out_.send(msg); }
    Output& output() { return out_; }

    /// The display, locked for as long as the result lives.
    LockedDisplay display() { return LockedDisplay(display_lock_, display_); }

    /// Reports a final state and ends the process: `closed` and
    /// `auth_failed` exit with 0, `failed` with 1.
    [[noreturn]] void finish(State state, std::string_view text);

private:
    Output out_;
    std::mutex display_lock_;
    Display display_;
};

class Session {
public:
    virtual ~Session() = default;
    /// Input from Casement: key, pointer, wheel, clipboard, resize, mute
    /// and anything newer. Must not block for long.
    virtual void input(const json& msg) = 0;
    /// Casement sent `disconnect` or closed stdin: close the connection.
    /// Casement ends the process two seconds later, so keep this short.
    virtual void disconnect() = 0;
};

using StartSession = std::function<std::unique_ptr<Session>(Host& host, const Connect& connect)>;

/// Runs the plugin over the process's standard streams until the tab ends.
/// Returns the exit code.
int run(const Info& info, const StartSession& start, std::size_t max_rects = 16);

}  // namespace casement
