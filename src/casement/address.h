// Connection addresses as the protocol puts them together:
// <scheme>://[<user>@]<host>[:<port>][/<path>][?<query>], values
// percent-encoded (RFC 3986), IPv6 hosts in square brackets, query fields
// as name=value (flags as a bare name) joined by &.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace casement {

struct Address {
    std::string scheme;  // lower case
    std::string user;
    std::string host;  // without IPv6 brackets
    bool ipv6 = false;
    std::optional<std::uint16_t> port;
    std::string path;  // without the leading '/', percent-decoded
    std::vector<std::pair<std::string, std::string>> query;  // in order, decoded

    /// A query value, or nullptr when the parameter is absent.
    const std::string* param(std::string_view name) const;
    /// A query flag: present without a value, or with 1, true, yes or on.
    bool flag(std::string_view name) const;
    /// The host as it goes into a URL: IPv6 in brackets.
    std::string url_host() const;
};

/// Parses an address. On failure the error is a sentence for the user.
std::optional<Address> parse_address(std::string_view text, std::string& error);

/// Percent-decodes. Empty optional for a broken escape or invalid UTF-8.
std::optional<std::string> percent_decode(std::string_view text);

/// Percent-encodes everything but RFC 3986 unreserved characters.
std::string percent_encode(std::string_view text);

}  // namespace casement
