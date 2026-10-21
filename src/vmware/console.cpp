#include "vmware/console.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>

#include "casement/log.h"
#include "vmware/failure.h"
#include "vmware/rfb_wire.h"
#include "vmware/socket_pair.h"

// rfbclient.h defines WIN32_LEAN_AND_MEAN itself; windows.h is in already.
// Its C headers aren't written for C++ warning levels.
#undef WIN32_LEAN_AND_MEAN
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <rfb/rfbclient.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace vmware {

namespace {

// LibVNCClient logs through two global functions. Its last line is kept for
// the failure text when the handshake or the stream breaks.
std::mutex log_lock;
std::string last_rfb_line;

void rfb_message(bool error, const char* format, va_list args) {
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) return;
    {
        std::lock_guard g(log_lock);
        last_rfb_line = line;
    }
    if (error) casement::log::warn("rfb: {}", line);
    else casement::log::debug("rfb: {}", line);
}

void rfb_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    rfb_message(false, format, args);
    va_end(args);
}

void rfb_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    rfb_message(true, format, args);
    va_end(args);
}

std::string last_line() {
    std::lock_guard g(log_lock);
    return last_rfb_line;
}

// The key under which the client data holds the Console.
int client_tag;

Console* console_of(rfbClient* client) { return static_cast<Console*>(rfbClientGetClientData(client, &client_tag)); }

rfbBool on_resize(rfbClient* client) { return console_of(client)->resize_framebuffer() ? TRUE : FALSE; }

void on_rect(rfbClient* client, int x, int y, int w, int h) { console_of(client)->got_rect(x, y, w, h); }

void on_update_done(rfbClient* client) { console_of(client)->update_done(); }

void on_cursor_shape(rfbClient* client, int xhot, int yhot, int width, int height, int) {
    console_of(client)->cursor_shape(xhot, yhot, width, height);
}

rfbBool on_cursor_position(rfbClient* client, int x, int y) {
    console_of(client)->cursor_position(x, y);
    return TRUE;
}

// A console ticket is the authentication; there is no VNC password.
char* no_password(rfbClient*) { return nullptr; }

constexpr std::uint8_t framebuffer_update_request = 3;
constexpr std::uint8_t qemu_message = 255;

// The biggest screen the plugin takes: well beyond any VM's resolution.
constexpr std::uint64_t max_framebuffer = std::uint64_t{16384} * 16384 * 4;

}  // namespace

Console::Console(casement::Host& host, KeyMode keys) : host_(host), keys_(keys) {
    WSADATA data;
    wsa_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    rfbClientLog = rfb_log;
    rfbClientErr = rfb_error;
}

Console::~Console() {
    stop();
    if (down_.joinable()) down_.join();
    if (up_.joinable()) up_.join();
    if (client_) {
        client_->frameBuffer = nullptr;
        rfbClientCleanup(client_);  // closes LibVNCClient's socket
        client_ = nullptr;
    } else if (lib_socket_ != INVALID_SOCKET) {
        closesocket(lib_socket_);
    }
    std::free(framebuffer_);
    if (bridge_socket_ != INVALID_SOCKET) closesocket(bridge_socket_);
    if (wsa_) WSACleanup();
}

