// Which TLS certificates the plugin accepts: one Windows trusts for the host
// name (its certificate store and the SSL policy), or one whose fingerprint
// the connection pins.

#pragma once

#include <windows.h>

#include <wincrypt.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vmware/fingerprint.h"

namespace vmware {

struct Trust {
    /// Accepted by fingerprint, besides certificates Windows trusts.
    std::vector<Fingerprint> pinned;
};

struct Verdict {
    bool trusted = false;
    bool by_windows = false;
    std::vector<std::uint8_t> sha256;
    /// Why Windows doesn't trust it, for the log.
    std::string windows_error;
};

/// Whether Windows trusts `cert` for `host` (an SSL server certificate with
/// a valid chain to a trusted root that names the host). Sets `error`
/// otherwise.
bool windows_trusts(PCCERT_CONTEXT cert, std::wstring_view host, std::string& error);

/// Whether a DER certificate matches one of the fingerprints.
bool matches(std::span<const std::uint8_t> der, const std::vector<Fingerprint>& pinned);

Verdict check_certificate(PCCERT_CONTEXT cert, std::wstring_view host, const Trust& trust);

}  // namespace vmware
