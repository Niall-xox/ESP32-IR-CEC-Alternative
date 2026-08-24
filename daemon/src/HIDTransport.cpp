#include "HIDTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
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

// The USB product string the firmware advertises, used to tell our device from
// anything else answering to the same VID/PID. Not a substitute for a
// registered pair — a string is no more unique than the numbers are — but it
// distinguishes "the first device the OS happened to enumerate" from "the one
// that says it is ours", which is the difference between a coin flip and a
// choice. Must match USB.productName() in firmware/src/main.cpp.
static constexpr wchar_t EXPECTED_PRODUCT[] = L"ESP32 IR Remote";

// Narrows a hidapi wide string for logging. Descriptor strings here are ASCII;
// anything else becomes '?' rather than dragging locale conversion into a log
// line. Handles both 2-byte and 4-byte wchar_t, so it is the same code on both
// platforms.
static std::string narrow(const wchar_t* w) {
    if (!w) return "(none)";
    std::string out;
    for (const wchar_t* p = w; *p; ++p) {
        const auto c = static_cast<unsigned long>(*p);
        out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    return out;
}

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

hid_device* HIDTransport::openMatching() {
    hid_device_info* list = hid_enumerate(vid_, pid_);
    if (!list) return nullptr;  // nothing present; the caller retries

    int count = 0;
    for (const hid_device_info* d = list; d; d = d->next) ++count;

    // One match is the ordinary case and takes exactly the path it always did.
    if (count == 1) {
        hid_free_enumeration(list);
        return hid_open(vid_, pid_, nullptr);
    }

    // More than one device answers to this VID/PID, and hid_open() would take
    // whichever the OS enumerated first — a choice that silently changes on
    // every replug. Naming them all turns "it works sometimes" into something
    // with a cause attached.
    std::cerr << "[transport] " << count << " devices match VID="
              << std::hex << vid_ << " PID=" << pid_ << std::dec
              << " — the placeholder IDs are not unique. Candidates:\n";
    for (const hid_device_info* d = list; d; d = d->next) {
        std::cerr << "[transport]   product=\"" << narrow(d->product_string)
                  << "\" manufacturer=\"" << narrow(d->manufacturer_string)
                  << "\" interface=" << d->interface_number
                  << " usage_page=" << std::hex << d->usage_page
                  << " usage=" << d->usage << std::dec
                  << " path=" << (d->path ? d->path : "(none)") << "\n";
    }

    const hid_device_info* chosen = nullptr;
    for (const hid_device_info* d = list; d; d = d->next) {
        if (d->product_string && std::wcscmp(d->product_string, EXPECTED_PRODUCT) == 0) {
            chosen = d;
            break;
        }
    }

    hid_device* opened = nullptr;
    if (chosen) {
        std::cerr << "[transport] Selecting the one identifying as \""
                  << narrow(EXPECTED_PRODUCT) << "\"\n";
        opened = hid_open_path(chosen->path);
    } else {
        // Nothing claims to be us. Falling back to the first match keeps the
        // old behaviour rather than refusing to run, but it is a guess and
        // says so.
        std::cerr << "[transport] None identifies as \"" << narrow(EXPECTED_PRODUCT)
                  << "\" — falling back to the first match, which may not be"
                     " the ESP32\n";
        opened = hid_open(vid_, pid_, nullptr);
    }

    hid_free_enumeration(list);
    return opened;
}

// Dumps the report descriptor as the *host* parsed it, which is not necessarily
// the one the firmware meant to publish.
//
// Windows derives the required output-report length from this and rejects a
// write that does not match, so a descriptor fetched during a window where the
// device was not ready produces a handle that opens cleanly and then fails
// every write. It is cached against the device instance, so it persists across
// process restarts and clears only on re-enumeration — an intermittency that
// follows replugs rather than time.
//
// The firmware's descriptor is 34 bytes and ends 0x91 0x02 0xC0: an Output item
// then end-collection. A short dump, or one with no 0x91, is the host holding a
// descriptor with no output report — nowhere to write to.
void HIDTransport::logReportDescriptor() {
#if defined(HID_API_VERSION_MAJOR) && \
    (HID_API_VERSION_MAJOR > 0 || HID_API_VERSION_MINOR >= 14)
    unsigned char desc[256];
    const int n = hid_get_report_descriptor(dev_, desc, sizeof(desc));
    if (n < 0) {
        std::cerr << "[transport] Could not read the report descriptor ("
                  << narrow(hid_error(dev_)) << ")\n";
        return;
    }

    std::cerr << "[transport] Report descriptor as the host parsed it (" << n
              << " bytes, firmware publishes 34):";
    for (int i = 0; i < n; ++i) {
        std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(desc[i]);
    }
    std::cerr << std::dec << std::setfill(' ') << "\n";

    bool hasOutput = false;
    for (int i = 0; i + 1 < n; ++i) {
        if (desc[i] == 0x91) { hasOutput = true; break; }
    }
    if (!hasOutput) {
        std::cerr << "[transport] No Output item (0x91) in that descriptor — the host"
                     " believes this device cannot be written to. Re-enumerate the"
                     " device (unplug and replug) and check the firmware starts HID"
                     " before USB.\n";
    }
#else
    std::cerr << "[transport] hidapi is older than 0.14 — cannot report the"
                 " descriptor the host parsed\n";
#endif
}

bool HIDTransport::ensureOpen(TimePoint deadline) {
    if (dev_) return true;

    while (true) {
        dev_ = openMatching();
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
        // Report what the backend said. "Write failed" alone cannot distinguish
        // a device unplugged mid-command from a handle opened onto something
        // that is not the ESP32 at all, and those want opposite responses.
        std::cerr << "[transport] Write failed (" << narrow(hid_error(dev_))
                  << ") — reopening device\n";
        logReportDescriptor();
        closeDevice();
        if (!ensureOpen(deadline)) {
            std::cerr << "[transport] ESP32 not found after reopen — skipping: " << cmd << "\n";
            return false;
        }
        if (hid_write(dev_, txBuf, sizeof(txBuf)) < 0) {
            std::cerr << "[transport] Write failed after reopen ("
                      << narrow(hid_error(dev_)) << ") — skipping: " << cmd << "\n";
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
            std::cerr << "[transport] Read failed (" << narrow(hid_error(dev_))
                      << ") — dropping device handle\n";
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
