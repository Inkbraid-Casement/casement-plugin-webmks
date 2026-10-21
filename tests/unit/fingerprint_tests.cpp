// Fingerprints: parsing, formatting, hashing with Windows CNG, PEM, and the
// certificate check that combines Windows' trust with pinned fingerprints.

#include "check.h"

#include <windows.h>

#include <wincrypt.h>

#include "casement/text.h"
#include "recorded.h"
#include "vmware/certificate.h"
#include "vmware/fingerprint.h"
#include "vmware/soap.h"

using namespace vmware;

namespace {

// The console certificate in recorded::ticket_vsphere9; recorded::
// ticket_vcenter8 (same fake-esxi run) gives its SHA-1 and SHA-256.
constexpr std::string_view console_sha1 = "82:9E:D1:4E:D8:60:15:AF:D7:00:82:37:C4:62:4D:C6:6B:30:96:03";
constexpr std::string_view console_sha256 =
    "A5:11:AF:F9:3B:F8:FE:01:FC:89:E4:0A:47:37:F9:08:91:E7:7E:AF:14:CD:01:E5:1D:B7:98:BE:74:FB:6C:0C";

std::vector<std::uint8_t> console_der() {
    auto ticket = soap::parse_ticket(recorded::ticket_vsphere9);
    if (!ticket.value) return {};
    return pem_certificate(ticket.value->ssl_certificate).value_or(std::vector<std::uint8_t>{});
}

}  // namespace

TEST(parses_and_formats_fingerprints) {
    auto d = parse_hex_digest("ab:CD 01-ef");
    REQUIRE(d);
    CHECK(*d == (std::vector<std::uint8_t>{0xab, 0xcd, 0x01, 0xef}));
    CHECK_EQ(format_digest(*d), std::string("AB:CD:01:EF"));
    CHECK(!parse_hex_digest("abc"));
    CHECK(!parse_hex_digest(""));
    CHECK(!parse_hex_digest("zz"));
    auto f = parse_fingerprint(console_sha1, HashAlgorithm::sha1);
    REQUIRE(f);
    CHECK_EQ(format_digest(f->digest), std::string(console_sha1));
    CHECK(!parse_fingerprint(console_sha1, HashAlgorithm::sha256));
    CHECK(parse_fingerprint(console_sha256, HashAlgorithm::sha256));
}

TEST(names_hash_algorithms) {
    CHECK(parse_hash_algorithm("sha256") == HashAlgorithm::sha256);
    CHECK(parse_hash_algorithm("SHA-256") == HashAlgorithm::sha256);
    CHECK(parse_hash_algorithm("sha1") == HashAlgorithm::sha1);
    CHECK(parse_hash_algorithm("sha512") == HashAlgorithm::sha512);
    CHECK(!parse_hash_algorithm("md5"));
    CHECK_EQ(digest_size(HashAlgorithm::sha384), 48u);
    CHECK_EQ(std::string(to_string(HashAlgorithm::sha256)), std::string("SHA-256"));
}

TEST(hashes_with_cng) {
    std::string abc = "abc";
    std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size());
    CHECK_EQ(format_digest(hash(HashAlgorithm::sha1, data)),
             std::string("A9:99:3E:36:47:06:81:6A:BA:3E:25:71:78:50:C2:6C:9C:D0:D8:9D"));
    CHECK_EQ(casement::hex(hash(HashAlgorithm::sha256, data)),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hash(HashAlgorithm::sha512, data).size(), 64u);
}

TEST(reads_pem_certificates) {
    std::vector<std::uint8_t> der = console_der();
    REQUIRE(!der.empty());
    CHECK_EQ(der[0], 0x30);  // a DER SEQUENCE
    CHECK_EQ(format_digest(hash(HashAlgorithm::sha256, der)), std::string(console_sha256));
    CHECK_EQ(format_digest(hash(HashAlgorithm::sha1, der)), std::string(console_sha1));
    CHECK(!pem_certificate("no certificate here"));
    CHECK(!pem_certificate("-----BEGIN CERTIFICATE-----\n!!!\n-----END CERTIFICATE-----"));
}

TEST(checks_certificates) {
    std::vector<std::uint8_t> der = console_der();
    REQUIRE(!der.empty());
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
    REQUIRE(cert);
    // Self-signed by fake-esxi: Windows doesn't trust it.
    std::string why;
    CHECK(!windows_trusts(cert, L"esx01.test", why));
    CHECK(!why.empty());
    Verdict none = check_certificate(cert, L"esx01.test", Trust{});
    CHECK(!none.trusted);
    CHECK_EQ(format_digest(none.sha256), std::string(console_sha256));
    Trust sha1{{*parse_fingerprint(console_sha1, HashAlgorithm::sha1)}};
    CHECK(check_certificate(cert, L"esx01.test", sha1).trusted);
    Trust sha256{{*parse_fingerprint(console_sha256, HashAlgorithm::sha256)}};
    Verdict pinned = check_certificate(cert, L"127.0.0.2", sha256);
    CHECK(pinned.trusted);
    CHECK(!pinned.by_windows);
    Trust other{{*parse_fingerprint(
        "00:11:AF:F9:3B:F8:FE:01:FC:89:E4:0A:47:37:F9:08:91:E7:7E:AF:14:CD:01:E5:1D:B7:98:BE:74:FB:6C:0C",
        HashAlgorithm::sha256)}};
    CHECK(!check_certificate(cert, L"esx01.test", other).trusted);
    CertFreeCertificateContext(cert);
}
