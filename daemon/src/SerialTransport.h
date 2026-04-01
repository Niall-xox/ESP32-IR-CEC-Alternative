#pragma once
#include "ITransport.h"
#include <string>

// ITransport implementation using POSIX serial over USB CDC (Phase 1).
// The ESP32-S3 appears as /dev/ttyACM0 when connected via its native USB port.
// Phase 2 will replace this with HIDTransport.
class SerialTransport : public ITransport {
public:
    // Opens and configures the serial port at the given path.
    // Throws std::runtime_error if the port cannot be opened.
    explicit SerialTransport(const std::string& port);
    ~SerialTransport() override;

    // Writes cmd + '\n' to the serial port.
    // If the write fails (e.g. port disappeared after system resume), the port
    // is reopened and the write retried until it succeeds or the timeout expires.
    void send(const std::string& cmd) override;

private:
    // Opens and configures the port. Returns true on success.
    bool openPort();

    // Applies 115200 8N1 raw mode termios settings to fd_.
    void configurePort();

    std::string port_;  // stored so the port can be reopened after wake
    int fd_ = -1;
};
