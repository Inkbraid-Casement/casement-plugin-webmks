// End to end: the plugin as Casement runs it (tools/sim), against
// tools/fake-esxi (govmomi's simulator with consoles: x11vnc and TigerVNC's
// X0tigervnc on an Xvfb display) and against plain vcsim. ci/e2e.sh starts
// them and sets:
//
//   CASEMENT_E2E_FAKE_ESXI     fake-esxi's control endpoint, host:port
//   CASEMENT_E2E_VCSIM         vcsim's API, host:port
//   CASEMENT_E2E_VCSIM_SHA256  the SHA-256 of vcsim's certificate
//   CASEMENT_E2E_VCSIM_VM_ID   the managed object id of vcsim's DC0_H0_VM0
//
// A test whose server isn't configured is skipped, unless CASEMENT_E2E=1.

#include "check.h"

#include <windows.h>

#include <wincrypt.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <thread>

#include "casement/text.h"
#include "sim.h"

using namespace casement::sim;
using namespace std::chrono_literals;
using casement::narrow;
using casement::widen;

namespace {

std::string env(const char* name) {
    char buf[2048];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n && n < sizeof(buf) ? std::string(buf, n) : std::string();
}

// Whether the servers for a test are there; skipping, or failing with
// CASEMENT_E2E=1, when they aren't.
bool configured(const char* variable) {
    if (!env(variable).empty()) return true;
    if (env("CASEMENT_E2E") == "1") ::check::fail(__FILE__, __LINE__, variable, "isn't set, but CASEMENT_E2E=1");
    else std::fprintf(stderr, "skipped: %s isn't set\n", variable);
    return false;
}

std::wstring plugin_path() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return (std::filesystem::path(self).parent_path() / PLUGIN_EXE).wstring();
}

// Plain-HTTP requests to fake-esxi's control endpoint.
class Control {
public:
    explicit Control(const std::string& host_port) {
        auto colon = host_port.rfind(':');
        session_ = WinHttpOpen(L"e2e", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        connect_ = WinHttpConnect(session_, widen(host_port.substr(0, colon)).c_str(),
                                  static_cast<INTERNET_PORT>(std::stoi(host_port.substr(colon + 1))), 0);
    }
    ~Control() {
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
    }
    json get(const std::string& path) { return request(L"GET", path); }
    json post(const std::string& path) { return request(L"POST", path); }

private:
    json request(const wchar_t* verb, const std::string& path) {
        HINTERNET r = WinHttpOpenRequest(connect_, verb, widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        std::string body;
        if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            char buf[65536];
            DWORD n = 0;
            while (WinHttpReadData(r, buf, sizeof(buf), &n) && n > 0) body.append(buf, n);
        }
        if (r) WinHttpCloseHandle(r);
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded()) std::fprintf(stderr, "control %s: %s\n", path.c_str(), body.c_str());
        return j.is_discarded() ? json() : j;
    }
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
};

// fake-esxi, with what /info says about it.
struct Fake {
    Control control;
    json info;
    std::string api;
    std::string thumbprint;

