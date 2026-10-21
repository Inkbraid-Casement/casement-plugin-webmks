// Certificate fingerprints: the hash of a certificate's DER encoding, written
// as hex bytes separated by colons ("AB:CD:…"), the way vSphere shows them.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vmware {

enum class HashAlgorithm { sha1, sha256, sha384, sha512 };

/// The algorithm's digest size in bytes.
std::size_t digest_size(HashAlgorithm algorithm);

/// The algorithm a vSphere `hashAlgorithm` value names ("sha256", "SHA-256",
/// …); empty for anything else.
std::optional<HashAlgorithm> parse_hash_algorithm(std::string_view name);

/// The name used in messages: "SHA-1", "SHA-256", …
std::string_view to_string(HashAlgorithm algorithm);

struct Fingerprint {
    HashAlgorithm algorithm = HashAlgorithm::sha256;
    std::vector<std::uint8_t> digest;

    bool operator==(const Fingerprint&) const = default;
};

/// Parses hex digits, optionally separated by colons, spaces or dashes, in
/// either case. Empty when anything else appears or the digits are odd.
std::optional<std::vector<std::uint8_t>> parse_hex_digest(std::string_view text);

/// Parses a fingerprint of `algorithm`; empty if the text isn't hex of
/// exactly that algorithm's size.
std::optional<Fingerprint> parse_fingerprint(std::string_view text, HashAlgorithm algorithm);

/// Upper-case hex with colons: "AB:CD:…".
std::string format_digest(std::span<const std::uint8_t> digest);

/// The DER encoding of the first certificate in PEM text. Empty if there
/// is none.
std::optional<std::vector<std::uint8_t>> pem_certificate(std::string_view pem);

/// The digest of `data` (Windows CNG). Empty if hashing fails.
std::vector<std::uint8_t> hash(HashAlgorithm algorithm, std::span<const std::uint8_t> data);

}  // namespace vmware
