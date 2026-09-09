#pragma once
#include <chrono>
#include <string>

class ITransport {
public:

    virtual bool send(const std::string& cmd,
                      std::chrono::milliseconds budget = std::chrono::milliseconds::zero()) = 0;

    virtual void invalidate() {}

    virtual ~ITransport() = default;
};
