#include "vmware/socket_pair.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <format>

#include "casement/text.h"

namespace vmware {

namespace {

void close(SOCKET& s) {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}

}  // namespace

bool socket_pair(SOCKET& a, SOCKET& b, std::string& error) {
    a = b = INVALID_SOCKET;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    auto fail = [&](std::string_view what) {
        error = std::format("{}: {}", what, casement::system_message(static_cast<unsigned long>(WSAGetLastError())));
        close(listener);
        close(a);
        close(b);
        return false;
    };
    if (listener == INVALID_SOCKET) return fail("can't create a socket");
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int length = sizeof(addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) != 0)
        return fail("can't listen on the loopback interface");
    a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (a == INVALID_SOCKET) return fail("can't create a socket");
    if (connect(a, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return fail("can't connect on loopback");
    sockaddr_in peer{};
    length = sizeof(peer);
    b = accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
    if (b == INVALID_SOCKET) return fail("can't accept on loopback");
    sockaddr_in mine{};
    length = sizeof(mine);
    if (getsockname(a, reinterpret_cast<sockaddr*>(&mine), &length) != 0) return fail("can't name a socket");
    if (peer.sin_port != mine.sin_port || peer.sin_addr.s_addr != mine.sin_addr.s_addr) {
        WSASetLastError(WSAECONNREFUSED);
        return fail("another program connected to the plugin's loopback socket");
    }
    close(listener);
    // Input events are a few bytes each; don't let Nagle hold them back.
    BOOL nodelay = TRUE;
    setsockopt(a, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    setsockopt(b, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    return true;
}

bool send_all(SOCKET s, const void* data, std::size_t size) {
    auto p = static_cast<const char*>(data);
    while (size > 0) {
        int chunk = static_cast<int>(std::min<std::size_t>(size, 1 << 20));
        int n = send(s, p, chunk, 0);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace vmware
