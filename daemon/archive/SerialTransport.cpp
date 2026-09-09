#include "SerialTransport.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

static constexpr auto SEND_BUDGET = std::chrono::milliseconds(4000);

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

    fd_ = open(port_.c_str(), O_WRONLY | O_NOCTTY | O_NDELAY);
    if (fd_ < 0) return false;

    fcntl(fd_, F_SETFL, 0);
    configurePort();
    return true;
}

void SerialTransport::configurePort() {

    struct termios options{};
    tcgetattr(fd_, &options);

    cfsetospeed(&options, B115200);

    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= CLOCAL | CREAD;

    options.c_oflag &= ~OPOST;

    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    tcsetattr(fd_, TCSANOW, &options);
}

bool SerialTransport::send(const std::string& cmd) {
    const std::string msg = cmd + "\n";
    const auto deadline = std::chrono::steady_clock::now() + SEND_BUDGET;

    while (true) {
        if (fd_ >= 0) {
            const ssize_t written = write(fd_, msg.c_str(), msg.size());

            if (written == (ssize_t)msg.size()) return true;

            if (written > 0) {
                std::cerr << "[transport] Short write (" << written << " of "
                          << msg.size() << " bytes) — reopening\n";
            } else {
                std::cerr << "[transport] Write failed, port stale — reopening\n";
            }
            close(fd_);
            fd_ = -1;
        }

        if (std::chrono::steady_clock::now() + REOPEN_POLL_INTERVAL >= deadline) {
            std::cerr << "[transport] Serial port unavailable within budget — "
                         "skipping command: " << cmd << " (" << port_ << ")\n";
            return false;
        }

        if (!openPort()) {
            std::this_thread::sleep_for(REOPEN_POLL_INTERVAL);
        }
    }
}
