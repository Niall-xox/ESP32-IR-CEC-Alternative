#pragma once
#include "ITransport.h"
#include <hidapi/hidapi.h>
#include <chrono>
#include <cstdint>
#include <string>

// ITransport implementation using USB HID.
// Finds the ESP32 by VID/PID, sends commands as 64-byte output reports,
// and waits for an ACK or ERR input report before returning.
//
// send() is synchronous — it blocks until the ESP32 responds. This allows
// the daemon to release the systemd inhibitor lock immediately after send()
// returns, knowing the IR signal has actually been transmitted.
//
// If the device is not present or has been unplugged, send() attempts to
// reopen it. If that fails, it logs a warning and returns silently so the
// system can still sleep or shut down normally.
class HIDTransport : public ITransport {
public:
    HIDTransport(uint16_t vid, uint16_t pid);
    ~HIDTransport() override;

    void send(const std::string& cmd) override;

private:
    bool tryOpenDevice(std::chrono::seconds timeout);

    uint16_t    vid_;
    uint16_t    pid_;
    hid_device* dev_ = nullptr;
};
