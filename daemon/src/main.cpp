// ESP32 IR Remote — PC Daemon (Phase 1)
//
// Monitors system power events and sends ON/OFF commands to the ESP32
// over USB CDC Serial. Uses a systemd inhibitor lock to delay sleep and
// shutdown until the IR command has been transmitted.
//
// Startup  → send ON  (TV on when PC boots)
// Sleep    → send OFF (TV off when PC sleeps),   inhibitor lock released after
// Wake     → send ON  (TV on when PC wakes)
// Shutdown → send OFF (TV off when PC shuts down), inhibitor lock released after

#include "IPowerMonitor.h"
#include "ITransport.h"
#include "SerialTransport.h"

#ifdef __linux__
#include "LinuxPowerMonitor.h"
#endif

#include <iostream>
#include <memory>
#include <stdexcept>

// Default port used when none is provided on the command line.
// Phase 2 will replace this with VID/PID auto-detection.
static constexpr auto DEFAULT_SERIAL_PORT = "/dev/ttyACM0";

int main(int argc, char* argv[]) {
    // Accept an optional port argument: ./esp32-ir-daemon /dev/ttyACM1
    const std::string port = (argc > 1) ? argv[1] : DEFAULT_SERIAL_PORT;

    // --- Transport ---
    std::unique_ptr<ITransport> transport;
    try {
        transport = std::make_unique<SerialTransport>(port);
        std::cout << "[transport] Opened " << port << "\n";
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
#else
    std::cerr << "[error] No power monitor available for this platform\n";
    return 1;
#endif

    // --- Wire up callbacks ---
    monitor->setOnSleep([&]() {
        transport->send("OFF");
        std::cout << "[cmd] OFF sent (sleep)\n";
    });

    monitor->setOnWake([&]() {
        transport->send("ON");
        std::cout << "[cmd] ON sent (wake)\n";
    });

    monitor->setOnShutdown([&]() {
        transport->send("OFF");
        std::cout << "[cmd] OFF sent (shutdown)\n";
    });

    // Send ON at startup — the service starting means the PC just booted.
    transport->send("ON");
    std::cout << "[cmd] ON sent (startup)\n";

    // --- Run ---
    // Blocks here until the process is killed by systemd on shutdown.
    std::cout << "[sys] Daemon running\n";
    monitor->run();

    return 0;
}
