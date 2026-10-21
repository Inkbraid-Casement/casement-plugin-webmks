// HTTPS requests and WebSocket connections through WinHTTP, with the
// plugin's own certificate check.
//
// WinHTTP is told to accept any certificate during the TLS handshake. Before
// it sends a request on a connection (WINHTTP_CALLBACK_STATUS_SENDING_REQUEST)
// the plugin checks the server's certificate and, if it doesn't pass, closes
// the request handle in the callback, which on Windows stops the request
// before any of it is sent. Request bodies, which carry the password, are
// written only after the check passed, so they never depend on that.

#pragma once

#include <windows.h>

#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "vmware/certificate.h"

namespace vmware {

class HttpError : public std::runtime_error {
public:
    enum class Kind {
        /// Name resolution, connection, TLS or a broken connection.
        network,
        /// The server's certificate isn't trusted.
        certificate,
        /// abort() stopped the request.
        cancelled,
    };
    HttpError(Kind kind, const std::string& text, DWORD code = 0) : std::runtime_error(text), kind(kind), code(code) {}
    Kind kind;
    DWORD code;
    /// For `certificate`: the SHA-256 of the certificate the server presented.
    std::vector<std::uint8_t> certificate_sha256;
};

struct HttpResponse {
    DWORD status = 0;
    std::string body;
    /// The Set-Cookie headers.
    std::vector<std::string> cookies;
};

/// A WebSocket after the upgrade. receive() and send() may run on two
/// threads at once; close() may come from a third.
class WebSocket {
public:
    explicit WebSocket(HINTERNET handle) : handle_(handle) {}
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    struct Received {
        enum class Kind { data, closed, error } kind = Kind::error;
        std::size_t bytes = 0;
        DWORD error = 0;
    };

    /// Waits for the next bytes the server sends in binary messages. Text
    /// messages are skipped.
    Received receive(std::span<std::uint8_t> buffer);
    /// Sends one binary message: 0 or the error. Sends from several threads
    /// go one after the other.
    DWORD send(std::span<const std::uint8_t> data);
    /// Sends a close frame (status 1000), unless a send is under way, and
    /// closes the handle without waiting for the server's close frame,
    /// which ends calls in progress on other threads.
    void close();
    /// After receive() reported `closed`: the server's close status and reason.
    USHORT close_status() const { return close_status_; }
    const std::string& close_reason() const { return close_reason_; }

private:
    std::atomic<HINTERNET> handle_;
    // WinHTTP takes one send at a time, and a close frame is a send too.
    std::mutex send_lock_;
    USHORT close_status_ = 0;
    std::string close_reason_;
};

class Https {
public:
    struct Options {
        std::string host;  // a name or an address, IPv6 without brackets
        std::uint16_t port = 443;
        Trust trust;
        DWORD connect_timeout_ms = 15000;
        DWORD receive_timeout_ms = 60000;
    };

    /// Throws HttpError if WinHTTP can't be set up.
    explicit Https(Options options);
    ~Https();
    Https(const Https&) = delete;
    Https& operator=(const Https&) = delete;

    /// Sends a request and reads the whole response. Throws HttpError.
    HttpResponse request(const wchar_t* verb, const std::string& path, const std::vector<std::wstring>& headers,
                         std::string_view body);

    /// Opens a WebSocket on `path`, offering the subprotocol `protocol`.
    /// Throws HttpError; when the server answers without upgrading, returns
    /// null with the answer in `refusal`.
    std::unique_ptr<WebSocket> websocket(const std::string& path, const std::string& protocol, HttpResponse& refusal);

    /// Stops the request in progress on another thread. Later requests work.
    void abort();

    /// For the requests that follow, e.g. a quick logout before exiting.
    void set_timeouts(DWORD connect_ms, DWORD receive_ms);

    /// The SHA-256 of the certificate the server presented last, if any.
    std::vector<std::uint8_t> certificate_sha256() const;

    const std::string& host() const { return options_.host; }
    std::uint16_t port() const { return options_.port; }

    /// A request in progress (internal; shared with the status callback).
    struct Pending;

private:
    // Opens and prepares a request; registers it for abort().
    void open(Pending& p, const wchar_t* verb, const std::string& path, const std::vector<std::wstring>& headers);
    // Sends it and waits for the response headers. Throws HttpError.
    void send(Pending& p, std::string_view body);
    void finish(Pending& p);

    Options options_;
    std::wstring whost_;
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    std::atomic<DWORD> connect_timeout_ms_;
    std::atomic<DWORD> receive_timeout_ms_;
    mutable std::mutex lock_;
    Pending* pending_ = nullptr;
    std::vector<std::uint8_t> last_sha256_;
};

/// WinHTTP's text for an error code.
std::string winhttp_message(DWORD code);

/// "host", or "host:port" ("[v6]:port") when the port isn't 443.
std::string host_port(const std::string& host, std::uint16_t port);

/// A sentence for a network failure (not a certificate) on the way to
/// `what`, for example "the vSphere API".
std::string network_text(const HttpError& e, std::string_view what, const std::string& host, std::uint16_t port);

}  // namespace vmware
