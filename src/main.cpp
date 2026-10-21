// casement-vmware: the console of a VM on VMware ESXi or vCenter Server as a
// Casement tab. See README.md.

#include "casement/plugin.h"
#include "vmware/session.h"
#include "vmware/target.h"

int main() {
    casement::Info info{"VMWARE", "VMware console", PLUGIN_VERSION, {"password", "cursor"}, 1, 1};
    return casement::run(info, [](casement::Host& host, const casement::Connect& c) -> std::unique_ptr<casement::Session> {
        std::string error;
        auto target = vmware::parse_target(c.address, c.username, error);
        if (!target) host.finish(casement::State::failed, error);
        if (c.password.empty()) host.finish(casement::State::auth_failed, "A password is needed.");
        return std::make_unique<vmware::VmwareSession>(host, std::move(*target), c.password);
    });
}
