// SOAP requests as the plugin builds them, and replies recorded from vcsim
// and fake-esxi (recorded.h), plus a few in the shape ESXi sends them.

#include "check.h"

#include <string>

#include "recorded.h"
#include "vmware/soap.h"

using namespace vmware::soap;

namespace {

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// As ESXi 8.0 answers RetrieveServiceContent (shortened: the managers the
// plugin doesn't use are left out), with its line breaks.
constexpr std::string_view esxi_service_content = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<soapenv:Envelope xmlns:soapenc="http://schemas.xmlsoap.org/soap/encoding/"
 xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"
 xmlns:xsd="http://www.w3.org/2001/XMLSchema"
 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<soapenv:Body>
<RetrieveServiceContentResponse xmlns="urn:vim25"><returnval><rootFolder type="Folder">ha-folder-root</rootFolder><propertyCollector type="PropertyCollector">ha-property-collector</propertyCollector><viewManager type="ViewManager">ViewManager</viewManager><about><name>VMware ESXi</name><fullName>VMware ESXi 8.0.3 build-24022510</fullName><vendor>VMware, Inc.</vendor><version>8.0.3</version><build>24022510</build><localeVersion>INTL</localeVersion><localeBuild>000</localeBuild><osType>vmnix-x86</osType><productLineId>embeddedEsx</productLineId><apiType>HostAgent</apiType><apiVersion>8.0.3.0</apiVersion><licenseProductName>VMware ESX Server</licenseProductName><licenseProductVersion>8.0</licenseProductVersion></about><setting type="OptionManager">HostAgentSettings</setting><userDirectory type="UserDirectory">ha-user-directory</userDirectory><sessionManager type="SessionManager">ha-sessionmgr</sessionManager><authorizationManager type="AuthorizationManager">ha-authmgr</authorizationManager><serviceManager type="ServiceManager">ha-servicemanager</serviceManager><perfManager type="PerformanceManager">ha-perfmgr</perfManager><searchIndex type="SearchIndex">ha-searchindex</searchIndex></returnval></RetrieveServiceContentResponse>
</soapenv:Body>
</soapenv:Envelope>)xml";

// A fault with other prefixes, a type only in xsi:type, and a message
// with entities.
constexpr std::string_view prefixed_fault = R"xml(<?xml version="1.0"?>
<S:Envelope xmlns:S="http://schemas.xmlsoap.org/soap/envelope/" xmlns:i="http://www.w3.org/2001/XMLSchema-instance">
<S:Body><S:Fault><faultcode>ServerFaultCode</faultcode><faultstring>Current license or ESXi version prohibits execution of the requested operation &amp; more.</faultstring>
<detail><Fault xmlns="urn:vim25" i:type="RestrictedVersion"></Fault></detail></S:Fault></S:Body></S:Envelope>)xml";

}  // namespace

