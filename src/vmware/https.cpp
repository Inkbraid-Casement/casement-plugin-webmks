#include "vmware/https.h"

#include <format>

#include "casement/log.h"
#include "casement/text.h"

namespace vmware {

namespace {

// Replies bigger than this aren't vSphere API answers.
constexpr std::size_t max_body = std::size_t{256} << 20;

constexpr DWORD ignore_certificate_errors = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

}  // namespace

std::string winhttp_message(DWORD code) {
    // WinHTTP's messages live in winhttp.dll, not the system table.
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buffer), 0,
                             nullptr);
    std::string text;
    if (n && buffer) {
        text = casement::narrow(std::wstring_view(buffer, n));
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.' || text.back() == ' '))
        text.pop_back();
    if (text.empty()) text = std::format("error {}", code);
    return text;
}

std::string host_port(const std::string& host, std::uint16_t port) {
    bool v6 = host.find(':') != std::string::npos;
    if (port == 443) return host;
    return v6 ? std::format("[{}]:{}", host, port) : std::format("{}:{}", host, port);
}

std::string network_text(const HttpError& e, std::string_view what, const std::string& host, std::uint16_t port) {
    switch (e.code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return std::format("Can't find the host {}.", host);
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_TIMEOUT:
            return std::format("Can't connect to {} on {}: {}.", what, host_port(host, port), e.what());
        case ERROR_WINHTTP_SECURE_FAILURE:
            return std::format("The TLS connection to {} on {} failed: {}.", what, host_port(host, port), e.what());
        default:
            return std::format("The connection to {} on {} failed: {}.", what, host_port(host, port), e.what());
    }
}

// One request: its handle and what the certificate check in the status
// callback found. The handle is closed exactly once, by whichever of the
// callback, abort() or the end of the request gets there first.
struct Https::Pending {
    HINTERNET handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> aborted{false};
    const std::wstring* host = nullptr;
    const Trust* trust = nullptr;
    bool checked = false;
    bool rejected = false;
    std::vector<std::uint8_t> sha256;
    std::string why;

    void close() {
        if (handle && !closed.exchange(true)) WinHttpCloseHandle(handle);
    }
};

namespace {

// Runs on the requesting thread just before WinHTTP writes the request: the
// TLS handshake is done and nothing has been sent yet.
void check_server(HINTERNET request, Https::Pending& p) {
    PCCERT_CONTEXT cert = nullptr;
    DWORD size = sizeof(cert);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) || !cert) {
        p.rejected = true;
        p.why = "the server presented no certificate";
    } else {
        Verdict v = check_certificate(cert, *p.host, *p.trust);
        CertFreeCertificateContext(cert);
        p.sha256 = v.sha256;
        if (!v.trusted) {
            p.rejected = true;
            p.why = v.windows_error;
            casement::log::info("certificate {} rejected; Windows: {}", format_digest(v.sha256), v.windows_error);
        } else {
            casement::log::trace("certificate {} accepted ({})", format_digest(v.sha256),
                                 v.by_windows ? "trusted by Windows" : "pinned");
        }
    }
    p.checked = true;
    // Closing the handle here stops the request before any of it is sent
    // (the pattern Microsoft documents for checking certificates).
    if (p.rejected) p.close();
}

void CALLBACK status_callback(HINTERNET request, DWORD_PTR context, DWORD status, LPVOID, DWORD) {
    if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || context == 0) return;
    check_server(request, *reinterpret_cast<Https::Pending*>(context));
}

}  // namespace

WebSocket::~WebSocket() { close(); }

WebSocket::Received WebSocket::receive(std::span<std::uint8_t> buffer) {
    for (;;) {
        HINTERNET h = handle_.load();
        if (!h) return {Received::Kind::error, 0, ERROR_WINHTTP_OPERATION_CANCELLED};
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        DWORD error = WinHttpWebSocketReceive(h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
        if (error != NO_ERROR) return {Received::Kind::error, 0, error};
        switch (type) {
            case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
            case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
                if (read == 0) continue;
                return {Received::Kind::data, read, 0};
            case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE: {
                USHORT status = 0;
                BYTE reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH];
                DWORD length = 0;
                if (WinHttpWebSocketQueryCloseStatus(h, &status, reason, sizeof(reason), &length) == NO_ERROR) {
                    close_status_ = status;
                    close_reason_.assign(reinterpret_cast<const char*>(reason), length);
                }
                return {Received::Kind::closed, 0, 0};
            }
            default:
                casement::log::debug("ignoring a text WebSocket message of {} bytes", read);
                continue;
        }
    }
}

