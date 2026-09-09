#include "LinuxPowerMonitor.h"

#include <iostream>
#include <stdexcept>

static constexpr auto LOGIND_SERVICE   = "org.freedesktop.login1";
static constexpr auto LOGIND_OBJECT    = "/org/freedesktop/login1";
static constexpr auto LOGIND_INTERFACE = "org.freedesktop.login1.Manager";

LinuxPowerMonitor::LinuxPowerMonitor() {

    connection_ = sdbus::createSystemBusConnection();

    proxy_ = sdbus::createProxy(*connection_,
        sdbus::ServiceName{LOGIND_SERVICE},
        sdbus::ObjectPath{LOGIND_OBJECT});

    proxy_->uponSignal("PrepareForSleep")
           .onInterface(LOGIND_INTERFACE)
           .call([this](bool start) { onPrepareForSleep(start); });

    proxy_->uponSignal("PrepareForShutdown")
           .onInterface(LOGIND_INTERFACE)
           .call([this](bool start) { onPrepareForShutdown(start); });

    if (!takeInhibitorLock()) {
        throw std::runtime_error("logind refused the initial inhibitor lock");
    }
}

LinuxPowerMonitor::~LinuxPowerMonitor() {
    releaseInhibitorLock();
}

void LinuxPowerMonitor::setOnCommand(std::function<bool(const TvCommand&)> cb) {
    onCommand_ = std::move(cb);
}

void LinuxPowerMonitor::assertTv(bool on, const char* reason) {
    if (!onCommand_) return;
    try {
        (void)onCommand_(TvCommand{on, reason});
    } catch (...) {

    }
}

void LinuxPowerMonitor::run() {
    connection_->enterEventLoop();
}

bool LinuxPowerMonitor::takeInhibitorLock() {

    try {
        sdbus::UnixFd fd;
        proxy_->callMethod("Inhibit")
            .onInterface(LOGIND_INTERFACE)
            .withArguments(
                std::string("sleep:shutdown"),
                std::string("esp32-ir-remote"),
                std::string("Sending IR command to TV"),
                std::string("delay"))
            .storeResultsTo(fd);

        inhibitorFd_ = std::move(fd);
        std::cout << "[inhibitor] Lock acquired\n";
        return true;
    } catch (const std::exception& e) {

        std::cerr << "[inhibitor] Failed to acquire lock: " << e.what()
                  << " — sleep and shutdown will not be delayed\n";
        return false;
    }
}

void LinuxPowerMonitor::releaseInhibitorLock() {
    if (inhibitorFd_.has_value()) {
        inhibitorFd_.reset();
        std::cout << "[inhibitor] Lock released\n";
    }
}

void LinuxPowerMonitor::onPrepareForSleep(bool start) {
    if (start) {
        std::cout << "[event] Going to sleep\n";

        assertTv(false, "sleep");
        releaseInhibitorLock();
    } else {
        std::cout << "[event] Woke up\n";
        assertTv(true, "wake");
        takeInhibitorLock();
    }
}

void LinuxPowerMonitor::onPrepareForShutdown(bool start) {
    if (start) {
        std::cout << "[event] Shutting down\n";

        assertTv(false, "shutdown");
        releaseInhibitorLock();
    }

}
