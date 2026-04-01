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
//   2. When a sleep/shutdown signal fires, the callback runs, we wait 500ms,
//      then release the lock — systemd then proceeds.
//   3. On wake, the lock is re-acquired ready for the next sleep.
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
    // Calls logind's Inhibit() method and stores the returned lock fd.
    void takeInhibitorLock();

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
