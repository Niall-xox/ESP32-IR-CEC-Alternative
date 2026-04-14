// ESP32 IR Remote — PC Daemon
//
// Monitors system power events and sends ON/OFF commands to the ESP32
// over USB HID. Uses a systemd inhibitor lock to delay sleep and shutdown
// until the ESP32 confirms the IR signal has been transmitted via ACK.
//
// Startup  → send ON  (TV on when PC boots)
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

#include <iostream>
#include <memory>
#include <stdexcept>

// Must match DEVICE_VID and DEVICE_PID in the firmware.
// Development placeholder values — replace with a registered VID/PID before
// commercial release.
static constexpr uint16_t DEVICE_VID = 0x1234;
static constexpr uint16_t DEVICE_PID = 0x5678;

int main() {
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
    monitor->setOnSleep([&]() {
        transport->send("OFF");
        std::cout << "[cmd] OFF sent and ACK received (sleep)\n";
    });

    monitor->setOnWake([&]() {
        transport->send("ON");
        std::cout << "[cmd] ON sent and ACK received (wake)\n";
    });

    monitor->setOnShutdown([&]() {
        transport->send("OFF");
        std::cout << "[cmd] OFF sent and ACK received (shutdown)\n";
    });

    // Send ON at startup — the service starting means the PC just booted.
    transport->send("ON");
    std::cout << "[cmd] ON sent and ACK received (startup)\n";

    // Blocks here until the process is killed by systemd on shutdown.
    std::cout << "[sys] Daemon running\n";
    monitor->run();

    return 0;
}
