#pragma once
#include "IPowerMonitor.h"

#include <sdbus-c++/sdbus-c++.h>
#include <functional>
#include <memory>
#include <optional>

class LinuxPowerMonitor : public IPowerMonitor {
public:

    LinuxPowerMonitor();
    ~LinuxPowerMonitor() override;

    void setOnCommand(std::function<bool(const TvCommand&)> cb) override;

    void run() override;

private:

    bool takeInhibitorLock();

    void releaseInhibitorLock();

    void onPrepareForSleep(bool start);
    void onPrepareForShutdown(bool start);

    void assertTv(bool on, const char* reason);

    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IProxy>      proxy_;

    std::optional<sdbus::UnixFd> inhibitorFd_;

    std::function<bool(const TvCommand&)> onCommand_;
};