void Console::open(const ConsoleEndpoint& endpoint, const std::string& vm_name) {
    vm_name_ = vm_name;
    Https::Options options;
    options.host = endpoint.host;
    options.port = endpoint.port;
    options.trust.pinned = endpoint.fingerprints;
    options.receive_timeout_ms = 30000;
    HttpResponse refusal;
    try {
        {
            auto https = std::make_unique<Https>(std::move(options));
            std::lock_guard g(handles_lock_);
            if (stopping_) throw Stopped();
            https_ = std::move(https);
        }
        // A request that carries nothing checks the host's certificate
        // first, so the ticket (in the WebSocket's path) goes only over a
        // connection that passed.
        https_->request(L"HEAD", "/", {}, {});
        auto ws = https_->websocket(endpoint.path, "binary", refusal);
        std::lock_guard g(handles_lock_);
        if (stopping_) throw Stopped();
        ws_ = std::move(ws);
    } catch (const HttpError& e) {
        if (e.kind == HttpError::Kind::cancelled || stopping_) throw Stopped();
        if (e.kind == HttpError::Kind::certificate)
            throw failed(std::format("The console of {} is on {}, whose certificate (SHA-256 {}) matches neither "
                                     "the fingerprint in the ticket nor a certificate Windows trusts.",
                                     vm_name, host_port(endpoint.host, endpoint.port),
                                     format_digest(e.certificate_sha256)));
        throw failed(network_text(e, std::format("the console of {}", vm_name), endpoint.host, endpoint.port));
    }
    if (!ws_)
        throw failed(std::format("The console of {} on {} refused the ticket (HTTP {}).", vm_name,
                                 host_port(endpoint.host, endpoint.port), refusal.status));
    casement::log::info("WebSocket open to {}", host_port(endpoint.host, endpoint.port));

    std::string error;
    SOCKET lib = INVALID_SOCKET, bridge = INVALID_SOCKET;
    if (!socket_pair(lib, bridge, error)) throw failed(std::format("The plugin can't start: {}.", error));
    {
        std::lock_guard g(handles_lock_);
        lib_socket_ = lib;
        bridge_socket_ = bridge;
        if (stopping_) throw Stopped();
    }
    down_ = std::thread([this] { downstream(); });
    up_ = std::thread([this] { upstream(); });

    rfbClient* client = rfbGetClient(8, 3, 4);
    if (!client) throw failed("The plugin can't start its RFB client.");
    // 32-bit little-endian pixels with red in bits 16-23: BGRX in memory,
    // the layout of Casement's surfaces.
    client->format.redShift = 16;
    client->format.greenShift = 8;
    client->format.blueShift = 0;
    client->appData.encodingsString = "copyrect tight zrle hextile zlib raw";
    client->appData.compressLevel = 3;
    client->appData.qualityLevel = 8;
    client->appData.enableJPEG = TRUE;
    client->appData.useRemoteCursor = TRUE;
    client->canHandleNewFBSize = TRUE;
    client->MallocFrameBuffer = on_resize;
    client->GotFrameBufferUpdate = on_rect;
    client->FinishedFrameBufferUpdate = on_update_done;
    client->GotCursorShape = on_cursor_shape;
    client->HandleCursorPos = on_cursor_position;
    client->GetPassword = no_password;
    client->programName = "casement-vmware";
    std::uint32_t none = rfbNoAuth;
    SetClientAuthSchemes(client, &none, 1);
    rfbClientSetClientData(client, &client_tag, this);
    // The socket is connected already; LibVNCClient skips its own connect
    // for a client that "accepted an incoming connection".
    client->listenSpecified = TRUE;
    client->sock = lib_socket_;
    rfb_socket_ = lib_socket_;
    lib_socket_ = INVALID_SOCKET;
    client_ = client;
    if (!rfbInitClient(client, nullptr, nullptr)) {
        // rfbInitClient has freed the client and closed its socket.
        client_ = nullptr;
        rfb_socket_ = INVALID_SOCKET;
        if (stopping_) throw Stopped();
        std::lock_guard g(end_lock_);
        if (end_) throw failed(end_->text);
        std::string why = last_line();
        throw failed(why.empty() ? std::format("The RFB handshake with the console of {} failed.", vm_name)
                                 : std::format("The RFB handshake with the console of {} failed: {}.", vm_name, why));
    }
    // rfbInitClient asked for the whole screen. From here on the plugin
    // writes all requests itself: LibVNCClient's own, after every update,
    // would keep more than one in flight.
    rearm();
    {
        std::lock_guard g(flow_lock_);
        outstanding_ = true;
    }
    casement::log::info("RFB {}.{} with {} ({}x{})", client->major, client->minor,
                        client->desktopName ? client->desktopName : "?", client->width, client->height);
}

ConsoleEnd Console::run() {
    {
        auto d = host_.display();
        d->on_idle([this] { display_idle(); });
        d->set_size(static_cast<std::uint32_t>(width_.load()), static_cast<std::uint32_t>(height_.load()));
    }
    running_ = true;
    while (!stopping_) {
        if (!HandleRFBServerMessage(client_)) break;
        rearm();
    }
    running_ = false;
    host_.display()->on_idle(nullptr);
    std::lock_guard g(end_lock_);
    if (end_) return *end_;
    if (stopping_) return {casement::State::closed, ""};
    std::string why = last_line();
    return {casement::State::failed,
            why.empty() ? std::format("The console of {} sent data the plugin can't read.", vm_name_)
                        : std::format("The console of {} sent data the plugin can't read: {}.", vm_name_, why)};
}

