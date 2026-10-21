#include "vmware/vsphere.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "casement/log.h"
#include "casement/text.h"
#include "vmware/failure.h"

namespace vmware {

namespace {

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty() || text.size() > 5) return std::nullopt;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value == 0 || value > 65535) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::optional<ConsoleEndpoint> parse_console_url(std::string_view url) {
    std::string lower(url.substr(0, std::min<std::size_t>(url.size(), 6)));
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower != "wss://") return std::nullopt;
    std::string_view rest = url.substr(6);
    std::size_t slash = rest.find('/');
    std::string_view authority = rest.substr(0, slash);
    ConsoleEndpoint e;
    e.path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    if (authority.starts_with('[')) {
        std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        e.host = std::string(authority.substr(1, close - 1));
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return std::nullopt;
            auto port = parse_port(after.substr(1));
            if (!port) return std::nullopt;
            e.port = *port;
        }
    } else {
        std::size_t colon = authority.rfind(':');
        e.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            auto port = parse_port(authority.substr(colon + 1));
            if (!port) return std::nullopt;
            e.port = *port;
        }
    }
    if (e.host.empty()) return std::nullopt;
    return e;
}

bool same_host(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

ConsoleEndpoint console_endpoint(const soap::Ticket& ticket, const std::string& api_host) {
    ConsoleEndpoint e;
    std::optional<ConsoleEndpoint> from_url;
    if (!ticket.url.empty()) {
        from_url = parse_console_url(ticket.url);
        // Not the URL itself in the log: it holds the ticket.
        if (!from_url) casement::log::warn("ignoring the ticket's URL, which isn't wss://host[:port]/path");
    }
    if (from_url) {
        e = *from_url;
    } else {
        e.host = ticket.host.empty() ? api_host : ticket.host;
        if (e.host.size() > 2 && e.host.front() == '[' && e.host.back() == ']') e.host = e.host.substr(1, e.host.size() - 2);
        e.port = static_cast<std::uint16_t>(ticket.port > 0 ? ticket.port : 443);
        e.path = "/ticket/" + ticket.ticket;
    }
    if (!ticket.ssl_thumbprint.empty()) {
        if (auto f = parse_fingerprint(ticket.ssl_thumbprint, HashAlgorithm::sha1)) e.fingerprints.push_back(*f);
        else casement::log::warn("ignoring the ticket's sslThumbprint “{}”", ticket.ssl_thumbprint);
    }
    if (!ticket.ssl_certificate.empty()) {
        if (auto der = pem_certificate(ticket.ssl_certificate))
            e.fingerprints.push_back(Fingerprint{HashAlgorithm::sha256, hash(HashAlgorithm::sha256, *der)});
        else casement::log::warn("ignoring the ticket's sslCertificate");
    }
    for (const auto& [algorithm, thumbprint] : ticket.cert_thumbprints) {
        auto a = parse_hash_algorithm(algorithm);
        std::optional<Fingerprint> f;
        if (a) f = parse_fingerprint(thumbprint, *a);
        if (f) e.fingerprints.push_back(*f);
        else casement::log::warn("ignoring the ticket's {} thumbprint “{}”", algorithm, thumbprint);
    }
    return e;
}

VSphere::VSphere(const Target& target) : target_(target) {}

VSphere::~VSphere() = default;

void VSphere::abort() {
    std::lock_guard g(lock_);
    aborted_ = true;
    if (https_ && !logging_out_) https_->abort();
}

std::string VSphere::call(const std::string& envelope) {
    if (aborted_ && !logging_out_) throw Stopped();
    std::vector<std::wstring> headers{L"Content-Type: text/xml; charset=utf-8",
                                      casement::widen("SOAPAction: \"" + soap::soap_action(version_) + "\"")};
    if (!cookie_.empty()) headers.push_back(casement::widen("Cookie: " + cookie_));
    HttpResponse r;
    try {
        r = https_->request(L"POST", "/sdk", headers, envelope);
    } catch (const HttpError& e) {
        if (e.kind == HttpError::Kind::cancelled || (aborted_ && !logging_out_)) throw Stopped();
        if (e.kind == HttpError::Kind::certificate)
            throw failed(std::format("Untrusted certificate, SHA-256 {}; add it as the certificate fingerprint to "
                                     "trust it.",
                                     format_digest(e.certificate_sha256)));
        throw failed(network_text(e, "the vSphere API", target_.host, target_.port));
    }
    // Faults come with status 500 and a SOAP body; anything else that isn't
    // XML is some other web server.
    if (r.status != 200 && r.status != 500) {
        if (r.status == 404)
            throw failed(std::format("{} has no vSphere API (HTTP 404 for /sdk); is it an ESXi host or a vCenter "
                                     "Server?",
                                     host_port(target_.host, target_.port)));
        throw failed(std::format("{} answered HTTP {} instead of the vSphere API.",
                                 host_port(target_.host, target_.port), r.status));
    }
    for (const std::string& c : r.cookies) {
        if (c.starts_with("vmware_soap_session=")) cookie_ = c.substr(0, c.find(';'));
    }
    return std::move(r.body);
}

template <class T>
T VSphere::value(soap::Reply<T> reply, std::string_view action) {
    if (reply.fault) fault(*reply.fault, action);
    if (!reply.value)
        throw failed(std::format("{} sent a reply the plugin can't read while it tried to {}: {}.",
                                 host_port(target_.host, target_.port), action, reply.error));
    return std::move(*reply.value);
}

void VSphere::fault(const soap::Fault& f, std::string_view action) {
    casement::log::info("fault {} while trying to {}: {}", f.type.empty() ? "(untyped)" : f.type, action, f.message);
    if (f.type == "InvalidLogin")
        throw Failure(casement::State::auth_failed, "The user name or password is wrong.");
    if (f.type == "NoPermission") {
        std::string privilege = f.privilege.empty() ? "" : std::format(" (it needs the privilege {})", f.privilege);
        throw failed(std::format("{} may not {}{}.", target_.user, action, privilege));
    }
    if (f.type == "RestrictedVersion")
        throw failed(std::format("The server's licence doesn't allow this through the API, so the plugin can't {}. "
                                 "The free ESXi licence restricts the API.",
                                 action));
    if (f.type == "NotAuthenticated")
        throw failed(std::format("The vSphere session ended while the plugin tried to {}; try again.", action));
    std::string message = f.message.empty() ? f.type : f.message;
    while (!message.empty() && (message.back() == '.' || message.back() == ' ' || message.back() == '\n'))
        message.pop_back();
    throw failed(std::format("The server refused to {}: {}.", action, message));
}

void VSphere::login(std::string_view password) {
    Https::Options options;
    options.host = target_.host;
    options.port = target_.port;
    if (target_.thumbprint) options.trust.pinned.push_back(*target_.thumbprint);
    try {
        auto https = std::make_unique<Https>(std::move(options));
        std::lock_guard g(lock_);
        if (aborted_) throw Stopped();
        https_ = std::move(https);
    } catch (const HttpError& e) {
        throw failed(network_text(e, "the vSphere API", target_.host, target_.port));
    }
    // Every server since vSphere 6.0, the first with webmks tickets, knows
    // this version; later calls use the server's own.
    version_ = "6.0";
    content_ = value(soap::parse_service_content(call(soap::retrieve_service_content())),
                     "read the service content");
    version_ = content_.api_version;
    casement::log::info("{} ({}, API {})", content_.full_name, content_.api_type, content_.api_version);
    value(soap::parse_done(call(soap::login(content_.session_manager, target_.user, password))), "log in");
    if (cookie_.empty()) throw failed("The server accepted the login but sent no session cookie.");
    casement::log::info("logged in as {}", target_.user);
}

std::optional<soap::VmInfo> VSphere::vm_by_ref(const soap::MoRef& ref) {
    soap::Reply<soap::VmList> reply = soap::parse_vms(call(soap::retrieve_vm(content_.property_collector, ref)));
    if (reply.fault && reply.fault->type == "ManagedObjectNotFound") return std::nullopt;
    soap::VmList list = value(std::move(reply), "read the VM's state");
    for (soap::VmInfo& vm : list.vms)
        if (vm.ref == ref) return std::move(vm);
    return std::nullopt;
}

std::vector<soap::VmInfo> VSphere::vms_named(const std::string& name) {
    soap::MoRef view =
        value(soap::parse_moref(call(soap::create_container_view(content_.view_manager, content_.root_folder,
                                                                 "VirtualMachine"))),
              "list the VMs");
    std::vector<soap::VmInfo> found;
    std::size_t total = 0;
    soap::VmList page =
        value(soap::parse_vms(call(soap::retrieve_vms_in_view(content_.property_collector, view))), "list the VMs");
    for (;;) {
        total += page.vms.size();
        for (soap::VmInfo& vm : page.vms)
            if (unescape_name(vm.name) == name) found.push_back(std::move(vm));
        if (page.token.empty()) break;
        page = value(soap::parse_vms(call(soap::continue_retrieve(content_.property_collector, page.token))),
                     "list the VMs");
    }
    casement::log::debug("{} VMs, {} called {}", total, found.size(), name);
    // The view would otherwise stay until the session ends.
    soap::Reply<bool> destroyed = soap::parse_done(call(soap::destroy_view(view)));
    if (!destroyed.value) casement::log::debug("can't destroy the view: {}", destroyed.error);
    return found;
}

soap::VmInfo VSphere::find_vm(const VmRef& ref) {
    std::optional<soap::VmInfo> vm;
    switch (ref.kind) {
        case VmKind::path: {
            soap::MoRef found = value(soap::parse_moref(call(soap::find_by_inventory_path(content_.search_index,
                                                                                          escape_path(ref.value)))),
                                      "look up the inventory path");
            if (found.empty()) {
                auto segments = std::count(ref.value.begin(), ref.value.end(), '/') + 1;
                throw failed(std::format("There's no VM at “{}”.{}", ref.value,
                                         segments < 3 ? " Inventory paths look like DC1/vm/web-01." : ""));
            }
            if (found.type != "VirtualMachine") throw failed(std::format("“{}” isn't a VM.", ref.value));
            vm = vm_by_ref(found);
            if (!vm) throw failed(std::format("There's no VM at “{}”.", ref.value));
            break;
        }
        case VmKind::id: {
            vm = vm_by_ref(soap::MoRef{"VirtualMachine", ref.value});
            if (vm) break;
            // Also a name: a VM may be called "2024".
            auto named = vms_named(ref.value);
            if (named.size() > 1) throw failed(std::format("Several VMs are called {}; use the inventory path.", ref.value));
            if (named.empty()) throw failed(std::format("There's no VM with the id or name “{}”.", ref.value));
            vm = std::move(named[0]);
            break;
        }
        case VmKind::name: {
            auto named = vms_named(ref.value);
            if (named.size() > 1) throw failed(std::format("Several VMs are called {}; use the inventory path.", ref.value));
            if (named.empty()) throw failed(std::format("There's no VM called “{}”.", ref.value));
            vm = std::move(named[0]);
            break;
        }
    }
    std::string name = unescape_name(vm->name);
    casement::log::info("VM {} is {} ({})", name, vm->ref.value, vm->power_state);
    if (vm->power_state == "poweredOff")
        throw failed(std::format("{} is powered off; its console is there only while it runs.", name));
    if (vm->power_state == "suspended")
        throw failed(std::format("{} is suspended; its console is there only while it runs.", name));
    return std::move(*vm);
}

ConsoleEndpoint VSphere::acquire_ticket(const soap::VmInfo& vm) {
    std::string name = unescape_name(vm.name);
    soap::Reply<soap::Ticket> reply = soap::parse_ticket(call(soap::acquire_ticket(vm.ref, "webmks")));
    if (reply.fault) {
        const std::string& t = reply.fault->type;
        if (t == "InvalidPowerState" || t == "InvalidState")
            throw failed(std::format("{} isn't running, so it has no console.", name));
        if (t == "NotSupported" || t == "NotImplemented" || t == "MethodNotFound")
            throw failed(std::format("{} doesn't issue console tickets ({}).", host_port(target_.host, target_.port),
                                     reply.fault->message.empty() ? t : reply.fault->message));
        fault(*reply.fault, std::format("open the console of {}", name));
    }
    soap::Ticket ticket = value(std::move(reply), std::format("open the console of {}", name));
    ConsoleEndpoint e = console_endpoint(ticket, target_.host);
    // The console of an ESXi host reached directly is the API's own server.
    if (same_host(e.host, target_.host)) {
        std::vector<std::uint8_t> api = https_->certificate_sha256();
        if (!api.empty()) e.fingerprints.push_back(Fingerprint{HashAlgorithm::sha256, api});
    }
    casement::log::info("console ticket for {}: {}:{}, {} fingerprint(s)", name, e.host, e.port,
                        e.fingerprints.size());
    return e;
}

void VSphere::logout() {
    if (!https_ || cookie_.empty()) return;
    {
        std::lock_guard g(lock_);
        logging_out_ = true;
    }
    https_->set_timeouts(1000, 1000);
    try {
        soap::Reply<bool> r = soap::parse_done(call(soap::logout(content_.session_manager)));
        if (r.value) casement::log::info("logged out");
        else casement::log::warn("logout: {}", r.fault ? r.fault->message : r.error);
    } catch (const std::exception& e) {
        casement::log::warn("logout failed: {}", e.what());
    }
    cookie_.clear();
}

}  // namespace vmware
