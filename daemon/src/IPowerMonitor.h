#pragma once
#include <chrono>
#include <functional>

// Abstract interface for monitoring PC power events.
//
// Concrete implementations:
//   LinuxPowerMonitor   — systemd-logind over D-Bus
//   WindowsPowerMonitor — Win32 Service API, driven by console display state
//
// Callers register one callback then call run(), which blocks and drives the
// underlying event loop until the process is killed.

// One assertion about what the TV should be doing, and everything the caller
// needs to carry it out honestly.
//
// This used to be three separate callbacks — sleep, wake, shutdown — with the
// time budget supplied by a pair of virtuals on this interface. That shape put
// the budget on the *event class* rather than on the event, and the difference
// stopped being academic the moment Windows started driving the TV from console
// display state: a screen blanking was routed through the sleep callback and
// therefore inherited a suspend's 1.5-second budget, on a path where nothing
// was waiting for the daemon at all. It also logged itself as "(sleep)", which
// is a plausible lie in the log of the kind the last two hardening passes were
// spent removing.
//
// Both defects came from the same place: an event's *reason* and its *deadline*
// are known only to whatever raised it, so they have to travel with the call
// rather than be inferred from which of three functions was chosen.
struct TvCommand {
    // What the TV should be. The commands are idempotent assertions rather than
    // instructions to toggle, which is why the IR codes are discrete.
    bool on;

    // Names the cause, for the log. Not decorative: "OFF (sleep)" and
    // "OFF (display off)" are different claims about why the TV went off, and
    // only one of them is true on any given line.
    const char* reason;

    // Wall-clock time the caller can afford to block for — however long this OS
    // will wait before proceeding without us, for *this* event. Zero means "no
    // limit beyond the transport's own default", which is what every Linux
    // event and every unhurried Windows one answers.
    std::chrono::milliseconds budget;
};

class IPowerMonitor {
public:
    // Called whenever the TV's state should be asserted.
    //
    // Returns whether the device *confirmed* the command — the same guarantee
    // ITransport::send() carries.
    //
    // An implementation that suppresses redundant commands needs that answer.
    // Recording an attempt that was never acknowledged as the TV's new state
    // would suppress the retry and leave it wrong indefinitely, which is the
    // "reports success regardless" failure in a new place. An implementation
    // that keeps no state may ignore the result — LinuxPowerMonitor does,
    // because logind tells it what happened and it sends on every event.
    virtual void setOnCommand(std::function<bool(const TvCommand&)> cb) = 0;

    // Called when the ESP32 itself appears or disappears, on a platform that
    // can say so. Strictly this is not a power event — but on Windows it
    // arrives through the very same channel, a service control handler
    // receiving device and power notifications alike, and giving it its own
    // notification path would mean a second platform conditional in main.cpp
    // for one callback.
    //
    // Not pure, and a no-op by default. A platform with no device notification
    // simply never calls it, and the transport goes on discovering a vanished
    // device the way it always has — by failing a write and reopening.
    // LinuxPowerMonitor does not implement it; udev is the equivalent there if
    // it ever earns its place.
    //
    // Implementations must invoke it on the same thread they invoke the command
    // callback on, because the handler is expected to touch the transport and
    // transports are not required to be thread-safe.
    virtual void setOnDeviceChange(std::function<void(bool present)> /*cb*/) {}

    // Blocks and runs the event loop. Returns only when the process is killed.
    virtual void run() = 0;

    virtual ~IPowerMonitor() = default;
};
