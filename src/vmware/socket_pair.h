// Two connected TCP sockets on the loopback interface, Windows' stand-in for
// socketpair(). LibVNCClient reads and writes a socket; the console bridges
// the other end to the WebSocket.

#pragma once

#include <winsock2.h>

#include <string>

namespace vmware {

/// Connects `a` and `b`. The listening socket lives only until its single
/// connection is accepted, and the accepted peer must be `a`, so no other
/// local program can slip in. On failure both are INVALID_SOCKET and
/// `error` says why.
bool socket_pair(SOCKET& a, SOCKET& b, std::string& error);

/// Sends all of `data`; false if the socket fails.
bool send_all(SOCKET s, const void* data, std::size_t size);

}  // namespace vmware
