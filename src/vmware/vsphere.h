// The vSphere API conversation: log in, find the VM, check that it runs, get
// a webmks ticket, log out. Works the same against ESXi and vCenter.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vmware/fingerprint.h"
#include "vmware/https.h"
#include "vmware/soap.h"
#include "vmware/target.h"

namespace vmware {

/// Where the console is and how to recognise its certificate.
struct ConsoleEndpoint {
    std::string host;  // IPv6 without brackets
    std::uint16_t port = 443;
    /// "/ticket/<ticket>"
    std::string path;
    /// From the ticket, and the API's certificate when the console is on the
    /// same host.
    std::vector<Fingerprint> fingerprints;
};

/// Reads a ticket's wss:// URL. Empty if it isn't one.
std::optional<ConsoleEndpoint> parse_console_url(std::string_view url);

/// The console endpoint a ticket describes. `api_host` stands in when the
/// ticket names no host (ESXi does that for itself).
ConsoleEndpoint console_endpoint(const soap::Ticket& ticket, const std::string& api_host);

/// Whether two host names (or addresses) are the same, ignoring case.
bool same_host(std::string_view a, std::string_view b);

class VSphere {
public:
    explicit VSphere(const Target& target);
    ~VSphere();

    /// RetrieveServiceContent and Login. Throws Failure: auth_failed for a
    /// wrong user name or password, failed otherwise.
    void login(std::string_view password);

    /// Finds the VM the address names and reads its power state. Throws
    /// Failure when there is none, several, or it isn't running.
    soap::VmInfo find_vm(const VmRef& vm);

    /// AcquireTicket("webmks"). Throws Failure.
    ConsoleEndpoint acquire_ticket(const soap::VmInfo& vm);

    /// Ends the API session, waiting a second at most. Errors are logged.
    void logout();

    /// Stops a call in progress on another thread, which throws Stopped,
    /// and the calls after it. Not logout(): the session should end.
    void abort();

private:
    std::string call(const std::string& envelope);
    [[noreturn]] void fault(const soap::Fault& f, std::string_view action);
    template <class T>
    T value(soap::Reply<T> reply, std::string_view action);
    std::vector<soap::VmInfo> vms_named(const std::string& name);
    std::optional<soap::VmInfo> vm_by_ref(const soap::MoRef& ref);

    Target target_;
    // https_ (set by login()) and logging_out_, for abort() from another
    // thread.
    std::mutex lock_;
    std::unique_ptr<Https> https_;
    bool logging_out_ = false;
    soap::ServiceContent content_;
    std::string version_ = "6.0";
    std::string cookie_;
    std::atomic<bool> aborted_{false};
};

}  // namespace vmware
