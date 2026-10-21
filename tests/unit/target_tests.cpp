// Addresses, the VM part's classification, and vSphere's name escaping.

#include "check.h"

#include "vmware/target.h"

using namespace vmware;

TEST(classifies_vms) {
    auto path = classify_vm("DC1/vm/web-01");
    CHECK(path.kind == VmKind::path);
    CHECK_EQ(path.value, std::string("DC1/vm/web-01"));
    auto slashes = classify_vm("/DC1/vm/Linux/web-01/");
    CHECK(slashes.kind == VmKind::path);
    CHECK_EQ(slashes.value, std::string("DC1/vm/Linux/web-01"));
    CHECK(classify_vm("vm-123").kind == VmKind::id);
    CHECK(classify_vm("42").kind == VmKind::id);
    CHECK(classify_vm("web-01").kind == VmKind::name);
    CHECK(classify_vm("vm-").kind == VmKind::name);
    CHECK(classify_vm("vm-12a").kind == VmKind::name);
    CHECK(classify_vm("VM-12").kind == VmKind::name);
    CHECK(classify_vm("vm 12").kind == VmKind::name);
}

TEST(parses_the_spec_example) {
    std::string error;
    auto t = parse_target("vmware://ann%40corp.local@vcenter.example.com/DC1/vm/web-01", "", error);
    REQUIRE(t);
    CHECK_EQ(t->user, std::string("ann@corp.local"));
    CHECK_EQ(t->host, std::string("vcenter.example.com"));
    CHECK_EQ(t->port, 443);
    CHECK(t->vm.kind == VmKind::path);
    CHECK_EQ(t->vm.value, std::string("DC1/vm/web-01"));
    CHECK(!t->thumbprint);
    CHECK(t->keys == KeyMode::automatic);
}

TEST(parses_all_fields) {
    std::string error;
    auto t = parse_target(
        "vmware://root@[fd00::7]:8443/vm-77?thumbprint=ab%3Acd%3Aef%3A01%3A23%3A45%3A67%3A89%3Aab%3Acd%3Aef%3A01%3A23%3A"
        "45%3A67%3A89%3Aab%3Acd%3Aef%3A01%3A23%3A45%3A67%3A89%3Aab%3Acd%3Aef%3A01%3A23%3A45%3A67%3A89&keys=keysyms",
        "", error);
    REQUIRE(t);
    CHECK_EQ(t->host, std::string("fd00::7"));
    CHECK(t->ipv6);
    CHECK_EQ(t->port, 8443);
    CHECK(t->vm.kind == VmKind::id);
    REQUIRE(t->thumbprint);
    CHECK(t->thumbprint->algorithm == HashAlgorithm::sha256);
    CHECK_EQ(t->thumbprint->digest.size(), 32u);
    CHECK_EQ(t->thumbprint->digest[0], 0xab);
    CHECK(t->keys == KeyMode::keysyms);
}

TEST(prefers_the_connect_username) {
    std::string error;
    auto t = parse_target("vmware://ann@esx01/web-01", "root", error);
    REQUIRE(t);
    CHECK_EQ(t->user, std::string("root"));
    CHECK(t->vm.kind == VmKind::name);
}

TEST(rejects_bad_addresses) {
    std::string error;
    CHECK(!parse_target("vmware://esx01/web-01", "", error));
    CHECK_EQ(error, std::string("The address has no user name."));
    CHECK(!parse_target("vmware://root@esx01", "", error));
    CHECK_EQ(error, std::string("The address names no VM."));
    CHECK(!parse_target("vnc://root@esx01/web-01", "", error));
    CHECK(!parse_target("vmware://root@esx01/web-01?keys=scancodes", "", error));
    CHECK_EQ(error, std::string("“scancodes” isn't a keyboard setting; use auto or keysyms."));
    CHECK(!parse_target("vmware://root@esx01/web-01?thumbprint=xyz", "", error));
    CHECK(error.starts_with("“xyz” isn't a certificate fingerprint"));
    // A SHA-1 thumbprint, as older vSphere screens show them.
    CHECK(!parse_target("vmware://root@esx01/web-01?thumbprint=82:9E:D1:4E:D8:60:15:AF:D7:00:82:37:C4:62:4D:C6:6B:30:96:03",
                        "", error));
    CHECK_EQ(error, std::string("The certificate fingerprint has 20 bytes; it should be the SHA-256, which has 32."));
    CHECK(!parse_target("vmware://root@esx01:0/web-01", "", error));
}

TEST(handles_escaped_names) {
    CHECK_EQ(unescape_name("100%25"), std::string("100%"));
    CHECK_EQ(unescape_name("a%2fb%2Fc%5cd%5C"), std::string("a/b/c\\d\\"));
    CHECK_EQ(unescape_name("50%"), std::string("50%"));
    CHECK_EQ(unescape_name("%41"), std::string("%41"));
    CHECK_EQ(escape_path_segment("100%"), std::string("100%25"));
    CHECK_EQ(escape_path_segment("a\\b"), std::string("a%5cb"));
    CHECK_EQ(escape_path("DC1/vm/100%/x"), std::string("DC1/vm/100%25/x"));
    CHECK_EQ(escape_path("plain"), std::string("plain"));
}
