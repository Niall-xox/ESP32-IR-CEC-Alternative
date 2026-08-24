#pragma once
#include <chrono>
#include <functional>

// Abstract interface for monitoring PC power events.
//
// Concrete implementations:
//   LinuxPowerMonitor   — systemd-logind over D-Bus
//   WindowsPowerMonitor — Win32 Service API, driven by console display state
//
// Callers register callbacks for each event then call run(), which blocks
// and drives the underlying event loop until the process is killed.
class IPowerMonitor {
public:
    // The callbacks return whether the device *confirmed* the command — the
    // same guarantee ITransport::send() carries.
    //
    // An implementation that suppresses redundant commands needs that answer.
    // Recording an attempt that was never acknowledged as the TV's new state
    // would suppress the retry and leave it wrong indefinitely, which is the
    // "reports success regardless" failure in a new place. An implementation
    // that keeps no state may ignore the result — LinuxPowerMonitor does,
    // because logind tells it what happened and it sends on every event.

    // Called when the system is about to enter sleep.
    virtual void setOnSleep(std::function<bool()> cb) = 0;

    // Called when the system has woken from sleep.
    virtual void setOnWake(std::function<bool()> cb) = 0;

    // Called when the system is about to shut down.
    virtual void setOnShutdown(std::function<bool()> cb) = 0;

    // Blocks and runs the event loop. Returns only when the process is killed.
    virtual void run() = 0;

    // How long this platform will wait for us once a sleep has been announced.
    //
    // The question "how long have I got?" is answered by whatever raised the
    // event, not by a transport that has no idea why it is being called. Linux
    // holds a delay inhibitor and gives the same generous budget for every
    // event, so it does not override this. Windows cannot veto a suspend and
    // gives roughly two seconds — but minutes for shutdown, so only the sleep
    // path is constrained.
    //
    // Zero means "no platform limit beyond the transport's own default".
    virtual std::chrono::milliseconds sleepBudget() const {
        return std::chrono::milliseconds::zero();
    }

    virtual ~IPowerMonitor() = default;
};
