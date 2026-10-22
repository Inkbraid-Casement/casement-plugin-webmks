#include "casement/plugin.h"

#include <algorithm>

#include "casement/log.h"

namespace casement {

void Host::finish(State state, std::string_view text) {
    if (state == State::failed) log::error("failed: {}", text);
    else log::info("{} {}", to_string(state), text);
    out_.try_send(state_message(state, text));
    ExitProcess(state == State::failed ? 1 : 0);
}

int run(const Info& info, const StartSession& start, std::size_t max_rects) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = take_protocol_output();
    log::init(("CASEMENT_" + info.log_name + "_LOG").c_str());
    Host host(out, max_rects);

    // Start: hello, then connect.
    int protocol = info.max_protocol;
    std::optional<json> connect;
    while (!connect) {
        auto msg = read_message(in);
        if (!msg) return 0;
        std::string type = get_string(*msg, "type");
        if (type == "hello") {
            int offered = static_cast<int>(get_int(*msg, "protocol", 1));
            std::string app = get_string(*msg, "app");
            // The highest version both speak; if Casement's is too old, ours,
            // so Casement can say which side needs an update.
            protocol = offered >= info.min_protocol ? std::min(offered, info.max_protocol) : info.max_protocol;
            log::info("{} offers protocol {}; {} {} speaks {}", app.empty() ? "Casement" : app, offered,
                      info.name, info.version, protocol);
            host.send(hello_message(protocol, info.name, info.version, info.capabilities));
            if (offered < info.min_protocol) log::error("Casement is too old for this plugin");
        } else if (type == "connect") {
            connect = std::move(msg);
        } else if (type == "disconnect") {
            return 0;
        } else {
            log::debug("before connect, ignoring {}", type);
        }
    }

    Connect c;
    c.address = get_string(*connect, "address");
    c.username = get_string(*connect, "username");
    c.password = get_string(*connect, "password");
    c.title = get_string(*connect, "title");
    c.protocol = protocol;
    c.message = std::move(*connect);
    c.message.erase("password");
    std::unique_ptr<Session> session = start(host, c);
    std::fill(c.password.begin(), c.password.end(), '\0');
    c.password.clear();

    for (;;) {
        auto msg = read_message(in);
        std::string type = msg ? get_string(*msg, "type") : std::string();
        if (!msg || type == "disconnect") {
            if (session) session->disconnect();
            return 0;
        }
        if (type == "surface" || type == "frame_ack") {
            host.display()->handle(*msg);
        } else if (type == "hello" || type == "connect") {
            // Once only.
        } else if (session) {
            session->input(*msg);
        }
    }
}

}  // namespace casement
