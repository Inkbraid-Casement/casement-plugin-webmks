// The loopback socket pair between LibVNCClient and the WebSocket.

#include "check.h"

#include <winsock2.h>

#include <string>

#include "vmware/socket_pair.h"

using namespace vmware;

TEST(connects_a_socket_pair) {
    WSADATA data;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    SOCKET a, b;
    std::string error;
    REQUIRE(socket_pair(a, b, error));
    std::string big(300000, 'x');
    big[0] = 'a';
    big.back() = 'z';
    CHECK(send_all(a, big.data(), 1000));
    char buf[1000];
    int got = 0;
    while (got < 1000) {
        int n = recv(b, buf + got, static_cast<int>(sizeof(buf)) - got, 0);
        if (n <= 0) break;
        got += n;
    }
    CHECK_EQ(got, 1000);
    CHECK_EQ(buf[0], 'a');
    CHECK(send_all(b, "hi", 2));
    char back[2] = {};
    CHECK_EQ(recv(a, back, 2, MSG_WAITALL), 2);
    CHECK(back[0] == 'h' && back[1] == 'i');
    // Shutting one end down ends the other's stream.
    shutdown(b, SD_BOTH);
    CHECK_EQ(recv(a, back, 1, 0), 0);
    closesocket(a);
    closesocket(b);
    WSACleanup();
}
