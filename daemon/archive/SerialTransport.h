#pragma once
#include "ITransport.h"
#include <string>

class SerialTransport : public ITransport {
public:

    explicit SerialTransport(const std::string& port);
    ~SerialTransport() override;

    bool send(const std::string& cmd,
              std::chrono::milliseconds budget = std::chrono::milliseconds::zero()) override;

private:

    bool openPort();

    void configurePort();

    std::string port_;
    int fd_ = -1;
};