    explicit Fake(const std::string& host_port) : control(host_port) {
        info = control.get("/info");
        api = info.value("api", "");
        thumbprint = info.value("api_sha256", "");
    }
    std::string address(const std::string& vm, const std::string& user = "user", const std::string& query = "") {
        return "vmware://" + user + "@" + api + "/" + vm + "?thumbprint=" + thumbprint + (query.empty() ? "" : "&" + query);
    }
    int sessions() { return control.get("/sessions").value("sessions", -1); }
    json stats() { return control.get("/stats"); }
    void reset() {
        control.post("/stats/reset");
        control.post("/x/events/reset");
    }
    // Waits until the X events satisfy `ok`.
    bool events(const std::function<bool(const json&)>& ok, std::chrono::milliseconds timeout = 5s) {
        auto end = std::chrono::steady_clock::now() + timeout;
        do {
            json e = control.get("/x/events");
            if (e.is_array() && ok(e)) return true;
            std::this_thread::sleep_for(100ms);
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }
    // Sessions end a moment after the plugin reports.
    bool logged_out(std::chrono::milliseconds timeout = 5s) {
        auto end = std::chrono::steady_clock::now() + timeout;
        while (sessions() != 0) {
            if (std::chrono::steady_clock::now() > end) return false;
            std::this_thread::sleep_for(100ms);
        }
        return true;
    }
};

std::unique_ptr<Fake> fake() {
    if (!configured("CASEMENT_E2E_FAKE_ESXI")) return nullptr;
    auto f = std::make_unique<Fake>(env("CASEMENT_E2E_FAKE_ESXI"));
    if (f->api.empty()) ::check::fail(__FILE__, __LINE__, "fake-esxi answers /info", "");
    return f->api.empty() ? nullptr : std::move(f);
}

int count_events(const json& events, const std::string& type, const std::function<bool(const json&)>& match) {
    int n = 0;
    for (const json& e : events)
        if (e.value("type", "") == type && match(e)) ++n;
    return n;
}

// Casement's side of one tab: answers surface requests, acknowledges frames
// (unless told not to) and keeps what the plugin sent.
class Tab {
public:
    static std::unique_ptr<Tab> open(const std::string& address, const std::string& password,
                                     const std::string& user = "") {
        auto t = std::unique_ptr<Tab>(new Tab());
        t->c = Casement::spawn(plugin_path(), {}, true, {{"CASEMENT_VMWARE_LOG", "debug"}});
        if (!t->c || !t->c->hello()) return nullptr;
        t->c->connect(address, password, user);
        return t;
    }

    ~Tab() {
        if (c) {
            c->send(json{{"type", "disconnect"}});
            c->close_stdin();
            c->wait(5000ms);
        }
    }

    bool run_until(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
        auto end = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            if (auto m = c->recv(std::min(left, std::chrono::milliseconds(100)))) handle(*m);
            else if (c->wait(0ms)) return done();
        }
        return true;
    }

    void pump(std::chrono::milliseconds duration) {
        run_until([] { return false; }, duration);
    }

    std::optional<json> state(std::chrono::milliseconds timeout = 20s) {
        std::size_t seen = states.size();
        if (!run_until([&] { return states.size() > seen; }, timeout)) return std::nullopt;
        return states.back();
    }

    bool connected() {
        auto s = state();
        if (!s) return false;
        if (s->value("state", "") != "connected") std::fprintf(stderr, "state: %s\n", s->dump().c_str());
        return s->value("state", "") == "connected";
    }

    // Waits for a frame that shows `colour` (0xRRGGBB) at (x, y).
    bool shows(std::uint32_t colour, std::uint32_t x = 10, std::uint32_t y = 10, std::chrono::milliseconds timeout = 10s) {
        return run_until([&] { return surface && frames > 0 && (surface->pixel(x, y) & 0xffffff) == colour; }, timeout);
    }

    void send(const json& m) { c->send(m); }
    void key(int scancode, bool down) { send(json{{"type", "key"}, {"scancode", scancode}, {"down", down}}); }
    void tap(int scancode) {
        key(scancode, true);
        key(scancode, false);
    }
    void pointer(int x, int y, int buttons) { send(json{{"type", "pointer"}, {"x", x}, {"y", y}, {"buttons", buttons}}); }

    void ack_pending() {
        if (pending) {
            send(json{{"type", "frame_ack"}, {"surface", (*pending)["surface"]}});
            pending.reset();
        }
    }

    std::unique_ptr<Casement> c;
    std::unique_ptr<HostSurface> surface;
    bool ack = true;
    std::optional<json> pending;
    int frames = 0;
    std::vector<json> states, cursors, positions, surface_requests;

private:
    Tab() = default;

