#pragma once
#include "IPowerMonitor.h"

#include <sdbus-c++/sdbus-c++.h>
#include <functional>
#include <memory>
#include <optional>

// IPowerMonitor implementation for Linux using systemd-logind over D-Bus.
//
// Subscribes to PrepareForSleep and PrepareForShutdown signals on
// org.freedesktop.login1. Holds a systemd delay inhibitor lock so the system
// waits for the IR command to fire before sleeping or shutting down.
//
// Inhibitor lock lifecycle:
//   1. Lock is acquired at construction and held continuously.
//   2. When a sleep/shutdown signal fires, the callback runs to completion —
//      which for HIDTransport means the ESP32 has ACKed — and the lock is then
//      released. systemd proceeds. There is no fixed delay anywhere.
//   3. On wake the lock is re-acquired, ready for the next sleep. A
//      cancelled shutdown is deliberately not handled — see the brief.
class LinuxPowerMonitor : public IPowerMonitor {
public:
    // Connects to the system D-Bus, registers signal handlers, and acquires
    // the initial inhibitor lock. Throws std::runtime_error on D-Bus failure.
    LinuxPowerMonitor();
    ~LinuxPowerMonitor() override;

    void setOnSleep(std::function<void()> cb) override;
    void setOnWake(std::function<void()> cb) override;
    void setOnShutdown(std::function<void()> cb) override;

    // Blocks and runs the sdbus event loop until the process is killed.
    void run() override;

private:
    // Calls logind's Inhibit() and stores the returned lock fd.
    //
    // Returns false instead of throwing. It is called from D-Bus signal
    // handlers, and an exception escaping one of those unwinds into the sdbus
    // event loop — which at best loses the lock silently and at worst takes the
    // process down. The constructor treats a false return as fatal; the
    // handlers log it and carry on without the delay guarantee.
    bool takeInhibitorLock();

    // Releases the lock by destroying the fd — systemd then proceeds.
    void releaseInhibitorLock();

    // D-Bus signal handlers, called from the sdbus event loop.
    void onPrepareForSleep(bool start);
    void onPrepareForShutdown(bool start);

    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IProxy>      proxy_;

    // The inhibitor lock is a file descriptor returned by logind's Inhibit().
    // Stored in an optional so it can be cleanly released and re-acquired.
    // Destroying the UnixFd closes the fd, which releases the lock.
    std::optional<sdbus::UnixFd> inhibitorFd_;

    std::function<void()> onSleep_;
    std::function<void()> onWake_;
    std::function<void()> onShutdown_;
};
