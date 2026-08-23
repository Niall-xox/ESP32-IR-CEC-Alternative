// ESP32 IR Remote — PC Daemon
//
// Monitors system power events and sends ON/OFF commands to the ESP32
// over USB HID. Uses a systemd inhibitor lock to delay sleep and shutdown
// until the ESP32 confirms the IR signal has been transmitted via ACK.
//
// Boot     → send ON  (TV on when PC boots; skipped on a mere service restart)
// Sleep    → send OFF, wait for ACK, release inhibitor lock
// Wake     → send ON  (no lock needed — system is already running)
// Shutdown → send OFF, wait for ACK, release inhibitor lock

#include "IPowerMonitor.h"
#include "ITransport.h"
#include "HIDTransport.h"

#ifdef __linux__
#include "LinuxPowerMonitor.h"
#elif defined(_WIN32)
#include "WindowsPowerMonitor.h"
#endif

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

// Must match DEVICE_VID and DEVICE_PID in the firmware.
// Development placeholder values — replace with a registered VID/PID before
// commercial release.
static constexpr uint16_t DEVICE_VID = 0x1234;
static constexpr uint16_t DEVICE_PID = 0x5678;

// How recently the machine must have booted for a service start to count as a
// boot. Generous on purpose: a slow machine that misses the window merely fails
// to turn the TV on, which the user can undo with their remote, whereas a tight
// window would make that the common case.
static constexpr double BOOT_WINDOW_SECONDS = 180.0;

// True if this process is starting because the machine booted, rather than
// because the service was restarted under a machine that was already up.
//
// The startup ON exists to mirror a boot. It also fired on every `systemctl
// restart`, package upgrade and Restart=on-failure, so a daemon that crashed at
// 3am turned the TV on — and a user who had deliberately switched the TV off
// while leaving the PC running had it switched back on under them.
static bool systemJustBooted() {
#ifdef __linux__
    std::ifstream uptimeFile("/proc/uptime");
    double uptimeSeconds = 0.0;
    if (uptimeFile >> uptimeSeconds) {
        return uptimeSeconds < BOOT_WINDOW_SECONDS;
    }
    // /proc unreadable — fall through to the old unconditional behaviour rather
    // than skipping the ON a real boot was owed.
#endif
    return true;
}

int main() {
    // Flush stdout on every write.
    //
    // std::cout is fully buffered whenever stdout is not a terminal, which is
    // exactly the case under systemd — stdout there is a pipe to the journal.
    // Left alone, log lines sit in a 4KB buffer for months of normal operation,
    // and anything written just before systemd SIGTERMs us on shutdown is lost
    // outright, including the OFF confirmation. std::cerr is unbuffered and
    // tied to std::cout, so errors always got through while successes vanished.
    // This daemon logs a handful of lines a day, so flushing every time costs
    // nothing.
    std::cout << std::unitbuf;

    // --- Transport ---
    std::unique_ptr<ITransport> transport;
    try {
        transport = std::make_unique<HIDTransport>(DEVICE_VID, DEVICE_PID);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 1;
    }

    // --- Power monitor ---
    std::unique_ptr<IPowerMonitor> monitor;

#ifdef __linux__
    try {
        monitor = std::make_unique<LinuxPowerMonitor>();
        std::cout << "[monitor] Connected to systemd-logind\n";
    } catch (const std::exception& e) {
        std::cerr << "[error] Failed to connect to D-Bus: " << e.what() << "\n";
        return 1;
    }
#elif defined(_WIN32)
    monitor = std::make_unique<WindowsPowerMonitor>();
    std::cout << "[monitor] Windows Service power monitor initialised\n";
#else
    std::cerr << "[error] No power monitor available for this platform\n";
    return 1;
#endif

    // --- Wire up callbacks ---
    // send() blocks until ACK is received from the ESP32, so by the time each
    // callback returns the IR signal has been confirmed transmitted.
    // LinuxPowerMonitor releases the inhibitor lock immediately after the callback.
    //
    // The outcome is always reported honestly. A failed command previously
    // logged as though it had succeeded, which sent debugging in entirely the
    // wrong direction — an unplugged ESP32 looked identical to a working one.
    auto report = [](const char* cmd, const char* event, bool ok) {
        if (ok) {
            std::cout << "[cmd] " << cmd << " sent and ACK received (" << event << ")\n";
        } else {
            std::cerr << "[cmd] " << cmd << " FAILED — no ACK, TV state not changed ("
                      << event << ")\n";
        }
    };

    monitor->setOnSleep([&]() {
        report("OFF", "sleep", transport->send("OFF"));
    });

    monitor->setOnWake([&]() {
        report("ON", "wake", transport->send("ON"));
    });

    monitor->setOnShutdown([&]() {
        report("OFF", "shutdown", transport->send("OFF"));
    });

    // Send ON at startup, but only when the service start actually corresponds
    // to the machine booting — see systemJustBooted().
    if (systemJustBooted()) {
        report("ON", "startup", transport->send("ON"));
    } else {
        std::cout << "[cmd] ON skipped — service restarted on an already-running "
                     "system, not a boot\n";
    }

    // Blocks here until the process is killed by systemd on shutdown.
    std::cout << "[sys] Daemon running\n";
    monitor->run();

    return 0;
}
