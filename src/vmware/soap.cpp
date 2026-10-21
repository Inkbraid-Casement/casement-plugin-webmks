#include "vmware/soap.h"

#include <cstring>
#include <format>

#include <pugixml.hpp>

namespace vmware::soap {

namespace {

constexpr std::string_view envelope_head =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<soapenv:Envelope xmlns:soapenc=\"http://schemas.xmlsoap.org/soap/encoding/\""
    " xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
    " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
    "<soapenv:Body>";
constexpr std::string_view envelope_tail = "</soapenv:Body></soapenv:Envelope>";

std::string envelope(std::string_view body) {
    std::string out(envelope_head);
    out += body;
    out += envelope_tail;
    return out;
}

std::string this_ref(const MoRef& ref) {
    return std::format("<_this type=\"{}\">{}</_this>", escape(ref.type), escape(ref.value));
}

std::string ref_element(std::string_view element, const MoRef& ref) {
    return std::format("<{0} type=\"{1}\">{2}</{0}>", element, escape(ref.type), escape(ref.value));
}

// Element names without their namespace prefix: replies from different
// servers use different prefixes for the same namespaces.
std::string_view local_name(const char* name) {
    std::string_view n(name);
    auto colon = n.find(':');
    return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

pugi::xml_node child(pugi::xml_node node, std::string_view name) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element && local_name(c.name()) == name) return c;
    return {};
}

std::vector<pugi::xml_node> children(pugi::xml_node node, std::string_view name) {
    std::vector<pugi::xml_node> out;
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element && local_name(c.name()) == name) out.push_back(c);
    return out;
}

pugi::xml_node first_element(pugi::xml_node node) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) return c;
    return {};
}

std::string text(pugi::xml_node node) { return node ? std::string(node.child_value()) : std::string(); }

MoRef moref(pugi::xml_node node) {
    if (!node) return {};
    return MoRef{node.attribute("type").value(), text(node)};
}

// The xsi:type of an element, whatever prefix the server bound the schema
// instance namespace to.
std::string xsi_type(pugi::xml_node node) {
    for (pugi::xml_attribute a = node.first_attribute(); a; a = a.next_attribute()) {
        std::string_view n(a.name());
        if (n.find(':') != std::string_view::npos && local_name(a.name()) == "type")
            return std::string(local_name(a.value()));
    }
    return {};
}

Fault read_fault(pugi::xml_node fault) {
    Fault f;
    f.message = text(child(fault, "faultstring"));
    pugi::xml_node detail = first_element(child(fault, "detail"));
    if (detail) {
        // <InvalidLoginFault xsi:type="InvalidLogin">: the element is the type
        // with "Fault" appended.
        std::string_view element = local_name(detail.name());
        if (element.size() > 5 && element.ends_with("Fault")) f.type = std::string(element.substr(0, element.size() - 5));
        else f.type = xsi_type(detail);
        f.privilege = text(child(detail, "privilegeId"));
        f.object = moref(child(detail, "object"));
        if (f.object.empty()) f.object = moref(child(detail, "obj"));
    }
    return f;
}

// Opens a reply: the element inside soapenv:Body, which is either the
// response or a fault. The document must outlive the returned node.
template <class T>
pugi::xml_node open_reply(pugi::xml_document& doc, std::string_view xml, Reply<T>& reply) {
    pugi::xml_parse_result r = doc.load_buffer(xml.data(), xml.size());
    if (!r) {
        reply.error = std::format("the reply isn't XML ({})", r.description());
        return {};
    }
    pugi::xml_node envelope = doc.document_element();
    if (local_name(envelope.name()) != "Envelope") {
        reply.error = "the reply isn't a SOAP envelope";
        return {};
    }
    pugi::xml_node content = first_element(child(envelope, "Body"));
    if (!content) {
        reply.error = "the SOAP reply has an empty body";
        return {};
    }
    if (local_name(content.name()) == "Fault") {
        reply.fault = read_fault(content);
        return {};
    }
    return content;
}

}  // namespace