    void handle(const json& m) {
        std::string type = m.value("type", "");
        if (type == "surface_request") {
            surface_requests.push_back(m);
            auto [s, msg] = c->create_surface(next_id_++, m["width"], m["height"], 32);
            surface = std::move(s);
            c->send(msg);
        } else if (type == "frame") {
            ++frames;
            if (ack) c->send(json{{"type", "frame_ack"}, {"surface", m["surface"]}});
            else pending = m;
        } else if (type == "cursor") {
            cursors.push_back(m);
        } else if (type == "cursor_pos") {
            positions.push_back(m);
        } else if (type == "state") {
            states.push_back(m);
        }
    }

    std::uint32_t next_id_ = 1;
};

std::string text_of(const std::optional<json>& state) { return state ? state->value("text", "") : ""; }
std::string state_of(const std::optional<json>& state) { return state ? state->value("state", "") : ""; }

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

}  // namespace

// --- fake-esxi: the console ------------------------------------------------

TEST(shows_the_vm_screen) {
    auto f = fake();
    if (!f) return;
    f->control.post("/x/root?color=3366cc");
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    REQUIRE(tab->run_until([&] { return !tab->surface_requests.empty(); }, 10s));
    CHECK_EQ(tab->surface_requests[0]["width"].get<int>(), 1024);
    CHECK_EQ(tab->surface_requests[0]["height"].get<int>(), 768);
    CHECK(tab->shows(0x3366cc));
    CHECK(tab->shows(0x3366cc, 1023, 767));
    f->control.post("/x/root?color=ff8000");
    CHECK(tab->shows(0xff8000, 500, 400));
    json last = f->stats()["last"];
    CHECK_EQ(last.value("subprotocols", json::array()), json::array({"binary"}));
    CHECK_EQ(last.value("version", ""), std::string("RFB 003.008"));
    // 32 bits, depth 24, little-endian, true colour, 255 per channel, red at
    // 16, green at 8, blue at 0: BGRX in memory.
    CHECK_EQ(last.value("pixel_format", ""), std::string("IBgAAQD/AP8A/xAIAAAAAA=="));
}

TEST(shows_a_photo_like_screen) {
    auto f = fake();
    if (!f) return;
    // Many colours: Tight sends them as JPEG (quality 8), so allow a little.
    auto looks_like = [](std::uint32_t pixel, int r, int g, int b) {
        auto within = [](int a, int e) { return a >= e - 16 && a <= e + 16; };
        return within(static_cast<int>(pixel >> 16 & 255), r) && within(static_cast<int>(pixel >> 8 & 255), g) &&
               within(static_cast<int>(pixel & 255), b);
    };
    for (std::string vm : {"web-01", "qemu"}) {
        f->control.post("/x/root?color=000000");
        auto tab = Tab::open(f->address(vm), "pass");
        REQUIRE(tab);
        REQUIRE(tab->connected());
        CHECK(tab->shows(0x000000));
        f->control.post("/x/gradient");
        bool ok = tab->run_until([&] {
            return tab->surface && looks_like(tab->surface->pixel(1000, 750), 249, 249, 128) &&
                   looks_like(tab->surface->pixel(512, 100), 127, 33, 128) &&
                   looks_like(tab->surface->pixel(20, 700), 5, 232, 128);
        }, 15s);
        if (!ok && tab->surface)
            std::fprintf(stderr, "%s: %08x %08x %08x\n", vm.c_str(), tab->surface->pixel(1000, 750),
                         tab->surface->pixel(512, 100), tab->surface->pixel(20, 700));
        CHECK(ok);
    }
    f->control.post("/x/root?color=000000");
}

TEST(keeps_one_update_in_flight) {
    auto f = fake();
    if (!f) return;
    f->control.post("/x/root?color=102030");
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    tab->ack = false;
    REQUIRE(tab->connected());
    // The first frame, not acknowledged.
    REQUIRE(tab->run_until([&] { return tab->pending.has_value(); }, 10s));
    tab->pump(1000ms);
    f->reset();
    f->control.post("/x/root?color=405060");
    tab->pump(1500ms);
    // Nothing new while Casement holds the frame: no request, no frame.
    CHECK_EQ(f->stats()["last"].value("update_requests", -1), 0);
    CHECK_EQ(tab->frames, 1);
    tab->ack_pending();
    tab->ack = true;
    CHECK(tab->shows(0x405060));
    int requests = f->stats()["last"].value("update_requests", -1);
    CHECK(requests >= 1 && requests <= 3);
}

