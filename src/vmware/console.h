// The VM console: a WebSocket to the host's /ticket/<ticket>, carrying plain
// RFB, which LibVNCClient decodes.
//
// LibVNCClient reads and writes a socket, so it gets one end of a loopback
// socket pair and two threads copy between the other end and the WebSocket.
// LibVNCClient handles the handshake and decodes the server's messages on
// the console's thread. The plugin writes the client's messages itself
// (input from Casement's thread, update requests when Casement has taken a
// frame), one send() per message, so they never interleave.

#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "casement/plugin.h"
#include "vmware/https.h"
#include "vmware/keyboard.h"
#include "vmware/target.h"
#include "vmware/vsphere.h"

struct _rfbClient;

namespace vmware {

/// How a console connection ended: `closed` or `failed`, with a sentence.
struct ConsoleEnd {
    casement::State state = casement::State::closed;
    std::string text;
};

class Console {
public:
    Console(casement::Host& host, KeyMode keys);
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    /// Opens the WebSocket and runs the RFB handshake up to ServerInit.
    /// Throws Failure, or Stopped after stop().
    void open(const ConsoleEndpoint& endpoint, const std::string& vm_name);

    /// After `connected` was sent: shows the picture and handles the
    /// server's messages until the connection ends.
    ConsoleEnd run();

    /// Key, pointer and wheel events from Casement.
    void input(const casement::json& msg);

    /// Ends the connection; run() returns. From any thread; only the first
    /// call does anything.
    void stop();

    // LibVNCClient's callbacks.
    bool resize_framebuffer();
    void got_rect(int x, int y, int w, int h);
    void update_done();
    void cursor_shape(int xhot, int yhot, int width, int height);
    void cursor_position(int x, int y);

private:
    void downstream();
    void upstream();
    void record_end(ConsoleEnd end);
    void write(std::span<const std::uint8_t> message);
    void request_update(bool incremental);
    void display_idle();
    void rearm();

    casement::Host& host_;
    KeyMode keys_;
    std::string vm_name_;
    // https_, ws_ and the sockets are set while connecting and used by
    // stop() from another thread.
    std::mutex handles_lock_;
    std::unique_ptr<Https> https_;
    std::unique_ptr<WebSocket> ws_;
    _rfbClient* client_ = nullptr;
    // The screen LibVNCClient decodes into; allocated here, since
    // LibVNCClient never frees it.
    std::uint8_t* framebuffer_ = nullptr;
    SOCKET lib_socket_ = INVALID_SOCKET;  // LibVNCClient's end until the handshake starts
    // LibVNCClient's end once it owns it, for the plugin's own writes.
    SOCKET rfb_socket_ = INVALID_SOCKET;
    SOCKET bridge_socket_ = INVALID_SOCKET;
    std::thread down_;
    std::thread up_;
    bool wsa_ = false;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> qemu_keys_{false};
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};

    // Update requests: one in flight, the next once Casement has the frame.
    std::mutex flow_lock_;
    bool outstanding_ = false;
    bool need_full_ = false;

    // Writes to LibVNCClient's socket, one message at a time.
    std::mutex write_lock_;

    std::mutex end_lock_;
    std::optional<ConsoleEnd> end_;

    // The console's thread only: the last cursor sent.
    std::vector<std::uint8_t> cursor_;
    int cursor_width_ = 0;
    int cursor_hot_x_ = 0;
    int cursor_hot_y_ = 0;

    // Casement's thread only.
    Keyboard keyboard_;
    int pointer_x_ = 0;
    int pointer_y_ = 0;
    std::uint8_t buttons_ = 0;
};

}  // namespace vmware
