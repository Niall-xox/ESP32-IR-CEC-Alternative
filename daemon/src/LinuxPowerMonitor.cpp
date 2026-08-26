#include "LinuxPowerMonitor.h"

#include <iostream>
#include <stdexcept>

// logind D-Bus coordinates
static constexpr auto LOGIND_SERVICE   = "org.freedesktop.login1";
static constexpr auto LOGIND_OBJECT    = "/org/freedesktop/login1";
static constexpr auto LOGIND_INTERFACE = "org.freedesktop.login1.Manager";

// The inhibitor lock is released immediately after send() returns.
// HIDTransport::send() blocks until ACK is received from the ESP32, so by the
// time the lock is released the IR signal has already been transmitted.

LinuxPowerMonitor::LinuxPowerMonitor() {
    // Connect to the system bus — logind lives here, not the session bus
    connection_ = sdbus::createSystemBusConnection();

    // Create a proxy to the logind Manager object.
    // sdbus-c++ v2 requires strong typedefs for service name and object path.
    proxy_ = sdbus::createProxy(*connection_,
        sdbus::ServiceName{LOGIND_SERVICE},
        sdbus::ObjectPath{LOGIND_OBJECT});

    // PrepareForSleep(bool start):
    //   start = true  → system is about to sleep    → send OFF, release lock
    //   start = false → system just woke up         → send ON,  re-take lock
    // In sdbus-c++ v2, uponSignal().onInterface().call() replaces registerSignalHandler().
    // The lambda receives typed parameters directly — no manual streaming needed.
    proxy_->uponSignal("PrepareForSleep")
           .onInterface(LOGIND_INTERFACE)
           .call([this](bool start) { onPrepareForSleep(start); });

    // PrepareForShutdown(bool start):
    //   start = true  → system is shutting down     → send OFF, release lock
    //   start = false → shutdown cancelled (rare)   → no action needed
    proxy_->uponSignal("PrepareForShutdown")
           .onInterface(LOGIND_INTERFACE)
           .call([this](bool start) { onPrepareForShutdown(start); });

    // finishRegistration() was removed in sdbus-c++ v2 — registration is immediate.

    // Take the inhibitor lock upfront. It must be held before any signal fires —
    // taking it reactively inside the handler risks a race where systemd doesn't
    // see us in time and proceeds without waiting.
    //
    // Failing here means logind refused the very first Inhibit(), which is a
    // broken environment rather than a transient hiccup. Better to exit and let
    // systemd restart us than to run on looking healthy while never once
    // delaying a suspend.
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

// Every command from this monitor states a zero budget — see the header.
void LinuxPowerMonitor::assertTv(bool on, const char* reason) {
    if (!onCommand_) return;
    try {
        (void)onCommand_(TvCommand{on, reason, std::chrono::milliseconds::zero()});
    } catch (...) {
        // Never propagate: this runs inside a D-Bus signal handler, where an
        // exception unwinds into the sdbus event loop.
    }
}

void LinuxPowerMonitor::run() {
    connection_->enterEventLoop();
}

bool LinuxPowerMonitor::takeInhibitorLock() {
    // Inhibit() arguments:
    //   what  — "sleep:shutdown" so one lock covers both event types
    //   who   — identifier shown in `systemd-inhibit --list` and journalctl
    //   why   — reason shown alongside the who field
    //   mode  — "delay" defers the action temporarily; "block" would prevent it entirely
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
        // Never propagate: this runs inside D-Bus signal handlers.
        std::cerr << "[inhibitor] Failed to acquire lock: " << e.what()
                  << " — sleep and shutdown will not be delayed\n";
        return false;
    }
}

void LinuxPowerMonitor::releaseInhibitorLock() {
    if (inhibitorFd_.has_value()) {
        inhibitorFd_.reset();  // UnixFd destructor closes the fd, releasing the lock
        std::cout << "[inhibitor] Lock released\n";
    }
}

void LinuxPowerMonitor::onPrepareForSleep(bool start) {
    if (start) {
        std::cout << "[event] Going to sleep\n";
        // Always release the inhibitor lock even if the command fails (e.g. ESP32
        // not plugged in) — the system must not be blocked from sleeping.
        assertTv(false, "sleep");
        releaseInhibitorLock();
    } else {
        std::cout << "[event] Woke up\n";
        assertTv(true, "wake");
        takeInhibitorLock();  // re-take ready for the next sleep event
    }
}

void LinuxPowerMonitor::onPrepareForShutdown(bool start) {
    if (start) {
        std::cout << "[event] Shutting down\n";
        // Always release the inhibitor lock even if the command fails — the
        // system must not be blocked from shutting down.
        assertTv(false, "shutdown");
        releaseInhibitorLock();
    }

    // start = false means a scheduled shutdown was cancelled. Deliberately
    // unhandled — see "Cancelled shutdown" in the brief.
    //
    // The lock is not re-taken here, so the next sleep or shutdown is not
    // delayed. The exposure is that one event, not a permanent one: the
    // wake path re-takes unconditionally, so a sleep/wake cycle restores
    // it. Accepted consequence of an edge case judged not worth the code.
}