TEST(types_keysyms_to_x11vnc) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    tab->pump(1500ms);
    f->reset();
    tab->tap(0x1e);  // a
    tab->key(0x2a, true);
    tab->tap(0x1e);  // A
    tab->key(0x2a, false);
    tab->tap(0x1c);  // Return
    tab->tap(0x148);  // Up
    CHECK(f->events([](const json& e) { return count_events(e, "KeyRelease", [](const json& k) { return k["name"] == "Up"; }) == 1; }));
    json events = f->control.get("/x/events");
    CHECK_EQ(count_events(events, "KeyPress", [](const json& k) { return k["keysym"] == "0x61" && k["state"] == "0x0"; }), 1);
    CHECK_EQ(count_events(events, "KeyPress", [](const json& k) { return k["keysym"] == "0x41" && k["state"] == "0x1"; }), 1);
    CHECK_EQ(count_events(events, "KeyPress", [](const json& k) { return k["name"] == "Return"; }), 1);
    CHECK_EQ(count_events(events, "KeyRelease", [](const json& k) { return k["keycode"] == 38; }), 2);
    json last = f->stats()["last"];
    CHECK(last["qemu_keys"].is_null());
    REQUIRE(last["keys"].is_array());
    CHECK_EQ(last["keys"][0]["keysym"].get<int>(), 0x61);
    CHECK_EQ(last["keys"][3]["keysym"].get<int>(), 0x41);
}

TEST(types_scancodes_to_tigervnc) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("qemu"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    // X0tigervnc confirms QEMU extended key events with its first updates.
    tab->pump(2000ms);
    f->reset();
    tab->tap(0x1e);   // a
    tab->tap(0x11d);  // right Ctrl
    tab->key(0x21d, true);  // Pause, as Casement sends it
    tab->key(0x45, true);
    tab->key(0x21d, false);
    tab->key(0x45, false);
    tab->tap(0x153);  // Delete
    CHECK(f->events([](const json& e) { return count_events(e, "KeyRelease", [](const json& k) { return k["name"] == "Delete"; }) == 1; }));
    json last = f->stats()["last"];
    CHECK(last["keys"].is_null());
    REQUIRE(last["qemu_keys"].is_array());
    std::vector<int> codes;
    for (const json& k : last["qemu_keys"])
        if (k["down"].get<bool>()) codes.push_back(k["keycode"].get<int>());
    CHECK(codes == (std::vector<int>{0x1e, 0x9d, 0xc6, 0xd3}));
    json events = f->control.get("/x/events");
    CHECK_EQ(count_events(events, "KeyPress", [](const json& k) { return k["keycode"] == 38; }), 1);
    CHECK_EQ(count_events(events, "KeyPress", [](const json& k) { return k["name"] == "Control_R"; }), 1);
    CHECK_EQ(count_events(events, "KeyPress", [](const json& k) { return k["name"] == "Pause"; }), 1);
    CHECK_EQ(count_events(events, "KeyRelease", [](const json& k) { return k["name"] == "Pause"; }), 1);
}

TEST(keysyms_setting_wins) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("qemu", "user", "keys=keysyms"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    tab->pump(2000ms);
    f->reset();
    tab->tap(0x30);  // b
    CHECK(f->events([](const json& e) { return count_events(e, "KeyRelease", [](const json& k) { return k["name"] == "b"; }) == 1; }));
    json last = f->stats()["last"];
    CHECK(last["qemu_keys"].is_null());
    CHECK(last["keys"].is_array());
}

