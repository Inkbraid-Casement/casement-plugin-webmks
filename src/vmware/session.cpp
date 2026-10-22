#include "vmware/session.h"

#include <format>

#include "casement/log.h"
#include "vmware/failure.h"

namespace vmware {

VmwareSession::VmwareSession(casement::Host& host, Target target, std::string password)
    : host_(host), target_(std::move(target)), api_(target_), console_(host, target_.keys) {
    done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_ = std::thread([this, p = std::move(password)]() mutable { work(std::move(p)); });
}

VmwareSession::~VmwareSession() {
    // Not reached in practice: every way out ends the process first.
    stopping_ = true;
    api_.abort();
    console_.stop();
    if (worker_.joinable()) worker_.join();
    if (done_) CloseHandle(done_);
}

void VmwareSession::work(std::string password) {
    casement::State state = casement::State::failed;
    std::string text;
    bool report = true;
    try {
        api_.login(password);
        SecureZeroMemory(password.data(), password.size());
        password.clear();
        soap::VmInfo vm = api_.find_vm(target_.vm);
        ConsoleEndpoint endpoint = api_.acquire_ticket(vm);
        console_.open(endpoint, unescape_name(vm.name));
        host_.send(casement::state_message(casement::State::connected));
        connected_ = true;
        ConsoleEnd end = console_.run();
        state = end.state;
        text = end.text;
    } catch (const Failure& f) {
        state = f.state;
        text = f.what();
    } catch (const Stopped&) {
        report = false;
    } catch (const std::exception& e) {
        text = std::format("Something unexpected went wrong: {}.", e.what());
    }
    SecureZeroMemory(password.data(), password.size());
    if (stopping_) report = false;
    // The console first, so the host ends it right away, then the session.
    console_.stop();
    api_.logout();
    SetEvent(done_);
    if (report) host_.finish(state, text);
}

void VmwareSession::input(const casement::json& msg) {
    if (connected_) console_.input(msg);
}

void VmwareSession::disconnect() {
    casement::log::info("disconnect");
    stopping_ = true;
    api_.abort();
    console_.stop();
    // The worker logs out; Casement ends the process two seconds after
    // `disconnect`, so wait a little less.
    WaitForSingleObject(done_, 1500);
    // Exiting here rather than returning: the worker may still be inside a
    // call that refers to this session.
    ExitProcess(0);
}

}  // namespace vmware
