#pragma once
#include "ITransport.h"
#include <hidapi/hidapi.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// ITransport implementation using USB HID.
// Finds the ESP32 by VID/PID, sends commands as 64-byte output reports,
// and waits for an ACK or ERR input report before returning.
//
// send() is synchronous — it blocks until the ESP32 responds. This allows
// the daemon to release the systemd inhibitor lock immediately after send()
// returns, knowing the IR signal has actually been transmitted.
//
// A background reader thread listens for unsolicited input reports from the
// ESP32 (e.g. PING triggered by a button press) and responds immediately.
// This thread holds devMutex_ only for the brief duration of each read, so
// send() can acquire it freely between background reads.
//
// If the device is not present, send() logs a warning and returns silently
// so the system can still sleep or shut down normally.
class HIDTransport : public ITransport {
public:
    HIDTransport(uint16_t vid, uint16_t pid);
    ~HIDTransport() override;

    // Sends cmd as a HID output report and blocks until ACK/PONG received.
    // Safe to call concurrently with the background reader thread.
    void send(const std::string& cmd) override;

private:
    bool tryOpenDevice(std::chrono::seconds timeout);

    // Background reader loop — runs on readerThread_, responds to PING with PONG.
    void readerLoop();

    uint16_t              vid_;
    uint16_t              pid_;
    hid_device*           dev_     = nullptr;
    std::mutex            devMutex_;
    std::thread           readerThread_;
    std::atomic<bool>     running_{ false };
};
