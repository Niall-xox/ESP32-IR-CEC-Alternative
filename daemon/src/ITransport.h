#pragma once
#include <string>

// Abstract interface for sending commands to the ESP32.
//
// Concrete implementations:
//   SerialTransport  — USB CDC Serial, Phase 1
//   HIDTransport     — USB HID, Phase 2
//
// main.cpp only interacts through this interface so the transport
// implementation can be swapped without touching any other code.
class ITransport {
public:
    // Send a command string to the ESP32.
    // Implementations handle any framing required by the protocol (e.g. '\n').
    virtual void send(const std::string& cmd) = 0;

    virtual ~ITransport() = default;
};