TEST(moves_and_clicks_the_pointer) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    tab->pump(1000ms);
    f->reset();
    tab->pointer(300, 200, 0);
    tab->pointer(300, 200, 1);
    tab->pointer(301, 200, 0);
    tab->pointer(310, 210, 2);
    tab->pointer(310, 210, 0);
    tab->pointer(320, 220, 4);
    tab->pointer(320, 220, 0);
    // Off the screen: kept to its edge.
    tab->pointer(5000, -5, 0);
    CHECK(f->events([](const json& e) { return count_events(e, "ButtonRelease", [](const json&) { return true; }) == 3; }));
    json events = f->control.get("/x/events");
    CHECK_EQ(count_events(events, "ButtonPress", [](const json& b) { return b["button"] == 1 && b["x"] == 300 && b["y"] == 200; }), 1);
    CHECK_EQ(count_events(events, "ButtonPress", [](const json& b) { return b["button"] == 2 && b["x"] == 310; }), 1);
    CHECK_EQ(count_events(events, "ButtonPress", [](const json& b) { return b["button"] == 3 && b["y"] == 220; }), 1);
    tab->pump(500ms);
    json where = f->control.get("/x/pointer");
    CHECK_EQ(where.value("x", -1), 1023);
    CHECK_EQ(where.value("y", -1), 0);
}

TEST(turns_the_wheel) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    tab->pump(1000ms);
    tab->pointer(100, 100, 0);
    f->reset();
    tab->send(json{{"type", "wheel"}, {"delta", 1}});
    tab->send(json{{"type", "wheel"}, {"delta", -2}});
    CHECK(f->events([](const json& e) { return count_events(e, "ButtonRelease", [](const json& b) { return b["button"] == 5; }) == 2; }));
    json events = f->control.get("/x/events");
    CHECK_EQ(count_events(events, "ButtonPress", [](const json& b) { return b["button"] == 4 && b["x"] == 100; }), 1);
    CHECK_EQ(count_events(events, "ButtonRelease", [](const json& b) { return b["button"] == 4; }), 1);
    CHECK_EQ(count_events(events, "ButtonPress", [](const json& b) { return b["button"] == 5; }), 2);
}

TEST(sends_the_cursor) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    // Over the root window: X's cross.
    tab->pointer(900, 700, 0);
    REQUIRE(tab->run_until([&] { return !tab->cursors.empty(); }, 10s));
    tab->pump(500ms);
    json first = tab->cursors.back();
    CHECK(first.value("width", 0) > 0 && first.value("height", 0) > 0);
    auto rgba = casement::base64_decode(first.value("rgba", ""));
    REQUIRE(rgba);
    CHECK_EQ(rgba->size(), static_cast<std::size_t>(first["width"].get<int>() * first["height"].get<int>() * 4));
    int opaque = 0;
    for (std::size_t i = 3; i < rgba->size(); i += 4) opaque += (*rgba)[i] == 255;
    CHECK(opaque > 10);
    // Over a window the shape changes, and back on the root it returns.
    f->control.post("/x/window");
    tab->pointer(60, 40, 0);
    tab->pointer(62, 41, 0);
    CHECK(tab->run_until([&] { return tab->cursors.back()["rgba"] != first["rgba"]; }, 10s));
    std::size_t shapes = tab->cursors.size();
    tab->pointer(900, 700, 0);
    tab->pointer(901, 701, 0);
    CHECK(tab->run_until([&] { return tab->cursors.size() > shapes; }, 10s));
    CHECK(tab->cursors.back()["rgba"] == first["rgba"]);
    f->control.post("/x/window/close");
    // The guest moves the pointer itself.
    f->control.post("/x/pointer?x=640&y=480");
    CHECK(tab->run_until([&] {
        return !tab->positions.empty() && tab->positions.back()["x"] == 640 && tab->positions.back()["y"] == 480;
    }, 10s));
}

