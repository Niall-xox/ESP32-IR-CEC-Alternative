#pragma once
#ifdef _WIN32

#include "IPowerMonitor.h"
#include <functional>
#include <windows.h>

// IPowerMonitor implementation for Windows.
//
// Runs the daemon as a Windows Service, which is required to receive power
// events (sleep, wake, shutdown) reliably from the OS. A plain background
// exe does not receive all event types — services get dedicated callbacks
// via the service control handler.
//
// Power events received:
//   PBT_APMSUSPEND          → sleep about to begin   → fires onSleep_
//   PBT_APMRESUMESUSPEND    → woke from sleep         → fires onWake_
//   SERVICE_CONTROL_PRESHUTDOWN → shutdown imminent   → fires onShutdown_
//
// The inhibitor equivalent on Windows is handled implicitly: the service
// control handler must return within a few seconds, and the OS waits for it.
// We block inside the handler until send() returns (ACK received), which
// achieves the same guarantee as the Linux inhibitor lock.
class WindowsPowerMonitor : public IPowerMonitor {
public:
    WindowsPowerMonitor();
    ~WindowsPowerMonitor() override = default;

    void setOnSleep(std::function<void()> cb) override;
    void setOnWake(std::function<void()> cb) override;
    void setOnShutdown(std::function<void()> cb) override;

    // Registers and starts the Windows Service control dispatcher.
    // Blocks until the service is stopped by the OS.
    void run() override;

    // Called by the static service entry point and control handler.
    // These must be accessible from the static callbacks.
    void serviceMain(DWORD argc, LPWSTR* argv);
    void serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData);

private:
    void setServiceStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0);

    std::function<void()> onSleep_;
    std::function<void()> onWake_;
    std::function<void()> onShutdown_;

    SERVICE_STATUS        status_        = {};
    SERVICE_STATUS_HANDLE statusHandle_  = nullptr;
};

#endif // _WIN32
