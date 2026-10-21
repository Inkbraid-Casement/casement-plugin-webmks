// What a vmware:// address asks for:
//
//   vmware://<user>@<host>[:<port>]/<vm>[?thumbprint=<SHA-256>][&keys=auto|keysyms]
//
// The VM is an inventory path if it contains '/', a managed object id if it
// looks like vm-123 (vCenter) or a number (ESXi), and a VM name otherwise.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "vmware/fingerprint.h"

namespace vmware {

enum class VmKind { path, id, name };

struct VmRef {
    VmKind kind = VmKind::name;
    /// For a path: without leading and trailing slashes.
    std::string value;
};

/// Classifies the address's VM part.
VmRef classify_vm(std::string_view text);

/// How keys reach the VM: XT scancodes when the server takes QEMU extended
/// key events, or keysyms of a US layout.
enum class KeyMode { automatic, keysyms };

struct Target {
    std::string host;  // without IPv6 brackets
    bool ipv6 = false;
    std::uint16_t port = 443;
    std::string user;
    VmRef vm;
    /// The API endpoint's certificate, when the user pinned it.
    std::optional<Fingerprint> thumbprint;
    KeyMode keys = KeyMode::automatic;
};

/// Parses an address. `username` (from `connect`) wins over the address's
/// user part when both are set. On failure, `error` is a sentence for the
/// user.
std::optional<Target> parse_target(std::string_view address, std::string_view username, std::string& error);

/// A VM name as vSphere stores it in `name`, with '%', '/' and '\' escaped
/// as %25, %2f and %5c, back to how the user sees it.
std::string unescape_name(std::string_view name);

/// One segment of an inventory path the way vSphere escapes names: '%' and
/// '\' as %25 and %5c ('/' can't be in a segment).
std::string escape_path_segment(std::string_view name);

/// An inventory path with every segment escaped.
std::string escape_path(std::string_view path);

}  // namespace vmware
