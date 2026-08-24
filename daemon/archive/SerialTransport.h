#pragma once
#include "ITransport.h"
#include <string>

// ITransport implementation using POSIX serial over USB CDC.
//
// Not compiled into the daemon. Retained as the supported path for ESP32 boards
// that cannot present as USB HID — the S3's native USB is what makes HID
// possible, and a classic ESP32 or an ESP8266 behind a USB-UART bridge has only
// CDC. Swapping HIDTransport for this in main.cpp is the whole change needed;
// nothing else in the daemon knows which transport it has.
//
// The ESP32-S3 appears as /dev/ttyACM0 when connected via its native USB port.
//
// Weaker than HIDTransport by nature, and the difference matters: CDC here is
// one-way, so a successful write is the only confirmation available. `true`
// therefore means "the bytes left the host", not "the IR signal fired" — the
// daemon will release the inhibitor lock on that weaker evidence. A device
// using this transport should be assumed to have no delivery guarantee.
class SerialTransport : public ITransport {
public:
    // Opens and configures the serial port at the given path.
    // Throws std::runtime_error if the port cannot be opened.
    explicit SerialTransport(const std::string& port);
    ~SerialTransport() override;

    // Writes cmd + '\n' to the serial port, reopening it first if the previous
    // write failed (e.g. the port disappeared across a system resume).
    //
    // Returns false — never throws — if the port cannot be reached within the
    // send budget, as ITransport requires: this runs inside a power-event
    // handler, where an exception would escape into the caller's event loop and
    // a long retry would outlive the inhibitor lock it is delaying.
    bool send(const std::string& cmd) override;

private:
    // Opens and configures the port. Returns true on success.
    bool openPort();

    // Applies 115200 8N1 raw mode termios settings to fd_.
    void configurePort();

    std::string port_;  // stored so the port can be reopened after wake
    int fd_ = -1;
};
