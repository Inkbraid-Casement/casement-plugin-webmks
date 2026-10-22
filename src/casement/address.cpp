#include "casement/address.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace casement {

namespace {

bool valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        i += n;
    }
    return true;
}

bool unreserved(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

std::optional<std::string> percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c != '%') {
            out += c;
            continue;
        }
        auto hexval = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return -1;
        };
        if (i + 2 >= text.size()) return std::nullopt;
        int hi = hexval(text[i + 1]), lo = hexval(text[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out += static_cast<char>(hi << 4 | lo);
        i += 2;
    }
    if (!valid_utf8(out)) return std::nullopt;
    return out;
}

std::string percent_encode(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (char c : text) {
        if (unreserved(c)) {
            out += c;
        } else {
            auto u = static_cast<unsigned char>(c);
            out += '%';
            out += digits[u >> 4];
            out += digits[u & 15];
        }
    }
    return out;
}

const std::string* Address::param(std::string_view name) const {
    for (const auto& [k, v] : query)
        if (k == name) return &v;
    return nullptr;
}

bool Address::flag(std::string_view name) const {
    const std::string* v = param(name);
    if (!v) return false;
    std::string s = lower(*v);
    return s.empty() || s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string Address::url_host() const { return ipv6 ? "[" + host + "]" : host; }

std::optional<Address> parse_address(std::string_view text, std::string& error) {
    auto bad = [&](std::string_view why) {
        error = std::string(why);
        return std::nullopt;
    };
    auto sep = text.find("://");
    if (sep == std::string_view::npos || sep == 0) return bad(std::format("“{}” isn't an address.", text));
    Address a;
    a.scheme = lower(text.substr(0, sep));
    std::string_view rest = text.substr(sep + 3);

    std::string_view query;
    if (auto q = rest.find('?'); q != std::string_view::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }
    if (auto h = rest.find('#'); h != std::string_view::npos) rest = rest.substr(0, h);
    std::string_view authority = rest, path;
    if (auto s = rest.find('/'); s != std::string_view::npos) {
        authority = rest.substr(0, s);
        path = rest.substr(s + 1);
    }
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        auto user = percent_decode(authority.substr(0, at));
        if (!user) return bad("The user name in the address isn't valid.");
        a.user = *user;
        authority = authority.substr(at + 1);
    }
    std::string_view port;
    if (authority.starts_with('[')) {
        auto close = authority.find(']');
        if (close == std::string_view::npos) return bad("The IPv6 address in the address has no closing ].");
        a.host = std::string(authority.substr(1, close - 1));
        a.ipv6 = true;
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return bad("The address has something unexpected after the host.");
            port = after.substr(1);
        }
    } else {
        if (auto c = authority.rfind(':'); c != std::string_view::npos) {
            port = authority.substr(c + 1);
            authority = authority.substr(0, c);
        }
        auto host = percent_decode(authority);
        if (!host) return bad("The host in the address isn't valid.");
        a.host = *host;
    }
    if (a.host.empty()) return bad("The address has no host.");
    if (a.host.find_first_of(" /?#@") != std::string::npos)
        return bad(std::format("“{}” isn't a host name.", a.host));
    if (!port.empty()) {
        unsigned value = 0;
        for (char c : port) {
            if (c < '0' || c > '9' || value > 65535) return bad(std::format("“{}” isn't a port.", port));
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value == 0 || value > 65535) return bad(std::format("“{}” isn't a port.", port));
        a.port = static_cast<std::uint16_t>(value);
    }
    if (!path.empty()) {
        auto p = percent_decode(path);
        if (!p) return bad("The path in the address isn't valid.");
        a.path = *p;
        while (!a.path.empty() && a.path.back() == '/') a.path.pop_back();
    }
    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        if (pair.empty()) continue;
        auto eq = pair.find('=');
        auto k = percent_decode(pair.substr(0, eq));
        auto v = percent_decode(eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1));
        if (!k || !v) return bad(std::format("“{}” in the address isn't valid.", pair));
        a.query.emplace_back(lower(*k), *v);
    }
    return a;
}

}  // namespace casement
