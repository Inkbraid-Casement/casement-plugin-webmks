// The vSphere Web Services (SOAP) messages the plugin uses: building request
// envelopes and reading responses and faults. No I/O here; vsphere.cpp sends
// them.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vmware::soap {

/// A managed object reference: <obj type="VirtualMachine">vm-42</obj>.
struct MoRef {
    std::string type;
    std::string value;

    bool empty() const { return value.empty(); }
    bool operator==(const MoRef&) const = default;
};

/// A SOAP fault. `type` is the vSphere fault type ("InvalidLogin",
/// "NoPermission", …) or empty when the reply names none.
struct Fault {
    std::string type;
    std::string message;
    /// NoPermission: the missing privilege.
    std::string privilege;
    /// NoPermission, ManagedObjectNotFound: the object concerned.
    MoRef object;
};

/// A reply read from the server: the value, a fault, or (neither) an
/// explanation of why the reply isn't one.
template <class T>
struct Reply {
    std::optional<T> value;
    std::optional<Fault> fault;
    std::string error;
};

struct ServiceContent {
    MoRef root_folder;
    MoRef property_collector;
    MoRef view_manager;
    MoRef session_manager;
    MoRef search_index;
    /// "VirtualCenter" or "HostAgent".
    std::string api_type;
    /// For example "8.0.3.0"; later requests ask for this version.
    std::string api_version;
    std::string full_name;
};

struct VmInfo {
    MoRef ref;
    /// As vSphere stores it, with '%', '/' and '\' escaped.
    std::string name;
    /// "poweredOn", "poweredOff" or "suspended".
    std::string power_state;
};

struct VmList {
    std::vector<VmInfo> vms;
    /// Set when more results wait for ContinueRetrievePropertiesEx.
    std::string token;
};

struct Ticket {
    std::string ticket;
    std::string host;
    int port = 0;
    std::string url;
    /// SHA-1 of the host's certificate.
    std::string ssl_thumbprint;
    /// The host's certificate itself, PEM (vSphere 9, instead of the SHA-1).
    std::string ssl_certificate;
    /// (hashAlgorithm, thumbprint) pairs from certThumbprintList.
    std::vector<std::pair<std::string, std::string>> cert_thumbprints;
};

/// The SOAPAction header value for an API version: "urn:vim25/8.0.3.0".
std::string soap_action(std::string_view api_version);

/// XML-escapes text for element content.
std::string escape(std::string_view text);

// Request envelopes.
std::string retrieve_service_content();
std::string login(const MoRef& session_manager, std::string_view user, std::string_view password);
std::string logout(const MoRef& session_manager);
std::string find_by_inventory_path(const MoRef& search_index, std::string_view path);
std::string create_container_view(const MoRef& view_manager, const MoRef& container, std::string_view type);
std::string destroy_view(const MoRef& view);
/// Name and power state of every VM in a ContainerView.
std::string retrieve_vms_in_view(const MoRef& property_collector, const MoRef& view);
/// Name and power state of one VM.
std::string retrieve_vm(const MoRef& property_collector, const MoRef& vm);
std::string continue_retrieve(const MoRef& property_collector, std::string_view token);
std::string acquire_ticket(const MoRef& vm, std::string_view ticket_type);

// Replies.
Reply<ServiceContent> parse_service_content(std::string_view xml);
/// For methods whose result the plugin doesn't need (Login, Logout,
/// DestroyView): true, or the fault.
Reply<bool> parse_done(std::string_view xml);
/// FindByInventoryPath, CreateContainerView: the returned reference, empty
/// when there is none.
Reply<MoRef> parse_moref(std::string_view xml);
/// RetrievePropertiesEx, ContinueRetrievePropertiesEx.
Reply<VmList> parse_vms(std::string_view xml);
Reply<Ticket> parse_ticket(std::string_view xml);

}  // namespace vmware::soap