void Console::stop() {
    WebSocket* ws;
    Https* https;
    SOCKET bridge;
    {
        std::lock_guard g(handles_lock_);
        if (stopping_.exchange(true)) return;
        ws = ws_.get();
        https = https_.get();
        bridge = bridge_socket_;
    }
    // First the end of the stream LibVNCClient reads, so run() returns
    // whatever the WebSocket does. It also ends the upstream thread. The
    // socket stays open until the destructor.
    if (bridge != INVALID_SOCKET) shutdown(bridge, SD_BOTH);
    if (ws) ws->close();
    if (https) https->abort();
}

void Console::record_end(ConsoleEnd end) {
    std::lock_guard g(end_lock_);
    if (!end_) end_ = std::move(end);
}

void Console::downstream() {
    std::vector<std::uint8_t> buffer(256 * 1024);
    for (;;) {
        WebSocket::Received r = ws_->receive(buffer);
        if (r.kind == WebSocket::Received::Kind::data) {
            if (!send_all(bridge_socket_, buffer.data(), r.bytes)) break;
            continue;
        }
        if (r.kind == WebSocket::Received::Kind::closed && !stopping_) {
            std::string reason = ws_->close_reason();
            casement::log::info("the server closed the WebSocket ({} {})", ws_->close_status(), reason);
            record_end({casement::State::closed,
                        reason.empty() ? std::format("The server closed the console of {}.", vm_name_)
                                       : std::format("The server closed the console of {}: {}.", vm_name_, reason)});
        } else if (!stopping_) {
            casement::log::info("WebSocket receive failed: {}", winhttp_message(r.error));
            record_end({casement::State::closed, std::format("The connection to the console of {} was lost: {}.",
                                                             vm_name_, winhttp_message(r.error))});
        }
        break;
    }
    // LibVNCClient reads the end of the stream and returns.
    shutdown(bridge_socket_, SD_SEND);
}

void Console::upstream() {
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        int n = recv(bridge_socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (n <= 0) break;
        DWORD error = ws_->send({buffer.data(), static_cast<std::size_t>(n)});
        if (error != NO_ERROR) {
            if (!stopping_) {
                casement::log::info("WebSocket send failed: {}", winhttp_message(error));
                record_end({casement::State::closed, std::format("The connection to the console of {} was lost: {}.",
                                                                 vm_name_, winhttp_message(error))});
                // LibVNCClient reads the end of the stream and returns,
                // whether or not the receiving side has noticed.
                shutdown(bridge_socket_, SD_BOTH);
            }
            break;
        }
    }
}

void Console::write(std::span<const std::uint8_t> message) {
    if (stopping_ || rfb_socket_ == INVALID_SOCKET) return;
    std::lock_guard g(write_lock_);
    if (!send_all(rfb_socket_, message.data(), message.size())) casement::log::debug("can't write to the RFB stream");
}

void Console::rearm() {
    // Keeps LibVNCClient from sending update requests (a server's
    // SupportedMessages pseudo-rectangle sets the bit again).
    auto& bits = client_->supportedMessages.client2server[framebuffer_update_request / 8];
    bits = static_cast<std::uint8_t>(bits & ~(1 << (framebuffer_update_request % 8)));
    if (!qemu_keys_ && SupportsClient2Server(client_, qemu_message)) {
        qemu_keys_ = true;
        casement::log::info("the server takes QEMU extended key events{}",
                            keys_ == KeyMode::automatic ? "; keys go as scancodes" : ", but keys go as keysyms");
    }
}

void Console::request_update(bool incremental) {
    auto w = static_cast<std::uint16_t>(width_.load());
    auto h = static_cast<std::uint16_t>(height_.load());
    write(rfb::update_request(incremental, 0, 0, w, h));
}

// Casement's thread, the display locked: the last frame is on screen.
void Console::display_idle() {
    if (!running_ || stopping_) return;
    {
        std::lock_guard g(flow_lock_);
        if (outstanding_) return;
        outstanding_ = true;
    }
    request_update(true);
}

bool Console::resize_framebuffer() {
    rfbClient* c = client_;
    std::free(framebuffer_);
    framebuffer_ = nullptr;
    c->frameBuffer = nullptr;
    std::uint64_t bytes = std::uint64_t(c->width) * std::uint64_t(c->height) * 4;
    if (c->width <= 0 || c->height <= 0 || bytes > max_framebuffer) {
        casement::log::error("the server's screen is {}x{}", c->width, c->height);
        return false;
    }
    framebuffer_ = static_cast<std::uint8_t*>(std::calloc(static_cast<std::size_t>(bytes), 1));
    if (!framebuffer_) return false;
    c->frameBuffer = framebuffer_;
    width_ = c->width;
    height_ = c->height;
    casement::log::info("screen {}x{}", c->width, c->height);
    if (running_) {
        // A new surface, and the whole new screen with the next request.
        host_.display()->set_size(static_cast<std::uint32_t>(c->width), static_cast<std::uint32_t>(c->height));
        std::lock_guard g(flow_lock_);
        need_full_ = true;
    }
    return true;
}