TEST(follows_a_resolution_change) {
    auto f = fake();
    if (!f) return;
    f->control.post("/x/root?color=00a000");
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    CHECK(tab->shows(0x00a000));
    f->control.post("/vnc/scale?value=1/2");
    CHECK(tab->run_until([&] { return tab->surface_requests.size() >= 2; }, 10s));
    if (tab->surface_requests.size() >= 2) {
        CHECK_EQ(tab->surface_requests[1]["width"].get<int>(), 512);
        CHECK_EQ(tab->surface_requests[1]["height"].get<int>(), 384);
        CHECK(tab->shows(0x00a000, 511, 383));
        CHECK_EQ(tab->surface->width, 512u);
    }
    f->control.post("/vnc/scale?value=1");
    CHECK(tab->run_until([&] { return tab->surface && tab->surface->width == 1024; }, 10s));
    CHECK(tab->shows(0x00a000, 1023, 767));
}

TEST(reads_every_kind_of_ticket) {
    auto f = fake();
    if (!f) return;
    std::string web01 = f->info["vms"].value("web-01", "");
    for (const std::string& vm : std::vector<std::string>{"web-02", "web-03", "esx-direct", "esx-direct-nothumb",
                                                          "DC0%2Fvm%2Fweb-01", web01, "DC0%2Fvm%2Fteam%2Fdup", "100%25",
                                                          "DC0%2Fvm%2F100%25"}) {
        auto tab = Tab::open(f->address(vm), "pass");
        REQUIRE(tab);
        auto s = tab->state();
        if (state_of(s) != "connected") std::fprintf(stderr, "%s: %s\n", vm.c_str(), s ? s->dump().c_str() : "-");
        CHECK_EQ(state_of(s), std::string("connected"));
        CHECK(tab->run_until([&] { return tab->frames > 0; }, 10s));
    }
    CHECK(f->logged_out());
}

TEST(checks_the_console_certificate) {
    auto f = fake();
    if (!f) return;
    for (const std::string vm : {"wrong-thumbprint", "no-thumbprint"}) {
        f->reset();
        auto tab = Tab::open(f->address(vm), "pass");
        REQUIRE(tab);
        auto s = tab->state();
        CHECK_EQ(state_of(s), std::string("failed"));
        CHECK(contains(text_of(s), "matches neither the fingerprint in the ticket nor a certificate Windows trusts"));
        CHECK(contains(text_of(s), f->info.value("console_sha256", "?")));
        json stats = f->stats();
        // The ticket never went to the console host.
        CHECK_EQ(stats.value("ticket_requests", -1), 0);
        CHECK_EQ(tab->c->wait(5000ms).value_or(99), 1ul);
    }
    CHECK(f->logged_out());
}

TEST(reports_a_refused_ticket) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("refused"), "pass");
    REQUIRE(tab);
    auto s = tab->state();
    CHECK_EQ(state_of(s), std::string("failed"));
    CHECK(contains(text_of(s), "refused the ticket (HTTP 404)"));
    CHECK(f->logged_out());
}

TEST(reports_a_closed_console) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("closing"), "pass");
    REQUIRE(tab);
    REQUIRE(tab->connected());
    auto s = tab->state(10s);
    CHECK_EQ(state_of(s), std::string("closed"));
    CHECK_EQ(text_of(s), std::string("The server closed the console of closing: The virtual machine was powered off."));
    CHECK_EQ(tab->c->wait(5000ms).value_or(99), 0ul);
    CHECK(f->logged_out());
}

TEST(logs_out_on_disconnect) {
    auto f = fake();
    if (!f) return;
    {
        auto tab = Tab::open(f->address("web-01"), "pass");
        REQUIRE(tab);
        REQUIRE(tab->connected());
        CHECK_EQ(f->sessions(), 1);
        tab->send(json{{"type", "disconnect"}});
        tab->c->close_stdin();
        CHECK_EQ(tab->c->wait(3000ms).value_or(99), 0ul);
    }
    CHECK(f->logged_out(1000ms));
    CHECK(f->stats()["last"].value("client_closed", false));
    // Disconnected while still connecting.
    auto tab = Tab::open(f->address("web-01"), "pass");
    REQUIRE(tab);
    tab->send(json{{"type", "disconnect"}});
    tab->c->close_stdin();
    CHECK_EQ(tab->c->wait(3000ms).value_or(99), 0ul);
    CHECK(f->logged_out());
}

