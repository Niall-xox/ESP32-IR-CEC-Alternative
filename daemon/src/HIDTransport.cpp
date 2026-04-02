#include "HIDTransport.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

// Must match REPORT_SIZE, DEVICE_VID, and DEVICE_PID in the firmware.
static constexpr size_t REPORT_SIZE    = 64;
static constexpr int    ACK_TIMEOUT_MS = 2000;  // ms to wait for ACK after sending

// How long to wait for the device on startup and after wake.
// Kept short so the daemon starts quickly even when the ESP32 isn't plugged in.
static constexpr auto STARTUP_OPEN_TIMEOUT = std::chrono::seconds(3);

// How long to wait when reopening after a failed write (e.g. USB just re-enumerated).
static constexpr auto REOPEN_TIMEOUT = std::chrono::seconds(5);

static constexpr auto OPEN_POLL = std::chrono::milliseconds(100);

HIDTransport::HIDTransport(uint16_t vid, uint16_t pid) : vid_(vid), pid_(pid) {
    hid_init();
    // Try to open the device at startup, but don't fail if it's not found.
    // The device may not be plugged in — the daemon should still start cleanly.
    if (tryOpenDevice(STARTUP_OPEN_TIMEOUT)) {
        std::cout << "[transport] HID device opened (VID="
                  << std::hex << vid_ << " PID=" << pid_ << std::dec << ")\n";
    } else {
        std::cout << "[transport] ESP32 not found at startup — will retry when needed\n";
    }
}

HIDTransport::~HIDTransport() {
    if (dev_) {
        hid_close(dev_);
    }
    hid_exit();
}

bool HIDTransport::tryOpenDevice(std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        dev_ = hid_open(vid_, pid_, nullptr);
        if (dev_) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(OPEN_POLL);
    }
}

void HIDTransport::send(const std::string& cmd) {
    // If the device isn't open, try to find it before sending.
    if (!dev_) {
        if (!tryOpenDevice(STARTUP_OPEN_TIMEOUT)) {
            // Device not found — log and return so the system can proceed
            // with sleep/shutdown without being blocked by a missing ESP32.
            std::cerr << "[transport] ESP32 not found — skipping IR command: " << cmd << "\n";
            return;
        }
        std::cout << "[transport] HID device opened (VID="
                  << std::hex << vid_ << " PID=" << pid_ << std::dec << ")\n";
    }

    // Build the 64-byte output report.
    // hidapi requires the first byte to be the report ID (0 = no report ID).
    uint8_t txBuf[REPORT_SIZE + 1] = {0};
    txBuf[0] = 0x00;
    memcpy(txBuf + 1, cmd.c_str(), std::min(cmd.size(), REPORT_SIZE));

    // Attempt write. If it fails, the device may have disappeared after wake.
    // Reopen and retry once.
    int res = hid_write(dev_, txBuf, sizeof(txBuf));
    if (res < 0) {
        std::cerr << "[transport] Write failed — reopening device\n";
        hid_close(dev_);
        dev_ = nullptr;
        if (!tryOpenDevice(REOPEN_TIMEOUT)) {
            std::cerr << "[transport] ESP32 not found after reopen — skipping: " << cmd << "\n";
            return;
        }
        res = hid_write(dev_, txBuf, sizeof(txBuf));
        if (res < 0) {
            std::cerr << "[transport] Write failed after reopen — skipping: " << cmd << "\n";
            return;
        }
    }

    // Wait for ACK or ERR. The firmware sends ACK only after sendNEC() completes,
    // so receiving ACK guarantees the IR signal has been transmitted.
    uint8_t rxBuf[REPORT_SIZE] = {0};
    res = hid_read_timeout(dev_, rxBuf, sizeof(rxBuf), ACK_TIMEOUT_MS);

    if (res <= 0) {
        std::cerr << "[transport] No ACK received for command: " << cmd << "\n";
        return;
    }

    if (rxBuf[0] == 'A' && rxBuf[1] == 'C' && rxBuf[2] == 'K') {
        return;  // success
    }
    if (rxBuf[0] == 'E' && rxBuf[1] == 'R' && rxBuf[2] == 'R') {
        std::cerr << "[transport] ESP32 returned ERR for command: " << cmd << "\n";
        return;
    }

    std::cerr << "[transport] Unexpected response for command: " << cmd << "\n";
}
