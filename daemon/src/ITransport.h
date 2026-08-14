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
    //
    // Returns true only if the device confirmed the command was carried out —
    // for HIDTransport that means an ACK was received, which in turn means the
    // IR signal has finished transmitting. Returns false if the device was
    // missing, the write failed, the response timed out, or the device replied
    // ERR. A false return must not stop the caller from proceeding: the system
    // still has to be allowed to sleep or shut down.
    virtual bool send(const std::string& cmd) = 0;

    virtual ~ITransport() = default;
};
