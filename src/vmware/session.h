// One Casement tab: the vSphere API conversation and then the console, on a
// thread of their own so that Casement's messages (input, surfaces,
// disconnect) keep flowing meanwhile.

#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "casement/plugin.h"
#include "vmware/console.h"
#include "vmware/target.h"
#include "vmware/vsphere.h"

namespace vmware {

class VmwareSession : public casement::Session {
public:
    VmwareSession(casement::Host& host, Target target, std::string password);
    ~VmwareSession() override;

    void input(const casement::json& msg) override;
    /// Closes the console, logs out (a second at most) and exits.
    void disconnect() override;

private:
    void work(std::string password);

    casement::Host& host_;
    Target target_;
    VSphere api_;
    Console console_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    HANDLE done_ = nullptr;
    std::thread worker_;
};

}  // namespace vmware
