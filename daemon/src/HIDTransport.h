#pragma once
#include "ITransport.h"
#include <hidapi/hidapi.h>
#include <chrono>
#include <cstdint>
#include <string>

// ITransport implementation using USB HID (Phase 2).
// Finds the ESP32 by VID/PID, sends commands as 64-byte output reports,
// and waits for an ACK or ERR input report before returning.
//
// send() is synchronous — it blocks until the ESP32 responds. This allows
// the daemon to release the systemd inhibitor lock immediately after send()
// returns, knowing the IR signal has actually been transmitted.
//
// If the device is not present (e.g. not plugged in), send() logs a warning
// and returns silently rather than throwing, so the system can still sleep
// or shut down normally without the TV being controlled.
class HIDTransport : public ITransport {
public:
    HIDTransport(uint16_t vid, uint16_t pid);
    ~HIDTransport() override;

    // Sends cmd as a HID output report and blocks until ACK is received.
    // If the device is absent, logs a warning and returns without throwing.
    // If the device disappears mid-send (e.g. after wake), it is reopened and retried.
    void send(const std::string& cmd) override;

private:
    // Tries to open the HID device, polling until found or timeout expires.
    // Returns true if the device was opened successfully, false on timeout.
    bool tryOpenDevice(std::chrono::seconds timeout);

    uint16_t vid_;
    uint16_t pid_;
    hid_device* dev_ = nullptr;
};