// --- fake-esxi: the API ----------------------------------------------------

TEST(finds_no_wrong_vm) {
    auto f = fake();
    if (!f) return;
    struct Case {
        std::string vm, text;
    } cases[] = {
        {"off", "off is powered off; its console is there only while it runs."},
        {"dup", "Several VMs are called dup; use the inventory path."},
        {"nosuchvm", "There's no VM called “nosuchvm”."},
        {"DC0%2Fvm%2Fnosuchvm", "There's no VM at “DC0/vm/nosuchvm”."},
        {"DC0%2Fhost%2FDC0_C0", "“DC0/host/DC0_C0” isn't a VM."},
        {"vm-9999", "There's no VM with the id or name “vm-9999”."},
    };
    for (const Case& c : cases) {
        auto tab = Tab::open(f->address(c.vm), "pass");
        REQUIRE(tab);
        auto s = tab->state();
        CHECK_EQ(state_of(s), std::string("failed"));
        CHECK_EQ(text_of(s), c.text);
    }
    CHECK(f->logged_out());
}

TEST(needs_the_console_privilege) {
    auto f = fake();
    if (!f) return;
    auto tab = Tab::open(f->address("web-01", "noconsole"), "pass");
    REQUIRE(tab);
    auto s = tab->state();
    CHECK_EQ(state_of(s), std::string("failed"));
    CHECK_EQ(text_of(s), std::string("noconsole may not open the console of web-01 (it needs the privilege "
                                     "VirtualMachine.Interact.ConsoleInteract)."));
    CHECK(f->logged_out());
}

TEST(asks_for_the_password) {
    auto f = fake();
    if (!f) return;
    f->reset();
    auto none = Tab::open(f->address("web-01"), "");
    REQUIRE(none);
    auto s = none->state();
    CHECK_EQ(state_of(s), std::string("auth_failed"));
    CHECK_EQ(none->c->wait(5000ms).value_or(99), 0ul);
    CHECK(f->stats()["counts"].value("logins", 0) == 0);
    auto wrong = Tab::open(f->address("web-01"), "wrong");
    REQUIRE(wrong);
    s = wrong->state();
    CHECK_EQ(state_of(s), std::string("auth_failed"));
    CHECK_EQ(text_of(s), std::string("The user name or password is wrong."));
    CHECK_EQ(wrong->c->wait(5000ms).value_or(99), 0ul);
}

TEST(checks_the_api_certificate) {
    auto f = fake();
    if (!f) return;
    std::string expected = "Untrusted certificate, SHA-256 " + f->thumbprint +
                           "; add it as the certificate fingerprint to trust it.";
    std::string wrong = "00" + f->thumbprint.substr(2);
    for (const std::string& address :
         std::vector<std::string>{"vmware://user@" + f->api + "/web-01", "vmware://user@" + f->api + "/web-01?thumbprint=" + wrong}) {
        f->reset();
        auto tab = Tab::open(address, "pass");
        REQUIRE(tab);
        auto s = tab->state();
        CHECK_EQ(state_of(s), std::string("failed"));
        CHECK_EQ(text_of(s), expected);
        // The password never reached the server.
        CHECK_EQ(f->stats()["counts"].value("logins", 0), 0);
    }
}

