#include "vmware/target.h"

#include <format>

#include "casement/address.h"

namespace vmware {

namespace {

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

VmRef classify_vm(std::string_view text) {
    if (text.find('/') != std::string_view::npos) {
        while (!text.empty() && text.front() == '/') text.remove_prefix(1);
        while (!text.empty() && text.back() == '/') text.remove_suffix(1);
        return {VmKind::path, std::string(text)};
    }
    if (all_digits(text) || (text.starts_with("vm-") && all_digits(text.substr(3))))
        return {VmKind::id, std::string(text)};
    return {VmKind::name, std::string(text)};
}

std::optional<Target> parse_target(std::string_view address, std::string_view username, std::string& error) {
    auto a = casement::parse_address(address, error);
    if (!a) return std::nullopt;
    if (a->scheme != "vmware") {
        error = std::format("“{}” isn't a vmware:// address.", address);
        return std::nullopt;
    }
    Target t;
    t.host = a->host;
    t.ipv6 = a->ipv6;
    t.port = a->port.value_or(443);
    t.user = username.empty() ? a->user : std::string(username);
    if (t.user.empty()) {
        error = "The address has no user name.";
        return std::nullopt;
    }
    t.vm = classify_vm(a->path);
    if (t.vm.value.empty()) {
        error = "The address names no VM.";
        return std::nullopt;
    }
    if (const std::string* p = a->param("thumbprint"); p && !p->empty()) {
        auto digest = parse_hex_digest(*p);
        if (!digest) {
            error = std::format("“{}” isn't a certificate fingerprint; it should be the SHA-256 as hex digits, "
                                "for example AB:CD:….", *p);
            return std::nullopt;
        }
        if (digest->size() != digest_size(HashAlgorithm::sha256)) {
            error = std::format("The certificate fingerprint has {} bytes; it should be the SHA-256, which has 32.",
                                digest->size());
            return std::nullopt;
        }
        t.thumbprint = Fingerprint{HashAlgorithm::sha256, std::move(*digest)};
    }
    if (const std::string* k = a->param("keys"); k && !k->empty()) {
        if (*k == "auto") {
            t.keys = KeyMode::automatic;
        } else if (*k == "keysyms") {
            t.keys = KeyMode::keysyms;
        } else {
            error = std::format("“{}” isn't a keyboard setting; use auto or keysyms.", *k);
            return std::nullopt;
        }
    }
    return t;
}

std::string unescape_name(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size()) {
            std::string_view code = name.substr(i + 1, 2);
            char c = 0;
            if (code == "25") c = '%';
            else if (code == "2f" || code == "2F") c = '/';
            else if (code == "5c" || code == "5C") c = '\\';
            if (c) {
                out += c;
                i += 2;
                continue;
            }
        }
        out += name[i];
    }
    return out;
}

std::string escape_path_segment(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (c == '%') out += "%25";
        else if (c == '\\') out += "%5c";
        else out += c;
    }
    return out;
}

std::string escape_path(std::string_view path) {
    std::string out;
    std::size_t start = 0;
    for (;;) {
        std::size_t slash = path.find('/', start);
        out += escape_path_segment(path.substr(start, slash - start));
        if (slash == std::string_view::npos) break;
        out += '/';
        start = slash + 1;
    }
    return out;
}

}  // namespace vmware
