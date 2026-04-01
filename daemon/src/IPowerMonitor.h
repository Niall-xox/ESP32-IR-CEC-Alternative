#pragma once
#include <functional>

// Abstract interface for monitoring PC power events.
//
// Concrete implementations:
//   LinuxPowerMonitor   — systemd-logind over D-Bus, Phase 1
//   WindowsPowerMonitor — Win32 Service API, Phase 2
//
// Callers register callbacks for each event then call run(), which blocks
// and drives the underlying event loop until the process is killed.
class IPowerMonitor {
public:
    // Called when the system is about to enter sleep.
    virtual void setOnSleep(std::function<void()> cb) = 0;

    // Called when the system has woken from sleep.
    virtual void setOnWake(std::function<void()> cb) = 0;

    // Called when the system is about to shut down.
    virtual void setOnShutdown(std::function<void()> cb) = 0;

    // Blocks and runs the event loop. Returns only when the process is killed.
    virtual void run() = 0;

    virtual ~IPowerMonitor() = default;
};
