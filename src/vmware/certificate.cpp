#include "vmware/certificate.h"

#include <winhttp.h>
#include <ws2tcpip.h>

#include <cstring>
#include <format>

#include "casement/text.h"

namespace vmware {

namespace {

// Whether `host` is an IP address that the certificate's subject
// alternative names list.
bool names_address(PCCERT_CONTEXT cert, std::wstring_view host) {
    std::wstring h(host);
    unsigned char address[16];
    DWORD length = 0;
    if (InetPtonW(AF_INET, h.c_str(), address) == 1) length = 4;
    else if (InetPtonW(AF_INET6, h.c_str(), address) == 1) length = 16;
    else return false;
    PCERT_EXTENSION ext =
        CertFindExtension(szOID_SUBJECT_ALT_NAME2, cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
    if (!ext) return false;
    CERT_ALT_NAME_INFO* names = nullptr;
    DWORD size = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, ext->Value.pbData, ext->Value.cbData,
                             CRYPT_DECODE_ALLOC_FLAG, nullptr, &names, &size))
        return false;
    bool found = false;
    for (DWORD i = 0; i < names->cAltEntry && !found; ++i) {
        const CERT_ALT_NAME_ENTRY& e = names->rgAltEntry[i];
        found = e.dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS && e.IPAddress.cbData == length &&
                std::memcmp(e.IPAddress.pbData, address, length) == 0;
    }
    LocalFree(names);
    return found;
}

}  // namespace

bool windows_trusts(PCCERT_CONTEXT cert, std::wstring_view host, std::string& error) {
    LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH), const_cast<LPSTR>(szOID_SERVER_GATED_CRYPTO),
                      const_cast<LPSTR>(szOID_SGC_NETSCAPE)};
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof(para);
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    para.RequestedUsage.Usage.cUsageIdentifier = static_cast<DWORD>(std::size(usages));
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    // The certificate's own store holds the intermediates the server sent.
    if (!CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &para, 0, nullptr, &chain)) {
        error = casement::system_message(GetLastError());
        return false;
    }
    // The chain itself first: not every SSL policy implementation rejects a
    // chain that doesn't reach a root (Wine's doesn't). Revocation isn't
    // checked, so its "unknown" bits don't count.
    DWORD problems = chain->TrustStatus.dwErrorStatus &
                     ~static_cast<DWORD>(CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION |
                                         CERT_TRUST_IS_NOT_TIME_NESTED);
    if (problems != CERT_TRUST_NO_ERROR) {
        CertFreeCertificateChain(chain);
        if (problems & (CERT_TRUST_IS_PARTIAL_CHAIN | CERT_TRUST_IS_UNTRUSTED_ROOT))
            error = std::format("no trusted root certificate authority issued it ({:#x})", problems);
        else if (problems & CERT_TRUST_IS_NOT_TIME_VALID)
            error = "it has expired or isn't valid yet";
        else
            error = std::format("its certificate chain has errors ({:#x})", problems);
        return false;
    }
    std::wstring name(host);
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = name.data();
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    BOOL ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);
    if (ok && status.dwError == static_cast<DWORD>(CERT_E_CN_NO_MATCH) && names_address(cert, host)) {
        // The SSL policy compares host names, not always addresses: with the
        // address in the certificate, check the chain alone.
        ssl.fdwChecks = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        status = {};
        status.cbSize = sizeof(status);
        ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);
    }
    CertFreeCertificateChain(chain);
    if (!ok) {
        error = casement::system_message(GetLastError());
        return false;
    }
    if (status.dwError != 0) {
        error = casement::system_message(status.dwError);
        return false;
    }
    return true;
}

bool matches(std::span<const std::uint8_t> der, const std::vector<Fingerprint>& pinned) {
    for (const Fingerprint& f : pinned) {
        std::vector<std::uint8_t> digest = hash(f.algorithm, der);
        if (!digest.empty() && digest == f.digest) return true;
    }
    return false;
}

Verdict check_certificate(PCCERT_CONTEXT cert, std::wstring_view host, const Trust& trust) {
    Verdict v;
    std::span<const std::uint8_t> der(cert->pbCertEncoded, cert->cbCertEncoded);
    v.sha256 = hash(HashAlgorithm::sha256, der);
    if (windows_trusts(cert, host, v.windows_error)) {
        v.trusted = true;
        v.by_windows = true;
    } else if (matches(der, trust.pinned)) {
        v.trusted = true;
    }
    return v;
}

}  // namespace vmware
