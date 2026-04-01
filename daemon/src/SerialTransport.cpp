#include "SerialTransport.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// How long to keep retrying if the port isn't available after wake.
static constexpr auto REOPEN_TIMEOUT = std::chrono::seconds(10);

// How long to wait between reopen attempts.
static constexpr auto REOPEN_POLL_INTERVAL = std::chrono::milliseconds(100);

SerialTransport::SerialTransport(const std::string& port) : port_(port) {
    if (!openPort()) {
        throw std::runtime_error("Failed to open serial port: " + port_);
    }
}

SerialTransport::~SerialTransport() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool SerialTransport::openPort() {
    // O_WRONLY  — send-only in Phase 1; no responses expected from the ESP32
    // O_NOCTTY  — don't let this port become the process's controlling terminal
    // O_NDELAY  — don't block if the device isn't ready yet
    fd_ = open(port_.c_str(), O_WRONLY | O_NOCTTY | O_NDELAY);
    if (fd_ < 0) return false;

    // Switch back to blocking mode for writes
    fcntl(fd_, F_SETFL, 0);
    configurePort();
    return true;
}

void SerialTransport::configurePort() {
    // Configure to match the firmware: 115200 baud, 8N1, raw mode
    struct termios options{};
    tcgetattr(fd_, &options);

    cfsetospeed(&options, B115200);

    options.c_cflag &= ~PARENB;         // no parity bit
    options.c_cflag &= ~CSTOPB;         // 1 stop bit
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;             // 8 data bits
    options.c_cflag |= CLOCAL | CREAD;  // ignore modem control lines

    // Raw output — no newline translation or other byte transformations
    options.c_oflag &= ~OPOST;

    // Raw input (write-only in Phase 1 but set for completeness)
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    tcsetattr(fd_, TCSANOW, &options);
}

void SerialTransport::send(const std::string& cmd) {
    std::string msg = cmd + "\n";

    auto deadline = std::chrono::steady_clock::now() + REOPEN_TIMEOUT;

    while (true) {
        if (fd_ >= 0) {
            ssize_t written = write(fd_, msg.c_str(), msg.size());
            if (written > 0) return;  // success

            // Write failed — port went stale (e.g. USB disconnected during sleep)
            std::cerr << "[transport] Write failed, port stale — reopening\n";
            close(fd_);
            fd_ = -1;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for serial port: " + port_);
        }

        // Poll until the port reappears (USB re-enumeration after wake)
        if (!openPort()) {
            std::this_thread::sleep_for(REOPEN_POLL_INTERVAL);
        }
    }
}