void Console::got_rect(int x, int y, int w, int h) {
    rearm();
    rfbClient* c = client_;
    int x1 = std::min(x + w, c->width), y1 = std::min(y + h, c->height);
    x = std::max(x, 0);
    y = std::max(y, 0);
    if (x1 <= x || y1 <= y || !c->frameBuffer) return;
    auto d = host_.display();
    if (d->width() != static_cast<std::uint32_t>(c->width) || d->height() != static_cast<std::uint32_t>(c->height))
        return;
    std::size_t bytes = static_cast<std::size_t>(x1 - x) * 4;
    for (int row = y; row < y1; ++row)
        std::memcpy(d->buffer() + static_cast<std::size_t>(row) * d->stride() + static_cast<std::size_t>(x) * 4,
                    c->frameBuffer + (static_cast<std::size_t>(row) * c->width + x) * 4, bytes);
    d->damage(x, y, x1 - x, y1 - y);
}

void Console::update_done() {
    bool request = false, full = false;
    {
        auto d = host_.display();
        d->flush();
        std::lock_guard g(flow_lock_);
        if (need_full_) {
            need_full_ = false;
            outstanding_ = request = full = true;
        } else {
            outstanding_ = false;
            // Nothing went to Casement (no visible change), so no frame_ack
            // will ask for the next update: ask now.
            if (d->idle()) outstanding_ = request = true;
        }
    }
    if (request) request_update(!full);
}

void Console::cursor_shape(int xhot, int yhot, int width, int height) {
    rfbClient* c = client_;
    if (!c->rcSource || !c->rcMask || width <= 0 || height <= 0) return;
    std::vector<std::uint8_t> rgba = rfb::cursor_rgba(c->rcSource, c->rcMask, width, height);
    // Servers repeat the shape (x11vnc after every pointer move over a new
    // window); Casement needs it once.
    if (rgba == cursor_ && width == cursor_width_ && xhot == cursor_hot_x_ && yhot == cursor_hot_y_) return;
    cursor_ = rgba;
    cursor_width_ = width;
    cursor_hot_x_ = xhot;
    cursor_hot_y_ = yhot;
    casement::log::debug("cursor {}x{}, hot spot {},{}", width, height, xhot, yhot);
    host_.send(casement::cursor_message(width, height, xhot, yhot, rgba));
}

void Console::cursor_position(int x, int y) { host_.send(casement::cursor_pos_message(x, y, true)); }

void Console::input(const casement::json& msg) {
    if (!running_ || stopping_) return;
    std::string type = casement::get_string(msg, "type");
    if (type == "key") {
        auto scancode = static_cast<std::uint32_t>(casement::get_int(msg, "scancode"));
        bool down = casement::get_bool(msg, "down");
        bool scancodes = keys_ == KeyMode::automatic && qemu_keys_;
        for (const KeyOut& k : keyboard_.translate(scancode, down)) {
            casement::log::trace("key {:#x} {} → keysym {:#x}, XT {:#x}", scancode, down ? "down" : "up", k.keysym,
                                 k.xt);
            if (scancodes && k.xt) write(rfb::qemu_key_event(k.keysym, k.xt, k.down));
            else if (k.keysym) write(rfb::key_event(k.keysym, k.down));
        }
    } else if (type == "pointer") {
        int w = width_, h = height_;
        pointer_x_ = std::clamp(static_cast<int>(casement::get_int(msg, "x")), 0, std::max(w - 1, 0));
        pointer_y_ = std::clamp(static_cast<int>(casement::get_int(msg, "y")), 0, std::max(h - 1, 0));
        buttons_ = static_cast<std::uint8_t>(casement::get_int(msg, "buttons") & 7);
        write(rfb::pointer_event(buttons_, static_cast<std::uint16_t>(pointer_x_),
                                 static_cast<std::uint16_t>(pointer_y_)));
    } else if (type == "wheel") {
        for (const auto& m : rfb::wheel_events(static_cast<int>(casement::get_int(msg, "delta")), buttons_,
                                               static_cast<std::uint16_t>(pointer_x_),
                                               static_cast<std::uint16_t>(pointer_y_)))
            write(m);
    } else {
        casement::log::debug("ignoring {}", type);
    }
}

}  // namespace vmware