std::string soap_action(std::string_view api_version) { return std::format("urn:vim25/{}", api_version); }

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            // A literal CR would come back as LF after the server's
            // end-of-line normalisation.
            case '\r': out += "&#13;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string retrieve_service_content() {
    return envelope("<RetrieveServiceContent xmlns=\"urn:vim25\">"
                    "<_this type=\"ServiceInstance\">ServiceInstance</_this>"
                    "</RetrieveServiceContent>");
}

std::string login(const MoRef& session_manager, std::string_view user, std::string_view password) {
    return envelope(std::format("<Login xmlns=\"urn:vim25\">{}<userName>{}</userName><password>{}</password></Login>",
                                this_ref(session_manager), escape(user), escape(password)));
}

std::string logout(const MoRef& session_manager) {
    return envelope(std::format("<Logout xmlns=\"urn:vim25\">{}</Logout>", this_ref(session_manager)));
}

std::string find_by_inventory_path(const MoRef& search_index, std::string_view path) {
    return envelope(std::format("<FindByInventoryPath xmlns=\"urn:vim25\">{}<inventoryPath>{}</inventoryPath>"
                                "</FindByInventoryPath>",
                                this_ref(search_index), escape(path)));
}

std::string create_container_view(const MoRef& view_manager, const MoRef& container, std::string_view type) {
    return envelope(std::format("<CreateContainerView xmlns=\"urn:vim25\">{}{}<type>{}</type>"
                                "<recursive>true</recursive></CreateContainerView>",
                                this_ref(view_manager), ref_element("container", container), escape(type)));
}

std::string destroy_view(const MoRef& view) {
    return envelope(std::format("<DestroyView xmlns=\"urn:vim25\">{}</DestroyView>", this_ref(view)));
}

namespace {

constexpr std::string_view vm_properties =
    "<propSet><type>VirtualMachine</type><pathSet>name</pathSet><pathSet>runtime.powerState</pathSet></propSet>";

}  // namespace

std::string retrieve_vms_in_view(const MoRef& property_collector, const MoRef& view) {
    return envelope(std::format(
        "<RetrievePropertiesEx xmlns=\"urn:vim25\">{}<specSet>{}"
        "<objectSet>{}<skip>true</skip>"
        "<selectSet xsi:type=\"TraversalSpec\"><name>view</name><type>ContainerView</type><path>view</path>"
        "<skip>false</skip></selectSet></objectSet>"
        "</specSet><options></options></RetrievePropertiesEx>",
        this_ref(property_collector), vm_properties, ref_element("obj", view)));
}

std::string retrieve_vm(const MoRef& property_collector, const MoRef& vm) {
    return envelope(std::format("<RetrievePropertiesEx xmlns=\"urn:vim25\">{}<specSet>{}"
                                "<objectSet>{}<skip>false</skip></objectSet>"
                                "</specSet><options></options></RetrievePropertiesEx>",
                                this_ref(property_collector), vm_properties, ref_element("obj", vm)));
}

std::string continue_retrieve(const MoRef& property_collector, std::string_view token) {
    return envelope(std::format("<ContinueRetrievePropertiesEx xmlns=\"urn:vim25\">{}<token>{}</token>"
                                "</ContinueRetrievePropertiesEx>",
                                this_ref(property_collector), escape(token)));
}

std::string acquire_ticket(const MoRef& vm, std::string_view ticket_type) {
    return envelope(std::format("<AcquireTicket xmlns=\"urn:vim25\">{}<ticketType>{}</ticketType></AcquireTicket>",
                                this_ref(vm), escape(ticket_type)));
}

