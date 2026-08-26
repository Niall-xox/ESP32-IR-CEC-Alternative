#pragma once
#include <chrono>
#include <string>

// Abstract interface for sending commands to the ESP32.
//
// Concrete implementations:
//   HIDTransport     — USB HID; the one that is compiled in
//   SerialTransport  — USB CDC serial; archived, for boards without HID
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
    //
    // `budget` is the wall-clock time the caller can afford to block for, which
    // is a property of the *event* rather than of the transport: it is however
    // long this OS will wait for us before proceeding without us. Zero means
    // "no caller-imposed limit" and the transport applies its own default.
    // IPowerMonitor supplies the value — see IPowerMonitor::sleepBudget().
    virtual bool send(const std::string& cmd,
                      std::chrono::milliseconds budget = std::chrono::milliseconds::zero()) = 0;

    // Drop any cached connection, so the next send() opens the device afresh.
    //
    // For when something outside the transport knows the device has gone — a
    // Windows device-removal notification — rather than leaving it to be
    // discovered by the next write failing and reopening. That path still
    // works and is still the only one on Linux; this makes the Windows log say
    // *why* the handle was dropped, on the one path where a device vanishing
    // and coming back is routine rather than exceptional.
    //
    // Not pure: a transport holding no connection state has nothing to do here.
    //
    // Must be called on the same thread as send(). Implementations are not
    // required to be thread-safe and HIDTransport is not — WindowsPowerMonitor
    // routes this through its worker for that reason.
    virtual void invalidate() {}

    virtual ~ITransport() = default;
};
