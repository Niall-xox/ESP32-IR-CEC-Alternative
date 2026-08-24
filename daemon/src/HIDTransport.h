#pragma once
#include "ITransport.h"
#include <hidapi/hidapi.h>
#include <chrono>
#include <cstdint>
#include <string>

// ITransport implementation using USB HID.
//
// Finds the ESP32 by VID/PID, sends commands as 64-byte output reports, and
// waits for the matching ACK or ERR input report before returning.
//
// Wire format (both directions, 64-byte payload):
//   [0]    sequence byte — echoed by the device, never 0
//   [1..]  NUL-terminated ASCII: "ON" / "OFF" out, "ACK" / "ERR" back
//
// The sequence byte is what makes an ACK trustworthy. hidraw queues input
// reports, so a reply that arrived after send() gave up on it stayed buffered
// and satisfied the *next* command's read — returning true for an IR signal
// that command never sent. Replies whose sequence does not match the request
// are discarded rather than accepted.
//
// send() is synchronous — it blocks until the device responds. That is what
// lets the daemon release the systemd inhibitor lock the moment send() returns,
// knowing the IR signal has actually been transmitted. Every step it takes
// shares one wall-clock budget, deliberately smaller than logind's
// InhibitDelayMaxSec, so it cannot outlive the lock it is holding things up for.
//
// A caller that has less time than that — a Windows suspend, where the machine
// goes down about two seconds after the notification and nothing can delay it —
// passes a smaller budget. SEND_BUDGET in the .cpp remains the single knob for
// the default; the parameter only ever tightens it, never loosens it.
//
// If the device is missing or has been unplugged, send() attempts to reopen it,
// then logs and returns false rather than throwing — the system still has to be
// allowed to sleep or shut down.
class HIDTransport : public ITransport {
public:
    // Throws std::runtime_error if the HID backend cannot be initialised.
    // A device that is simply absent is not an error — send() retries.
    HIDTransport(uint16_t vid, uint16_t pid);
    ~HIDTransport() override;

    // Returns true only once a matching ACK has been received — see ITransport::send.
    // A zero budget means "use SEND_BUDGET"; a smaller one is honoured as given,
    // and a larger one is clamped back down to SEND_BUDGET.
    bool send(const std::string& cmd,
              std::chrono::milliseconds budget = std::chrono::milliseconds::zero()) override;

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // Opens the device if it is not already open, retrying until `deadline`.
    bool ensureOpen(TimePoint deadline);

    // One attempt at opening the right device. When several answer to the
    // VID/PID it names them all and prefers the one whose USB product string
    // identifies it, rather than taking whichever the OS enumerated first —
    // an order that changes on every replug.
    hid_device* openMatching();

    // Logs the report descriptor the *host* holds, which is what Windows
    // derives the required write length from. Called when a write fails,
    // because that is where the difference between it and the firmware's
    // descriptor explains the failure.
    void logReportDescriptor();

    void closeDevice();

    // Discards reports left over from commands that already timed out.
    void drainInput();

    // Next sequence value, wrapping 1..255. Never 0, so a zeroed buffer
    // cannot be mistaken for a valid reply.
    uint8_t nextSequence();

    // Reads until a reply carrying `seq` arrives, or the budget runs out.
    bool awaitAck(uint8_t seq, const std::string& cmd, TimePoint deadline);

    uint16_t    vid_;
    uint16_t    pid_;
    hid_device* dev_ = nullptr;
    uint8_t     seq_ = 0;
};
