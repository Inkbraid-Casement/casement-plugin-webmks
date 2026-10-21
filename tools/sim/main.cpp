// casement-sim: drives a plugin the way Casement does, for trying addresses
// by hand.
//
//   casement-sim <plugin.exe> <address> [--user U] [--password P] [--seconds S]
//       [--out DIR] [--clipboard TEXT] [--click X,Y] [--type 0x1E,0x30] [--protocol N]
//
// It answers every surface_request, acknowledges every frame, prints the
// other messages, and writes the last picture to DIR/last-frame.ppm.
// --type sends key presses: set-1 scancodes in hex, e.g. 0x1E,0x30 for "ab".

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "sim.h"

using namespace casement::sim;
using namespace std::chrono;

namespace {

std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

std::string summary(const json& m) {
    std::string s = m.dump();
    if (s.size() > 300) s = s.substr(0, 300) + "…";
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: casement-sim <plugin.exe> <address> [options]\n");
        return 2;
    }
    std::wstring program = widen(argv[1]);
    std::string address = argv[2], user, password, clipboard;
    double seconds = 10;
    std::wstring out = L".";
    int protocol = 1;
    std::optional<std::pair<int, int>> click;
    std::vector<int> keys;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--user") user = value();
        else if (a == "--password") password = value();
        else if (a == "--seconds") seconds = std::stod(value());
        else if (a == "--out") out = widen(value().c_str());
        else if (a == "--clipboard") clipboard = value();
        else if (a == "--protocol") protocol = std::stoi(value());
        else if (a == "--click") {
            std::string v = value();
            auto comma = v.find(',');
            click = std::pair{std::stoi(v.substr(0, comma)), std::stoi(v.substr(comma + 1))};
        } else if (a == "--type") {
            std::string v = value();
            for (std::size_t p = 0; p < v.size();) {
                auto comma = v.find(',', p);
                keys.push_back(std::stoi(v.substr(p, comma - p), nullptr, 16));
                p = comma == std::string::npos ? v.size() : comma + 1;
            }
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    auto c = Casement::spawn(program, {}, true);
    if (!c) return 1;
    auto hello = c->hello(protocol);
    if (!hello) {
        std::fprintf(stderr, "sim: no hello from the plugin\n");
        return 1;
    }
    std::fprintf(stderr, "sim: %s\n", summary(*hello).c_str());
    c->connect(address, password, user);

    auto end = steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(seconds));
    std::unique_ptr<HostSurface> surface;
    std::uint32_t next_id = 1, frames = 0;
    bool input_sent = false;
    while (steady_clock::now() < end) {
        auto m = c->recv(milliseconds(100));
        if (!m) {
            if (c->wait(milliseconds(0))) break;
            continue;
        }
        std::string type = m->value("type", "");
        if (type == "surface_request") {
            auto [s, msg] = c->create_surface(next_id++, (*m)["width"], (*m)["height"]);
            if (!s) {
                std::fprintf(stderr, "sim: can't create a surface\n");
                return 1;
            }
            surface = std::move(s);
            c->send(msg);
            std::fprintf(stderr, "sim: surface_request %ux%u\n", surface->width, surface->height);
        } else if (type == "frame") {
            ++frames;
            if (frames <= 3) std::fprintf(stderr, "sim: frame %u, %zu rects\n", frames, (*m)["rects"].size());
            c->send(json{{"type", "frame_ack"}, {"surface", (*m)["surface"]}});
            if (!input_sent) {
                input_sent = true;
                if (click) {
                    for (int buttons : {0, 1, 0})
                        c->send(json{{"type", "pointer"}, {"x", click->first}, {"y", click->second}, {"buttons", buttons}});
                }
                for (int k : keys) {
                    c->send(json{{"type", "key"}, {"scancode", k}, {"down", true}});
                    c->send(json{{"type", "key"}, {"scancode", k}, {"down", false}});
                }
                if (!clipboard.empty()) c->send(json{{"type", "clipboard"}, {"text", clipboard}});
            }
        } else {
            std::fprintf(stderr, "sim: %s\n", summary(*m).c_str());
        }
    }
    std::fprintf(stderr, "sim: %u frames\n", frames);
    if (surface) {
        std::wstring path = out + L"\\last-frame.ppm";
        if (write_ppm(path, surface->pixels(), surface->width, surface->height, surface->stride))
            std::fprintf(stderr, "sim: wrote last-frame.ppm\n");
    }
    c->send(json{{"type", "disconnect"}});
    c->close_stdin();
    auto code = c->wait(milliseconds(3000));
    if (code) std::fprintf(stderr, "sim: plugin exited with %lu\n", *code);
    else std::fprintf(stderr, "sim: plugin still running after 3 s\n");
    return 0;
}