TEST(builds_envelopes) {
    std::string e = retrieve_service_content();
    CHECK(e.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    CHECK(contains(e, "<soapenv:Body><RetrieveServiceContent xmlns=\"urn:vim25\">"
                      "<_this type=\"ServiceInstance\">ServiceInstance</_this></RetrieveServiceContent>"));
    CHECK(e.ends_with("</soapenv:Body></soapenv:Envelope>"));
    CHECK_EQ(soap_action("8.0.3.0"), std::string("urn:vim25/8.0.3.0"));
}

TEST(escapes_login) {
    std::string e = login({"SessionManager", "ha-sessionmgr"}, "ann@corp.local", "p<&>\"'\r\nw");
    CHECK(contains(e, "<_this type=\"SessionManager\">ha-sessionmgr</_this>"));
    CHECK(contains(e, "<userName>ann@corp.local</userName>"));
    CHECK(contains(e, "<password>p&lt;&amp;&gt;&quot;&apos;&#13;\nw</password>"));
    CHECK_EQ(escape("a&b"), std::string("a&amp;b"));
}

TEST(builds_lookups) {
    std::string path = find_by_inventory_path({"SearchIndex", "SearchIndex"}, "DC1/vm/a&b");
    CHECK(contains(path, "<FindByInventoryPath xmlns=\"urn:vim25\"><_this type=\"SearchIndex\">SearchIndex</_this>"
                         "<inventoryPath>DC1/vm/a&amp;b</inventoryPath></FindByInventoryPath>"));
    std::string view = create_container_view({"ViewManager", "ViewManager"}, {"Folder", "group-d1"}, "VirtualMachine");
    CHECK(contains(view, "<container type=\"Folder\">group-d1</container><type>VirtualMachine</type>"
                         "<recursive>true</recursive>"));
    std::string props = retrieve_vms_in_view({"PropertyCollector", "propertyCollector"}, {"ContainerView", "session[1]2"});
    CHECK(contains(props, "<pathSet>name</pathSet><pathSet>runtime.powerState</pathSet>"));
    CHECK(contains(props, "<obj type=\"ContainerView\">session[1]2</obj><skip>true</skip>"));
    CHECK(contains(props, "<selectSet xsi:type=\"TraversalSpec\"><name>view</name><type>ContainerView</type>"
                          "<path>view</path>"));
    CHECK(contains(props, "<options></options>"));
    std::string one = retrieve_vm({"PropertyCollector", "propertyCollector"}, {"VirtualMachine", "vm-42"});
    CHECK(contains(one, "<obj type=\"VirtualMachine\">vm-42</obj><skip>false</skip>"));
    CHECK(!contains(one, "selectSet"));
    CHECK(contains(continue_retrieve({"PropertyCollector", "propertyCollector"}, "7"), "<token>7</token>"));
    CHECK(contains(destroy_view({"ContainerView", "v"}), "<DestroyView xmlns=\"urn:vim25\"><_this type=\"ContainerView\">"));
}

TEST(builds_ticket_and_logout) {
    CHECK(contains(acquire_ticket({"VirtualMachine", "vm-42"}, "webmks"),
                   "<AcquireTicket xmlns=\"urn:vim25\"><_this type=\"VirtualMachine\">vm-42</_this>"
                   "<ticketType>webmks</ticketType></AcquireTicket>"));
    CHECK(contains(logout({"SessionManager", "SessionManager"}),
                   "<Logout xmlns=\"urn:vim25\"><_this type=\"SessionManager\">SessionManager</_this></Logout>"));
}

TEST(reads_service_content_from_vcenter) {
    auto r = parse_service_content(recorded::service_content);
    REQUIRE(r.value);
    CHECK(!r.fault);
    CHECK(r.value->root_folder == (MoRef{"Folder", "group-d1"}));
    CHECK(r.value->property_collector == (MoRef{"PropertyCollector", "propertyCollector"}));
    CHECK(r.value->view_manager == (MoRef{"ViewManager", "ViewManager"}));
    CHECK(r.value->session_manager == (MoRef{"SessionManager", "SessionManager"}));
    CHECK(r.value->search_index == (MoRef{"SearchIndex", "SearchIndex"}));
    CHECK_EQ(r.value->api_type, std::string("VirtualCenter"));
    CHECK_EQ(r.value->api_version, std::string("6.5"));
}

TEST(reads_service_content_from_esxi) {
    auto r = parse_service_content(esxi_service_content);
    REQUIRE(r.value);
    CHECK(r.value->root_folder == (MoRef{"Folder", "ha-folder-root"}));
    CHECK(r.value->session_manager == (MoRef{"SessionManager", "ha-sessionmgr"}));
    CHECK(r.value->search_index == (MoRef{"SearchIndex", "ha-searchindex"}));
    CHECK_EQ(r.value->api_type, std::string("HostAgent"));
    CHECK_EQ(r.value->api_version, std::string("8.0.3.0"));
    CHECK_EQ(r.value->full_name, std::string("VMware ESXi 8.0.3 build-24022510"));
}

TEST(reads_login_and_its_fault) {
    auto ok = parse_done(recorded::login);
    CHECK(ok.value && *ok.value);
    auto bad = parse_done(recorded::login_invalid);
    CHECK(!bad.value);
    REQUIRE(bad.fault);
    CHECK_EQ(bad.fault->type, std::string("InvalidLogin"));
    CHECK_EQ(bad.fault->message, std::string("Login failure"));
}

TEST(reads_views_and_vms) {
    auto view = parse_moref(recorded::container_view);
    REQUIRE(view.value);
    CHECK_EQ(view.value->type, std::string("ContainerView"));
    CHECK(view.value->value.starts_with("session["));

    auto vms = parse_vms(recorded::vms_in_view);
    REQUIRE(vms.value);
    REQUIRE(vms.value->vms.size() == 4);
    CHECK(vms.value->vms[0].ref == (MoRef{"VirtualMachine", "vm-62"}));
    CHECK_EQ(vms.value->vms[0].name, std::string("DC0_H0_VM0"));
    CHECK_EQ(vms.value->vms[0].power_state, std::string("poweredOn"));
    CHECK(vms.value->token.empty());

    auto fake = parse_vms(recorded::fake_vms_in_view);
    REQUIRE(fake.value);
    REQUIRE(fake.value->vms.size() == 14);
    int dups = 0;
    for (const auto& vm : fake.value->vms) {
        if (vm.name == "dup") ++dups;
        if (vm.name == "off") CHECK_EQ(vm.power_state, std::string("poweredOff"));
    }
    CHECK_EQ(dups, 2);
    CHECK_EQ(fake.value->vms[12].name, std::string("100%25"));

    auto one = parse_vms(recorded::vm_properties);
    REQUIRE(one.value);
    REQUIRE(one.value->vms.size() == 1);
    CHECK_EQ(one.value->vms[0].name, std::string("DC0_H0_VM1"));

    CHECK(parse_done(recorded::destroy_view).value);
    CHECK(parse_done(recorded::logout).value);
}

TEST(reads_paging_tokens) {
    constexpr std::string_view page = R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"><soapenv:Body><RetrievePropertiesExResponse xmlns="urn:vim25"><returnval><token>1</token><objects><obj type="VirtualMachine">vm-1</obj><propSet><name>name</name><val>a</val></propSet></objects></returnval></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>)";
    auto r = parse_vms(page);
    REQUIRE(r.value);
    CHECK_EQ(r.value->token, std::string("1"));
    REQUIRE(r.value->vms.size() == 1);
    CHECK(r.value->vms[0].power_state.empty());
    constexpr std::string_view none = R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"><soapenv:Body><RetrievePropertiesExResponse xmlns="urn:vim25"></RetrievePropertiesExResponse></soapenv:Body></soapenv:Envelope>)";
    auto empty = parse_vms(none);
    REQUIRE(empty.value);
    CHECK(empty.value->vms.empty());
}

TEST(reads_inventory_paths) {
    auto found = parse_moref(recorded::path_found);
    REQUIRE(found.value);
    CHECK(*found.value == (MoRef{"VirtualMachine", "vm-65"}));
    auto missing = parse_moref(recorded::path_missing);
    REQUIRE(missing.value);
    CHECK(missing.value->empty());
}

TEST(reads_object_faults) {
    auto r = parse_vms(recorded::object_not_found);
    REQUIRE(r.fault);
    CHECK_EQ(r.fault->type, std::string("ManagedObjectNotFound"));
    CHECK(r.fault->object == (MoRef{"VirtualMachine", "vm-999"}));

    auto missing = parse_ticket(recorded::no_acquire_ticket);
    REQUIRE(missing.fault);
    CHECK_EQ(missing.fault->type, std::string("MethodNotFound"));
    CHECK_EQ(missing.fault->message, std::string("VirtualMachine:vm-62 does not implement: AcquireTicket"));

    auto denied = parse_ticket(recorded::no_permission);
    REQUIRE(denied.fault);
    CHECK_EQ(denied.fault->type, std::string("NoPermission"));
    CHECK_EQ(denied.fault->privilege, std::string("VirtualMachine.Interact.ConsoleInteract"));
    CHECK(denied.fault->object == (MoRef{"VirtualMachine", "vm-62"}));

    auto restricted = parse_done(prefixed_fault);
    REQUIRE(restricted.fault);
    CHECK_EQ(restricted.fault->type, std::string("RestrictedVersion"));
    CHECK(restricted.fault->message.ends_with("operation & more."));
}

TEST(reads_tickets) {
    auto v8 = parse_ticket(recorded::ticket_vcenter8);
    REQUIRE(v8.value);
    CHECK_EQ(v8.value->ticket, std::string("7ccc0651062d610e44b21d885ae0717c"));
    CHECK_EQ(v8.value->host, std::string("127.0.0.2"));
    CHECK_EQ(v8.value->port, 10502);
    CHECK_EQ(v8.value->url, std::string("wss://127.0.0.2:10502/ticket/7ccc0651062d610e44b21d885ae0717c"));
    CHECK_EQ(v8.value->ssl_thumbprint, std::string("82:9E:D1:4E:D8:60:15:AF:D7:00:82:37:C4:62:4D:C6:6B:30:96:03"));
    REQUIRE(v8.value->cert_thumbprints.size() == 1);
    CHECK_EQ(v8.value->cert_thumbprints[0].first, std::string("sha256"));
    CHECK(v8.value->cert_thumbprints[0].second.starts_with("A5:11:AF:F9"));
    CHECK(v8.value->ssl_certificate.empty());

    auto v9 = parse_ticket(recorded::ticket_vsphere9);
    REQUIRE(v9.value);
    CHECK(v9.value->ssl_thumbprint.empty());
    CHECK(v9.value->url.empty());
    CHECK(v9.value->ssl_certificate.starts_with("-----BEGIN CERTIFICATE-----\nMIIBZDCC"));
}

TEST(rejects_what_isnt_soap) {
    auto html = parse_service_content("<html><body>Welcome to VMware ESXi</body></html>");
    CHECK(!html.value);
    CHECK(!html.fault);
    CHECK(!html.error.empty());
    auto junk = parse_done("not xml at all <");
    CHECK(!junk.value);
    CHECK(!junk.error.empty());
    auto empty = parse_service_content(
        R"(<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/"><soapenv:Body/></soapenv:Envelope>)");
    CHECK(!empty.value);
    auto bad_port = parse_ticket(
        R"(<soapenv:Envelope xmlns:soapenv="x"><soapenv:Body><AcquireTicketResponse><returnval><ticket>t</ticket><port>99999</port></returnval></AcquireTicketResponse></soapenv:Body></soapenv:Envelope>)");
    CHECK(!bad_port.value);
    auto no_ticket = parse_ticket(
        R"(<soapenv:Envelope xmlns:soapenv="x"><soapenv:Body><AcquireTicketResponse><returnval></returnval></AcquireTicketResponse></soapenv:Body></soapenv:Envelope>)");
    CHECK(!no_ticket.value);
}