TEST(trusts_what_windows_trusts) {
    auto f = fake();
    if (!f) return;
    auto ca = casement::base64_decode(f->info.value("ca", ""));
    REQUIRE(ca);
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, ca->data(), static_cast<DWORD>(ca->size()));
    REQUIRE(cert);
    // The run's own CA, in the current user's root store for this test only.
    HCERTSTORE root = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"Root");
    REQUIRE(root);
    PCCERT_CONTEXT added = nullptr;
    CHECK(CertAddCertificateContextToStore(root, cert, CERT_STORE_ADD_REPLACE_EXISTING, &added));
    // Saved now, for the plugin's process (Wine writes registry stores lazily).
    CertControlStore(root, 0, CERT_STORE_CTRL_COMMIT, nullptr);
    {
        auto tab = Tab::open("vmware://user@" + f->api + "/web-01", "pass");
        REQUIRE(tab);
        auto s = tab->state();
        if (state_of(s) != "connected") std::fprintf(stderr, "%s\n", s ? s->dump().c_str() : "-");
        CHECK_EQ(state_of(s), std::string("connected"));
    }
    if (added) CertDeleteCertificateFromStore(added);
    CertControlStore(root, 0, CERT_STORE_CTRL_COMMIT, nullptr);
    CertCloseStore(root, 0);
    CertFreeCertificateContext(cert);
}

// --- plain vcsim -----------------------------------------------------------

namespace {

std::string vcsim(const std::string& vm, const std::string& thumbprint = "") {
    return "vmware://user@" + env("CASEMENT_E2E_VCSIM") + "/" + vm + "?thumbprint=" +
           (thumbprint.empty() ? env("CASEMENT_E2E_VCSIM_SHA256") : thumbprint);
}

std::optional<json> outcome(const std::string& address, const std::string& password = "pass") {
    auto tab = Tab::open(address, password);
    if (!tab) return std::nullopt;
    auto s = tab->state();
    tab->c->wait(5000ms);
    return s;
}

}  // namespace

TEST(vcsim_finds_vms_but_issues_no_tickets) {
    if (!configured("CASEMENT_E2E_VCSIM")) return;
    std::string host = env("CASEMENT_E2E_VCSIM");
    for (const std::string& vm : std::vector<std::string>{"DC0_H0_VM0", "DC0%2Fvm%2FDC0_H0_VM0", env("CASEMENT_E2E_VCSIM_VM_ID"),
                                                          "DC0%2Fvm%2Fteam%2FDC0_C0_RP0_VM1"}) {
        auto s = outcome(vcsim(vm));
        CHECK_EQ(state_of(s), std::string("failed"));
        CHECK(contains(text_of(s), host + " doesn't issue console tickets"));
        CHECK(contains(text_of(s), "does not implement: AcquireTicket"));
    }
}

TEST(vcsim_reports_lookup_failures) {
    if (!configured("CASEMENT_E2E_VCSIM")) return;
    CHECK_EQ(text_of(outcome(vcsim("DC0_H0_VM1"))), std::string("DC0_H0_VM1 is powered off; its console is there only while it runs."));
    CHECK_EQ(text_of(outcome(vcsim("DC0_C0_RP0_VM1"))), std::string("Several VMs are called DC0_C0_RP0_VM1; use the inventory path."));
    CHECK_EQ(text_of(outcome(vcsim("nosuchvm"))), std::string("There's no VM called “nosuchvm”."));
    CHECK_EQ(text_of(outcome(vcsim("DC0%2Fvm%2Fnosuchvm"))), std::string("There's no VM at “DC0/vm/nosuchvm”."));
}

TEST(vcsim_checks_login_and_certificate) {
    if (!configured("CASEMENT_E2E_VCSIM")) return;
    auto wrong = outcome(vcsim("DC0_H0_VM0"), "wrong");
    CHECK_EQ(state_of(wrong), std::string("auth_failed"));
    auto untrusted = outcome("vmware://user@" + env("CASEMENT_E2E_VCSIM") + "/DC0_H0_VM0");
    CHECK_EQ(state_of(untrusted), std::string("failed"));
    CHECK_EQ(text_of(untrusted), "Untrusted certificate, SHA-256 " + env("CASEMENT_E2E_VCSIM_SHA256") +
                                     "; add it as the certificate fingerprint to trust it.");
}

CHECK_MAIN
