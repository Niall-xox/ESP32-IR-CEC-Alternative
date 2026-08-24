#include "HIDTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

static constexpr size_t REPORT_SIZE = 64;

// Everything send() does shares one wall-clock budget.
//
// logind's InhibitDelayMaxSec defaults to 5s. Once it expires systemd stops
// waiting for the delay inhibitor and suspends anyway — with the daemon still
// blocked mid-command, now running across a suspend. The old timeouts were
// independent rather than pooled, so the reopen path could spend 5s before the
// 2s ACK wait even started: 7s of blocking to honour a 5s guarantee. Deriving
// every step from one budget under the limit makes that impossible to hit.
static constexpr auto SEND_BUDGET = std::chrono::milliseconds(4000);

// Longest wait for a reply, further clamped to whatever is left of the budget.
static constexpr auto ACK_TIMEOUT = std::chrono::milliseconds(2000);

// The constructor does not run inside a power-event handler, so it can wait
// longer than SEND_BUDGET for the device to finish enumerating at boot.
static constexpr auto STARTUP_OPEN_TIMEOUT = std::chrono::seconds(3);

static constexpr auto OPEN_POLL = std::chrono::milliseconds(100);

// Bounds the drain loop so a device streaming reports cannot hold us there.
static constexpr int MAX_DRAIN_REPORTS = 64;

HIDTransport::HIDTransport(uint16_t vid, uint16_t pid) : vid_(vid), pid_(pid) {
    if (hid_init() != 0) {
        throw std::runtime_error("hid_init() failed — no usable HID backend");
    }

    if (!ensureOpen(Clock::now() + STARTUP_OPEN_TIMEOUT)) {
        std::cout << "[transport] ESP32 not found at startup — will retry when needed\n";
    }
}

HIDTransport::~HIDTransport() {
    closeDevice();
    hid_exit();
}

void HIDTransport::closeDevice() {
    if (dev_) {
        hid_close(dev_);
        dev_ = nullptr;
    }
}

bool HIDTransport::ensureOpen(TimePoint deadline) {
    if (dev_) return true;

    while (true) {
        dev_ = hid_open(vid_, pid_, nullptr);
        if (dev_) {
            std::cout << "[transport] HID device opened (VID="
                      << std::hex << vid_ << " PID=" << pid_ << std::dec << ")\n";
            return true;
        }
        // Stop before a sleep that would overrun the deadline.
        if (Clock::now() + OPEN_POLL >= deadline) return false;
        std::this_thread::sleep_for(OPEN_POLL);
    }
}

void HIDTransport::drainInput() {
    uint8_t scratch[REPORT_SIZE];
    for (int i = 0; i < MAX_DRAIN_REPORTS; ++i) {
        // Zero timeout — return what is already queued, never block.
        if (hid_read_timeout(dev_, scratch, sizeof(scratch), 0) <= 0) return;
    }
    std::cerr << "[transport] Input queue still not empty after draining "
              << MAX_DRAIN_REPORTS << " reports\n";
}

uint8_t HIDTransport::nextSequence() {
    if (++seq_ == 0) seq_ = 1;
    return seq_;
}

bool HIDTransport::send(const std::string& cmd, std::chrono::milliseconds budget) {
    // Zero means the caller has no limit of its own. Anything larger than
    // SEND_BUDGET is clamped: a caller may tell us it has *less* time than the
    // default, never more, because the default is what keeps this call inside
    // logind's InhibitDelayMaxSec. Clamping rather than trusting the caller is
    // what stops the budget invariant from depending on every call site.
    const auto effective = (budget <= std::chrono::milliseconds::zero())
                               ? SEND_BUDGET
                               : std::min(budget, SEND_BUDGET);

    const TimePoint deadline = Clock::now() + effective;

    if (!ensureOpen(deadline)) {
        std::cerr << "[transport] ESP32 not found — skipping IR command: " << cmd << "\n";
        return false;
    }

    // Clear replies to commands we already gave up on, so they cannot be
    // mistaken for the answer to this one. The sequence check in awaitAck()
    // catches what slips through after this point; draining first keeps the
    // common case from having to.
    drainInput();

    const uint8_t seq = nextSequence();

    // txBuf[0] is the HID report ID — 0, as the descriptor declares no IDs.
    // The 64-byte payload follows: [0] sequence, [1..] NUL-terminated command.
    uint8_t txBuf[REPORT_SIZE + 1] = {0};
    txBuf[0] = 0x00;
    txBuf[1] = seq;
    std::memcpy(txBuf + 2, cmd.c_str(), std::min(cmd.size(), REPORT_SIZE - 2));

    if (hid_write(dev_, txBuf, sizeof(txBuf)) < 0) {
        std::cerr << "[transport] Write failed — reopening device\n";
        closeDevice();
        if (!ensureOpen(deadline)) {
            std::cerr << "[transport] ESP32 not found after reopen — skipping: " << cmd << "\n";
            return false;
        }
        if (hid_write(dev_, txBuf, sizeof(txBuf)) < 0) {
            std::cerr << "[transport] Write failed after reopen — skipping: " << cmd << "\n";
            closeDevice();
            return false;
        }
    }

    return awaitAck(seq, cmd, deadline);
}

bool HIDTransport::awaitAck(uint8_t seq, const std::string& cmd, TimePoint deadline) {
    // ACK_TIMEOUT bounds this wait on its own, and the shared deadline bounds it
    // again. A tightened budget therefore shortens the ACK wait automatically,
    // rather than needing a second platform-specific constant kept in step with
    // the first by hand.
    const TimePoint ackDeadline = std::min(deadline, Clock::now() + ACK_TIMEOUT);

    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            ackDeadline - Clock::now());
        if (remaining.count() <= 0) break;

        uint8_t rxBuf[REPORT_SIZE] = {0};
        const int res = hid_read_timeout(dev_, rxBuf, sizeof(rxBuf),
                                         static_cast<int>(remaining.count()));

        if (res < 0) {
            // The handle is dead — usually the device was unplugged mid-command.
            // Drop it so the next send() reopens rather than failing again.
            std::cerr << "[transport] Read failed — dropping device handle\n";
            closeDevice();
            return false;
        }
        if (res == 0) break;  // nothing arrived before the deadline

        // rxBuf[0] echoes the sequence of the command being answered. Anything
        // else is a reply to a command we already gave up on: discard it and
        // keep waiting for ours instead of counting it as success.
        if (res < 4 || rxBuf[0] != seq) {
            std::cerr << "[transport] Discarding unmatched reply (seq "
                      << static_cast<int>(rxBuf[0]) << ", expected "
                      << static_cast<int>(seq) << ") — stale reply, or firmware "
                         "too old for the sequenced protocol\n";
            continue;
        }

        const char* body = reinterpret_cast<const char*>(rxBuf) + 1;

        if (std::strncmp(body, "ACK", 3) == 0) {
            return true;  // IR signal confirmed transmitted
        }
        if (std::strncmp(body, "ERR", 3) == 0) {
            std::cerr << "[transport] ESP32 returned ERR for command: " << cmd
                      << " — command unknown, or the active profile has no code"
                         " configured for this direction\n";
            return false;
        }

        std::cerr << "[transport] Unexpected response for command: " << cmd << "\n";
        return false;
    }

    std::cerr << "[transport] No response received for command: " << cmd << "\n";
    return false;
}