Reply<ServiceContent> parse_service_content(std::string_view xml) {
    Reply<ServiceContent> reply;
    pugi::xml_document doc;
    pugi::xml_node response = open_reply(doc, xml, reply);
    if (!response) return reply;
    pugi::xml_node rv = child(response, "returnval");
    if (!rv) {
        reply.error = "the service content is missing";
        return reply;
    }
    ServiceContent sc;
    sc.root_folder = moref(child(rv, "rootFolder"));
    sc.property_collector = moref(child(rv, "propertyCollector"));
    sc.view_manager = moref(child(rv, "viewManager"));
    sc.session_manager = moref(child(rv, "sessionManager"));
    sc.search_index = moref(child(rv, "searchIndex"));
    pugi::xml_node about = child(rv, "about");
    sc.api_type = text(child(about, "apiType"));
    sc.api_version = text(child(about, "apiVersion"));
    sc.full_name = text(child(about, "fullName"));
    if (sc.root_folder.empty() || sc.property_collector.empty() || sc.session_manager.empty() ||
        sc.search_index.empty() || sc.view_manager.empty() || sc.api_version.empty()) {
        reply.error = "the service content lacks the session manager, search index, views or version";
        return reply;
    }
    reply.value = std::move(sc);
    return reply;
}

Reply<bool> parse_done(std::string_view xml) {
    Reply<bool> reply;
    pugi::xml_document doc;
    if (open_reply(doc, xml, reply)) reply.value = true;
    return reply;
}

Reply<MoRef> parse_moref(std::string_view xml) {
    Reply<MoRef> reply;
    pugi::xml_document doc;
    pugi::xml_node response = open_reply(doc, xml, reply);
    if (!response) return reply;
    reply.value = moref(child(response, "returnval"));
    return reply;
}

Reply<VmList> parse_vms(std::string_view xml) {
    Reply<VmList> reply;
    pugi::xml_document doc;
    pugi::xml_node response = open_reply(doc, xml, reply);
    if (!response) return reply;
    VmList list;
    // No returnval at all means no objects.
    pugi::xml_node rv = child(response, "returnval");
    list.token = text(child(rv, "token"));
    for (pugi::xml_node object : children(rv, "objects")) {
        VmInfo vm;
        vm.ref = moref(child(object, "obj"));
        for (pugi::xml_node prop : children(object, "propSet")) {
            std::string name = text(child(prop, "name"));
            if (name == "name") vm.name = text(child(prop, "val"));
            else if (name == "runtime.powerState") vm.power_state = text(child(prop, "val"));
        }
        if (!vm.ref.empty()) list.vms.push_back(std::move(vm));
    }
    reply.value = std::move(list);
    return reply;
}

Reply<Ticket> parse_ticket(std::string_view xml) {
    Reply<Ticket> reply;
    pugi::xml_document doc;
    pugi::xml_node response = open_reply(doc, xml, reply);
    if (!response) return reply;
    pugi::xml_node rv = child(response, "returnval");
    Ticket t;
    t.ticket = text(child(rv, "ticket"));
    t.host = text(child(rv, "host"));
    t.url = text(child(rv, "url"));
    t.ssl_thumbprint = text(child(rv, "sslThumbprint"));
    t.ssl_certificate = text(child(rv, "sslCertificate"));
    std::string port = text(child(rv, "port"));
    if (!port.empty()) {
        int value = 0;
        for (char c : port) {
            if (c < '0' || c > '9' || value > 65535) {
                value = -1;
                break;
            }
            value = value * 10 + (c - '0');
        }
        if (value <= 0 || value > 65535) {
            reply.error = std::format("the ticket names port “{}”", port);
            return reply;
        }
        t.port = value;
    }
    for (pugi::xml_node entry : children(rv, "certThumbprintList"))
        t.cert_thumbprints.emplace_back(text(child(entry, "hashAlgorithm")), text(child(entry, "thumbprint")));
    if (t.ticket.empty()) {
        reply.error = "the server sent no ticket";
        return reply;
    }
    reply.value = std::move(t);
    return reply;
}

}  // namespace vmware::soap
