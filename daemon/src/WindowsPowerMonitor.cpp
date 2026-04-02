#ifdef _WIN32

#include "WindowsPowerMonitor.h"
#include <iostream>

// The service name must match what was used in `sc create` when installing.
static constexpr wchar_t SERVICE_NAME[] = L"esp32-ir-remote";

// Global pointer to the single instance, needed for the static service callbacks.
// Windows requires a plain function pointer (no captures) for the service entry
// and control handler, so we route them through this global to the instance.
static WindowsPowerMonitor* g_instance = nullptr;

// Static entry point called by the Service Control Manager when the service starts.
// Forwards to the instance method so the rest of the implementation is non-static.
static VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    if (g_instance) g_instance->serviceMain(argc, argv);
}

// Static control handler called by the SCM for control events (stop, power, etc).
static DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD eventType,
                                         LPVOID eventData, LPVOID context) {
    if (g_instance) g_instance->serviceCtrlHandler(control, eventType, eventData);
    return NO_ERROR;
}

WindowsPowerMonitor::WindowsPowerMonitor() {
    g_instance = this;
}

void WindowsPowerMonitor::setOnSleep(std::function<void()> cb)    { onSleep_    = std::move(cb); }
void WindowsPowerMonitor::setOnWake(std::function<void()> cb)     { onWake_     = std::move(cb); }
void WindowsPowerMonitor::setOnShutdown(std::function<void()> cb) { onShutdown_ = std::move(cb); }

void WindowsPowerMonitor::run() {
    // The dispatch table tells the SCM which function to call when starting the service.
    // The table is null-terminated.
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), ServiceMain },
        { nullptr, nullptr }
    };

    // StartServiceCtrlDispatcher blocks here until the service stops.
    // It connects this process to the SCM and hands off control.
    if (!StartServiceCtrlDispatcherW(dispatchTable)) {
        std::cerr << "[error] StartServiceCtrlDispatcher failed: " << GetLastError() << "\n";
    }
}

void WindowsPowerMonitor::serviceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // Register the control handler. RegisterServiceCtrlHandlerExW is used
    // (rather than the basic RegisterServiceCtrlHandlerW) because we need
    // the extended handler signature to receive power event details.
    statusHandle_ = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
    if (!statusHandle_) {
        std::cerr << "[error] RegisterServiceCtrlHandlerEx failed: " << GetLastError() << "\n";
        return;
    }

    // Tell the SCM the service is starting.
    status_.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    status_.dwCurrentState            = SERVICE_START_PENDING;
    status_.dwControlsAccepted        = 0;
    status_.dwWin32ExitCode           = NO_ERROR;
    status_.dwServiceSpecificExitCode = 0;
    status_.dwCheckPoint              = 0;
    status_.dwWaitHint                = 3000;
    SetServiceStatus(statusHandle_, &status_);

    // Tell the SCM which control codes we want to receive:
    //   SERVICE_ACCEPT_STOP            — allows the service to be stopped
    //   SERVICE_ACCEPT_PRESHUTDOWN     — called early in shutdown, before SESSION_CHANGE,
    //                                    giving us time to send the IR command before the
    //                                    system powers off. Preferred over ACCEPT_SHUTDOWN
    //                                    which fires too late for reliable USB communication.
    //   SERVICE_ACCEPT_POWEREVENT      — delivers sleep and wake events
    status_.dwCurrentState     = SERVICE_RUNNING;
    status_.dwControlsAccepted = SERVICE_ACCEPT_STOP
                               | SERVICE_ACCEPT_PRESHUTDOWN
                               | SERVICE_ACCEPT_POWEREVENT;
    setServiceStatus(SERVICE_RUNNING);

    std::cout << "[monitor] Windows Service running\n";

    // Block the service main thread with a wait event.
    // The control handler fires on a separate SCM thread, so all callback work
    // happens there. This thread just needs to stay alive.
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForSingleObject(stopEvent, INFINITE);
    CloseHandle(stopEvent);
}

void WindowsPowerMonitor::serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID /*eventData*/) {
    switch (control) {

    case SERVICE_CONTROL_PRESHUTDOWN:
        // System is shutting down. We block here until send() returns (ACK received),
        // then report stopped. The SCM waits for us before proceeding with shutdown,
        // which is the Windows equivalent of the Linux inhibitor lock.
        std::cout << "[event] Shutting down\n";
        try { if (onShutdown_) onShutdown_(); } catch (...) {}
        setServiceStatus(SERVICE_STOPPED);
        break;

    case SERVICE_CONTROL_POWEREVENT:
        switch (eventType) {

        case PBT_APMSUSPEND:
            // System is about to sleep. Block until ACK received before returning —
            // the OS waits for the handler to return before suspending.
            std::cout << "[event] Going to sleep\n";
            try { if (onSleep_) onSleep_(); } catch (...) {}
            break;

        case PBT_APMRESUMESUSPEND:
            // System just woke from sleep. No blocking required — system is running.
            std::cout << "[event] Woke up\n";
            try { if (onWake_) onWake_(); } catch (...) {}
            break;
        }
        break;

    case SERVICE_CONTROL_STOP:
        std::cout << "[monitor] Service stop requested\n";
        setServiceStatus(SERVICE_STOPPED);
        break;
    }
}

void WindowsPowerMonitor::setServiceStatus(DWORD state, DWORD exitCode, DWORD waitHint) {
    status_.dwCurrentState  = state;
    status_.dwWin32ExitCode = exitCode;
    status_.dwWaitHint      = waitHint;
    SetServiceStatus(statusHandle_, &status_);
}

#endif // _WIN32