DWORD WebSocket::send(std::span<const std::uint8_t> data) {
    std::lock_guard g(send_lock_);
    HINTERNET h = handle_.load();
    if (!h) return ERROR_WINHTTP_OPERATION_CANCELLED;
    return WinHttpWebSocketSend(h, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                const_cast<std::uint8_t*>(data.data()), static_cast<DWORD>(data.size()));
}

void WebSocket::close() {
    // A send under way may be waiting for the server to read; the close
    // doesn't wait behind it, and goes without the close frame.
    std::unique_lock g(send_lock_, std::try_to_lock);
    HINTERNET h = handle_.exchange(nullptr);
    if (!h) return;
    if (g.owns_lock()) WinHttpWebSocketShutdown(h, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle(h);
}

Https::Https(Options options) : options_(std::move(options)), whost_(casement::widen(options_.host)) {
    session_ = WinHttpOpen(L"Casement-VMware/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) {
        DWORD e = GetLastError();
        throw HttpError(HttpError::Kind::network, std::format("WinHTTP can't start: {}", winhttp_message(e)), e);
    }
    // TLS 1.2 or 1.3 only; ESXi 6.7 and later speak 1.2.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        // Windows before 11 don't know TLS 1.3 here.
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
#else
    WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
#endif
    set_timeouts(options_.connect_timeout_ms, options_.receive_timeout_ms);
    connect_ = WinHttpConnect(session_, whost_.c_str(), options_.port, 0);
    if (!connect_) {
        DWORD e = GetLastError();
        WinHttpCloseHandle(session_);
        session_ = nullptr;
        throw HttpError(HttpError::Kind::network,
                        std::format("can't use {}:{}: {}", options_.host, options_.port, winhttp_message(e)), e);
    }
}

Https::~Https() {
    abort();
    if (connect_) WinHttpCloseHandle(connect_);
    if (session_) WinHttpCloseHandle(session_);
}

void Https::set_timeouts(DWORD connect_ms, DWORD receive_ms) {
    connect_timeout_ms_ = connect_ms;
    receive_timeout_ms_ = receive_ms;
}

void Https::abort() {
    std::lock_guard g(lock_);
    if (pending_) {
        pending_->aborted = true;
        pending_->close();
    }
}

std::vector<std::uint8_t> Https::certificate_sha256() const {
    std::lock_guard g(lock_);
    return last_sha256_;
}

void Https::open(Pending& p, const wchar_t* verb, const std::string& path, const std::vector<std::wstring>& headers) {
    p.host = &whost_;
    p.trust = &options_.trust;
    std::wstring wpath = casement::widen(path);
    p.handle = WinHttpOpenRequest(connect_, verb, wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!p.handle) {
        DWORD e = GetLastError();
        throw HttpError(HttpError::Kind::network, winhttp_message(e), e);
    }
    // Per request: handles copy their timeouts when they are created.
    WinHttpSetTimeouts(p.handle, 0, static_cast<int>(connect_timeout_ms_.load()), 30000,
                       static_cast<int>(receive_timeout_ms_.load()));
    DWORD flags = ignore_certificate_errors;
    DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&p);
    if (!WinHttpSetOption(p.handle, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags)) ||
        !WinHttpSetOption(p.handle, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features)) ||
        !WinHttpSetOption(p.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(p.handle, status_callback, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
        DWORD e = GetLastError();
        p.close();
        throw HttpError(HttpError::Kind::network, std::format("can't prepare the request: {}", winhttp_message(e)), e);
    }
    for (const std::wstring& h : headers) {
        if (!WinHttpAddRequestHeaders(p.handle, h.c_str(), static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            DWORD e = GetLastError();
            p.close();
            throw HttpError(HttpError::Kind::network, std::format("can't add a header: {}", winhttp_message(e)), e);
        }
    }
    std::lock_guard g(lock_);
    pending_ = &p;
}

void Https::finish(Pending& p) {
    {
        std::lock_guard g(lock_);
        if (pending_ == &p) pending_ = nullptr;
        if (!p.sha256.empty()) last_sha256_ = p.sha256;
    }
    p.close();
}

void Https::send(Pending& p, std::string_view body) {
    // The headers first. The body (a password, for Login) is written only
    // once the certificate passed: closing the handle in the callback stops
    // the request on Windows, but not everywhere WinHTTP is implemented
    // (Wine sends the request line and headers anyway).
    BOOL ok = WinHttpSendRequest(p.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                 static_cast<DWORD>(body.size()), 0);
    DWORD error = ok ? 0 : GetLastError();
    if (ok && !p.checked) {
        // The status callback should have run; check here rather than not at
        // all if it didn't.
        casement::log::warn("WinHTTP sent the request without the certificate callback");
        check_server(p.handle, p);
    }
    if (p.rejected) ok = FALSE;
    while (ok && !body.empty()) {
        DWORD written = 0;
        ok = WinHttpWriteData(p.handle, body.data(), static_cast<DWORD>(body.size()), &written);
        if (!ok) error = GetLastError();
        else if (written == 0) ok = FALSE;
        else body.remove_prefix(written);
    }
    if (ok) {
        ok = WinHttpReceiveResponse(p.handle, nullptr);
        if (!ok) error = GetLastError();
    }
    if (ok) return;
    if (p.rejected) {
        HttpError e(HttpError::Kind::certificate, std::format("the certificate isn't trusted: {}", p.why));
        e.certificate_sha256 = p.sha256;
        throw e;
    }
    if (p.aborted) throw HttpError(HttpError::Kind::cancelled, "cancelled", ERROR_WINHTTP_OPERATION_CANCELLED);
    throw HttpError(HttpError::Kind::network, winhttp_message(error), error);
}

HttpResponse Https::request(const wchar_t* verb, const std::string& path, const std::vector<std::wstring>& headers,
                            std::string_view body) {
    Pending p;
    try {
        open(p, verb, path, headers);
        send(p, body);
        HttpResponse r;
        DWORD size = sizeof(r.status);
        WinHttpQueryHeaders(p.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &size, WINHTTP_NO_HEADER_INDEX);
        for (DWORD index = 0;;) {
            wchar_t value[4096];
            DWORD bytes = sizeof(value);
            DWORD before = index;
            if (!WinHttpQueryHeaders(p.handle, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX, value, &bytes,
                                     &index) ||
                index == before)
                break;
            r.cookies.push_back(casement::narrow(std::wstring_view(value, bytes / sizeof(wchar_t))));
        }
        std::vector<char> chunk(64 * 1024);
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(p.handle, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
                DWORD e = GetLastError();
                if (p.aborted) throw HttpError(HttpError::Kind::cancelled, "cancelled", e);
                throw HttpError(HttpError::Kind::network, winhttp_message(e), e);
            }
            if (read == 0) break;
            r.body.append(chunk.data(), read);
            if (r.body.size() > max_body)
                throw HttpError(HttpError::Kind::network, "the reply is far larger than any API answer");
        }
        finish(p);
        return r;
    } catch (...) {
        finish(p);
        throw;
    }
}

