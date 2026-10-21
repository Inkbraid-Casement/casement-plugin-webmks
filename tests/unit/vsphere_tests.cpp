// From a ticket to the console endpoint and the fingerprints it accepts.

#include "check.h"

#include "recorded.h"
#include "vmware/vsphere.h"

using namespace vmware;

TEST(reads_console_urls) {
    auto e = parse_console_url("wss://esx01.example.com/ticket/abc");
    REQUIRE(e);
    CHECK_EQ(e->host, std::string("esx01.example.com"));
    CHECK_EQ(e->port, 443);
    CHECK_EQ(e->path, std::string("/ticket/abc"));
    auto v6 = parse_console_url("WSS://[fd00::1]:9443/ticket/x");
    REQUIRE(v6);
    CHECK_EQ(v6->host, std::string("fd00::1"));
    CHECK_EQ(v6->port, 9443);
    CHECK(!parse_console_url("https://esx01/ticket/x"));
    CHECK(!parse_console_url("wss://:443/ticket/x"));
    CHECK(!parse_console_url("wss://esx01:99999/ticket/x"));
    CHECK(!parse_console_url("wss://[fd00::1/ticket/x"));
}

TEST(takes_the_endpoint_from_a_vcenter_ticket) {
    auto t = soap::parse_ticket(recorded::ticket_vcenter8);
    REQUIRE(t.value);
    ConsoleEndpoint e = console_endpoint(*t.value, "vcenter.example.com");
    CHECK_EQ(e.host, std::string("127.0.0.2"));
    CHECK_EQ(e.port, 10502);
    CHECK_EQ(e.path, std::string("/ticket/7ccc0651062d610e44b21d885ae0717c"));
    REQUIRE(e.fingerprints.size() == 2);
    CHECK(e.fingerprints[0].algorithm == HashAlgorithm::sha1);
    CHECK(e.fingerprints[1].algorithm == HashAlgorithm::sha256);
}

TEST(takes_the_certificate_from_a_vsphere9_ticket) {
    auto t = soap::parse_ticket(recorded::ticket_vsphere9);
    REQUIRE(t.value);
    ConsoleEndpoint e = console_endpoint(*t.value, "vcenter.example.com");
    CHECK_EQ(e.path, std::string("/ticket/5d43861f079b56ea9baa100bd314fb8f"));
    REQUIRE(e.fingerprints.size() == 1);
    CHECK_EQ(format_digest(e.fingerprints[0].digest),
             std::string("A5:11:AF:F9:3B:F8:FE:01:FC:89:E4:0A:47:37:F9:08:91:E7:7E:AF:14:CD:01:E5:1D:B7:98:BE:74:FB:6C:0C"));
}

TEST(falls_back_to_the_api_host) {
    soap::Ticket t;
    t.ticket = "52-abc";
    t.ssl_thumbprint = "not hex";
    t.cert_thumbprints = {{"sha256", "00:11"}, {"md5", "00"}};
    ConsoleEndpoint e = console_endpoint(t, "esx01.example.com");
    CHECK_EQ(e.host, std::string("esx01.example.com"));
    CHECK_EQ(e.port, 443);
    CHECK_EQ(e.path, std::string("/ticket/52-abc"));
    CHECK(e.fingerprints.empty());
    t.host = "[fd00::9]";
    t.port = 902;
    ConsoleEndpoint v6 = console_endpoint(t, "x");
    CHECK_EQ(v6.host, std::string("fd00::9"));
    CHECK_EQ(v6.port, 902);
}

TEST(compares_hosts) {
    CHECK(same_host("ESX01.example.com", "esx01.EXAMPLE.com"));
    CHECK(!same_host("esx01", "esx02"));
    CHECK(!same_host("esx01", "esx01.example.com"));
}
