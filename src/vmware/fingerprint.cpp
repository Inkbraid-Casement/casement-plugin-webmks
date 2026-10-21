#include "vmware/fingerprint.h"

#include <windows.h>

#include <bcrypt.h>

#include <cctype>

#include "casement/text.h"

namespace vmware {

std::size_t digest_size(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::sha1: return 20;
        case HashAlgorithm::sha256: return 32;
        case HashAlgorithm::sha384: return 48;
        case HashAlgorithm::sha512: return 64;
    }
    return 0;
}

std::optional<HashAlgorithm> parse_hash_algorithm(std::string_view name) {
    std::string s;
    for (char c : name)
        if (c != '-' && c != '_' && c != ' ') s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "sha1") return HashAlgorithm::sha1;
    if (s == "sha256") return HashAlgorithm::sha256;
    if (s == "sha384") return HashAlgorithm::sha384;
    if (s == "sha512") return HashAlgorithm::sha512;
    return std::nullopt;
}

std::string_view to_string(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::sha1: return "SHA-1";
        case HashAlgorithm::sha256: return "SHA-256";
        case HashAlgorithm::sha384: return "SHA-384";
        case HashAlgorithm::sha512: return "SHA-512";
    }
    return "?";
}

std::optional<std::vector<std::uint8_t>> parse_hex_digest(std::string_view text) {
    std::vector<std::uint8_t> out;
    int high = -1;
    for (char c : text) {
        if (c == ':' || c == ' ' || c == '-') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return std::nullopt;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<std::uint8_t>(high << 4 | v));
            high = -1;
        }
    }
    if (high >= 0 || out.empty()) return std::nullopt;
    return out;
}

std::optional<Fingerprint> parse_fingerprint(std::string_view text, HashAlgorithm algorithm) {
    auto digest = parse_hex_digest(text);
    if (!digest || digest->size() != digest_size(algorithm)) return std::nullopt;
    return Fingerprint{algorithm, std::move(*digest)};
}

std::string format_digest(std::span<const std::uint8_t> digest) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        if (i > 0) out += ':';
        out += digits[digest[i] >> 4];
        out += digits[digest[i] & 15];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> pem_certificate(std::string_view pem) {
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    std::size_t b = pem.find(begin);
    if (b == std::string_view::npos) return std::nullopt;
    b += begin.size();
    std::size_t e = pem.find(end, b);
    if (e == std::string_view::npos) return std::nullopt;
    std::string base64;
    for (char c : pem.substr(b, e - b))
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t') base64 += c;
    auto der = casement::base64_decode(base64);
    if (!der || der->empty()) return std::nullopt;
    return der;
}

std::vector<std::uint8_t> hash(HashAlgorithm algorithm, std::span<const std::uint8_t> data) {
    const wchar_t* id = BCRYPT_SHA256_ALGORITHM;
    switch (algorithm) {
        case HashAlgorithm::sha1: id = BCRYPT_SHA1_ALGORITHM; break;
        case HashAlgorithm::sha256: id = BCRYPT_SHA256_ALGORITHM; break;
        case HashAlgorithm::sha384: id = BCRYPT_SHA384_ALGORITHM; break;
        case HashAlgorithm::sha512: id = BCRYPT_SHA512_ALGORITHM; break;
    }
    std::vector<std::uint8_t> digest(digest_size(algorithm));
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, id, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(h, digest.data(), static_cast<ULONG>(digest.size()), 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) digest.clear();
    return digest;
}

}  // namespace vmware