std::unique_ptr<WebSocket> Https::websocket(const std::string& path, const std::string& protocol,
                                            HttpResponse& refusal) {
    Pending p;
    try {
        open(p, L"GET", path, {casement::widen("Sec-WebSocket-Protocol: " + protocol)});
        if (!WinHttpSetOption(p.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            DWORD e = GetLastError();
            throw HttpError(HttpError::Kind::network, std::format("WinHTTP can't open WebSockets: {}",
                                                                  winhttp_message(e)), e);
        }
        send(p, {});
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(p.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != HTTP_STATUS_SWITCH_PROTOCOLS) {
            refusal.status = status;
            char chunk[4096];
            DWORD read = 0;
            while (refusal.body.size() < 16384 && WinHttpReadData(p.handle, chunk, sizeof(chunk), &read) && read > 0)
                refusal.body.append(chunk, read);
            finish(p);
            return nullptr;
        }
        wchar_t chosen[256];
        DWORD bytes = sizeof(chosen);
        if (WinHttpQueryHeaders(p.handle, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol", chosen, &bytes,
                                WINHTTP_NO_HEADER_INDEX)) {
            std::string answer = casement::narrow(std::wstring_view(chosen, bytes / sizeof(wchar_t)));
            if (answer != protocol)
                throw HttpError(HttpError::Kind::network,
                                std::format("the server chose the WebSocket protocol “{}”, not “{}”", answer, protocol));
        } else {
            casement::log::debug("the server confirmed no WebSocket protocol");
        }
        // The WebSocket handle mustn't call back into this function's state.
        WinHttpSetStatusCallback(p.handle, nullptr, 0, 0);
        DWORD_PTR none = 0;
        WinHttpSetOption(p.handle, WINHTTP_OPTION_CONTEXT_VALUE, &none, sizeof(none));
        HINTERNET ws = WinHttpWebSocketCompleteUpgrade(p.handle, 0);
        if (!ws) {
            DWORD e = GetLastError();
            throw HttpError(HttpError::Kind::network, std::format("the WebSocket upgrade failed: {}",
                                                                  winhttp_message(e)), e);
        }
        // A console can be quiet for hours, so receives don't time out.
        // WinHTTP's WebSocket keep-alive frames (every 30 seconds by
        // default) keep the connection in use, so TCP notices when it breaks.
        DWORD infinite = INFINITE;
        WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &infinite, sizeof(infinite));
        finish(p);
        return std::make_unique<WebSocket>(ws);
    } catch (...) {
        finish(p);
        throw;
    }
}

}  // namespace vmware
