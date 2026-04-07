#include "HIDTransport.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

static constexpr size_t REPORT_SIZE    = 64;
static constexpr int    ACK_TIMEOUT_MS = 2000;

static constexpr auto STARTUP_OPEN_TIMEOUT = std::chrono::seconds(3);
static constexpr auto REOPEN_TIMEOUT       = std::chrono::seconds(5);
static constexpr auto OPEN_POLL            = std::chrono::milliseconds(100);

// How often the background reader polls for input reports from the ESP32.
// Short enough to respond to PING promptly, long enough not to waste CPU.
static constexpr auto READER_POLL_MS = std::chrono::milliseconds(50);

HIDTransport::HIDTransport(uint16_t vid, uint16_t pid) : vid_(vid), pid_(pid) {
    hid_init();

    if (tryOpenDevice(STARTUP_OPEN_TIMEOUT)) {
        std::cout << "[transport] HID device opened (VID="
                  << std::hex << vid_ << " PID=" << pid_ << std::dec << ")\n";
    } else {
        std::cout << "[transport] ESP32 not found at startup — will retry when needed\n";
    }

    // Start the background reader thread.
    // It listens for input reports from the ESP32 (e.g. button-triggered PING)
    // and responds immediately without blocking the power event path.
    running_ = true;
    readerThread_ = std::thread(&HIDTransport::readerLoop, this);
}

HIDTransport::~HIDTransport() {
    // Signal the reader thread to stop and wait for it to exit cleanly
    running_ = false;
    if (readerThread_.joinable()) readerThread_.join();

    std::lock_guard<std::mutex> lock(devMutex_);
    if (dev_) hid_close(dev_);
    hid_exit();
}

bool HIDTransport::tryOpenDevice(std::chrono::seconds timeout) {
    // tryOpenDevice is only called from the constructor (before the reader thread
    // starts) and from send() (which holds devMutex_), so no lock needed here.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        dev_ = hid_open(vid_, pid_, nullptr);
        if (dev_) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(OPEN_POLL);
    }
}

// ---------------------------------------------------------------------------
// Background reader loop
//
// Polls for input reports from the ESP32 using a non-blocking read (timeout=0).
// When a PING is received, responds immediately with PONG as an output report.
//
// The mutex is held only for the duration of each read/write pair, releasing
// between iterations so send() can acquire it freely.
// ---------------------------------------------------------------------------
void HIDTransport::readerLoop() {
    uint8_t rxBuf[REPORT_SIZE] = {0};

    while (running_) {
        {
            std::lock_guard<std::mutex> lock(devMutex_);

            if (dev_) {
                // Non-blocking read — returns immediately if no report available
                int res = hid_read_timeout(dev_, rxBuf, sizeof(rxBuf), 0);

                if (res > 0 &&
                    rxBuf[0] == 'P' && rxBuf[1] == 'I' &&
                    rxBuf[2] == 'N' && rxBuf[3] == 'G') {

                    // Respond with PONG as a 64-byte output report.
                    // hidapi requires the first byte to be the report ID (0).
                    uint8_t txBuf[REPORT_SIZE + 1] = {0};
                    txBuf[1] = 'P'; txBuf[2] = 'O'; txBuf[3] = 'N'; txBuf[4] = 'G';

                    int written = hid_write(dev_, txBuf, sizeof(txBuf));
                    if (written < 0) {
                        std::cerr << "[transport] Failed to send PONG\n";
                    }
                }
            }
        }
        // Sleep between polls — releases the mutex so send() isn't starved
        std::this_thread::sleep_for(READER_POLL_MS);
    }
}

// ---------------------------------------------------------------------------
// send()
//
// Acquires the device mutex, writes the command, waits for a response.
// The background reader thread sleeps between polls, so the mutex is almost
// always available immediately.
// ---------------------------------------------------------------------------
void HIDTransport::send(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(devMutex_);

    if (!dev_) {
        if (!tryOpenDevice(STARTUP_OPEN_TIMEOUT)) {
            std::cerr << "[transport] ESP32 not found — skipping IR command: " << cmd << "\n";
            return;
        }
        std::cout << "[transport] HID device opened (VID="
                  << std::hex << vid_ << " PID=" << pid_ << std::dec << ")\n";
    }

    uint8_t txBuf[REPORT_SIZE + 1] = {0};
    txBuf[0] = 0x00;
    memcpy(txBuf + 1, cmd.c_str(), std::min(cmd.size(), REPORT_SIZE));

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

    uint8_t rxBuf[REPORT_SIZE] = {0};
    res = hid_read_timeout(dev_, rxBuf, sizeof(rxBuf), ACK_TIMEOUT_MS);

    if (res <= 0) {
        std::cerr << "[transport] No response received for command: " << cmd << "\n";
        return;
    }

    if (rxBuf[0] == 'A' && rxBuf[1] == 'C' && rxBuf[2] == 'K') {
        return;  // success
    }
    if (rxBuf[0] == 'P' && rxBuf[1] == 'O' && rxBuf[2] == 'N' && rxBuf[3] == 'G') {
        return;  // PING/PONG exchange
    }
    if (rxBuf[0] == 'E' && rxBuf[1] == 'R' && rxBuf[2] == 'R') {
        std::cerr << "[transport] ESP32 returned ERR for command: " << cmd << "\n";
        return;
    }

    std::cerr << "[transport] Unexpected response for command: " << cmd << "\n";
}
