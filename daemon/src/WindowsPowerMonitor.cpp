#ifdef _WIN32

#include "WindowsPowerMonitor.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

// The service name must match what was used in `sc create` when installing —
// see packaging/windows/install-service.ps1.
static constexpr wchar_t SERVICE_NAME[] = L"esp32-ir-remote";

// GUID_CONSOLE_DISPLAY_STATE, spelled out rather than taken from the SDK.
//
// The SDK declares the power-setting GUIDs but leaves their definitions in a
// library that has to be linked for them to resolve, which differs between SDK
// versions and is a link error waiting to happen for one constant. The value is
// stable and documented; defining it here costs a line and removes the
// question entirely.
static const GUID CONSOLE_DISPLAY_STATE = {
    0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 }
};

// Display-state values delivered with the notification above.
static constexpr DWORD DISPLAY_OFF    = 0;
static constexpr DWORD DISPLAY_ON     = 1;
static constexpr DWORD DISPLAY_DIMMED = 2;

// Global pointer to the single instance, needed for the static service
// callbacks. Windows requires a plain function pointer (no captures) for the
// service entry and control handler, so we route them through this global.
static WindowsPowerMonitor* g_instance = nullptr;

// Static entry point called by the Service Control Manager when the service
// starts. Forwards to the instance method.
static VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    if (g_instance) g_instance->serviceMain(argc, argv);
}

// Static control handler called by the SCM for control events.
static DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD eventType,
                                         LPVOID eventData, LPVOID /*context*/) {
    if (!g_instance) return ERROR_CALL_NOT_IMPLEMENTED;
    return g_instance->serviceCtrlHandler(control, eventType, eventData);
}

// Diagnostic log line, timestamped to the millisecond.
//
// The [event] and [cmd] lines elsewhere in the daemon are left byte-identical
// to what Linux writes, so the two platforms stay comparable. These extra
// [power] lines carry a timestamp because the journal is not here to add one,
// and because the gap between the last line before a suspend and the first
// after a resume is how the grace period gets measured at all.
//
// The whole line is composed before it is written: the SCM handler thread and
// the worker both log, and a line assembled with several << could interleave.
static void plog(const std::string& msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u",
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::cout << ("[power] " + std::string(stamp) + " " + msg + "\n");
}

WindowsPowerMonitor::WindowsPowerMonitor() {
    g_instance = this;
}

WindowsPowerMonitor::~WindowsPowerMonitor() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopRequested_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();

    if (displayNotify_) {
        UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
    }
    if (workerDone_) {
        CloseHandle(workerDone_);
        workerDone_ = nullptr;
    }
    g_instance = nullptr;
}

void WindowsPowerMonitor::setOnSleep(std::function<bool()> cb)    { onSleep_    = std::move(cb); }
void WindowsPowerMonitor::setOnWake(std::function<bool()> cb)     { onWake_     = std::move(cb); }
void WindowsPowerMonitor::setOnShutdown(std::function<bool()> cb) { onShutdown_ = std::move(cb); }

void WindowsPowerMonitor::run() {
    // The dispatch table tells the SCM which function to call when starting the
    // service. The table is null-terminated.
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), ServiceMain },
        { nullptr, nullptr }
    };

    // Blocks here until the service stops. It connects this process to the SCM
    // and hands off control to ServiceMain on an SCM-owned thread.
    if (StartServiceCtrlDispatcherW(dispatchTable)) return;

    const DWORD err = GetLastError();
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        // Started by hand rather than by the SCM. Power events are only
        // delivered to a service on Windows, so there is nothing useful to run
        // in the foreground — say so plainly rather than sitting there doing
        // nothing. --console covers the case of checking the device is
        // reachable; see main.cpp.
        std::cerr << "[monitor] Not started by the Service Control Manager.\n"
                     "[monitor] Power events are only delivered to a service. Install with\n"
                     "[monitor] packaging/windows/install-service.ps1, or pass --console to\n"
                     "[monitor] check the ESP32 is reachable without installing anything.\n";
    } else {
        std::cerr << "[error] StartServiceCtrlDispatcher failed: " << err << "\n";
    }
}

void WindowsPowerMonitor::serviceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // RegisterServiceCtrlHandlerExW rather than the basic RegisterServiceCtrlHandlerW:
    // the extended handler signature is the one that carries power event details.
    statusHandle_ = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
    if (!statusHandle_) {
        std::cerr << "[error] RegisterServiceCtrlHandlerEx failed: " << GetLastError() << "\n";
        return;
    }

    status_.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    status_.dwServiceSpecificExitCode = 0;
    status_.dwControlsAccepted        = 0;
    reportStatus(SERVICE_START_PENDING);

    workerDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!workerDone_) {
        std::cerr << "[error] CreateEvent failed: " << GetLastError() << "\n";
        reportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    {
        // SERVICE_ACCEPT_PRESHUTDOWN is called early in shutdown and allows
        // minutes, where SERVICE_ACCEPT_SHUTDOWN fires too late for reliable
        // USB communication. POWEREVENT delivers both the suspend/resume
        // notifications and the display-state changes registered below.
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.dwControlsAccepted = SERVICE_ACCEPT_STOP
                                   | SERVICE_ACCEPT_PRESHUTDOWN
                                   | SERVICE_ACCEPT_POWEREVENT;
    }
    reportStatus(SERVICE_RUNNING);

    // Subscribe to console display state. Registering is documented to report
    // the current value immediately, which is what gives the service its
    // starting picture of the world — and therefore the ON at boot, since the
    // uptime gate Linux uses cannot survive Fast Startup.
    //
    // Deliberately after the status above declares SERVICE_ACCEPT_POWEREVENT.
    // The value delivered at registration is the one the boot ON depends on,
    // and a control the service has not yet said it accepts is not a control
    // the SCM is obliged to deliver.
    displayNotify_ = RegisterPowerSettingNotification(
        reinterpret_cast<HANDLE>(statusHandle_), &CONSOLE_DISPLAY_STATE,
        DEVICE_NOTIFY_SERVICE_HANDLE);

    if (!displayNotify_) {
        // Not fatal. PBT_APMSUSPEND and PBT_APMRESUMESUSPEND still cover sleep
        // and a user-initiated wake on a classic S3 machine; what is lost is
        // Modern Standby and the boot ON. Loud, because the daemon is now
        // running in a degraded mode that otherwise looks identical.
        std::cerr << "[monitor] RegisterPowerSettingNotification failed: " << GetLastError()
                  << " — falling back to suspend/resume events only. Modern Standby\n"
                     "[monitor] machines and the ON at boot will not work in this mode.\n";
    }

    // Started last. A notification arriving before this point is not lost: the
    // handler only records the desired state, and the worker reads it as its
    // first action.
    worker_ = std::thread(&WindowsPowerMonitor::workerLoop, this);

    std::cout << "[monitor] Windows Service running\n";

    // The worker owns the lifetime from here: it signals when it has finished
    // whatever the last control asked for, so SERVICE_STOPPED is reported once,
    // from one place, after the work is actually done.
    WaitForSingleObject(workerDone_, INFINITE);
    if (worker_.joinable()) worker_.join();

    if (displayNotify_) {
        UnregisterPowerSettingNotification(displayNotify_);
        displayNotify_ = nullptr;
    }
    reportStatus(SERVICE_STOPPED);
}

DWORD WindowsPowerMonitor::serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData) {
    switch (control) {

    case SERVICE_CONTROL_INTERROGATE: {
        std::lock_guard<std::mutex> lock(statusMutex_);
        SetServiceStatus(statusHandle_, &status_);
        return NO_ERROR;
    }

    case SERVICE_CONTROL_PRESHUTDOWN:
        plog("SERVICE_CONTROL_PRESHUTDOWN");
        reportStatus(SERVICE_STOP_PENDING);
        requestShutdown();
        return NO_ERROR;

    case SERVICE_CONTROL_STOP:
        std::cout << "[monitor] Service stop requested\n";
        reportStatus(SERVICE_STOP_PENDING);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopRequested_ = true;
        }
        queueCv_.notify_all();
        return NO_ERROR;

    case SERVICE_CONTROL_POWEREVENT:
        switch (eventType) {

        case PBT_APMSUSPEND:
            // A notification, not a request: the machine goes down about two
            // seconds from now whatever we do. Secondary to display state,
            // which normally fires first — lastAsserted_ absorbs the overlap.
            plog("PBT_APMSUSPEND");
            requestState(false, Trigger::Suspend);
            return NO_ERROR;

        case PBT_APMRESUMESUSPEND:
            // Sent only for a user-initiated resume, so it does imply somebody
            // is at the machine. It exists here as the fallback for a machine
            // where display-state notifications never arrive — so it defers to
            // display state whenever there is one.
            //
            // The case that forces this: a Modern Standby machine cycles in and
            // out of DRIPS, and a resume raised while the display is still off
            // would turn the TV on to a blank screen. The two directions are
            // not symmetric. A spurious OFF is corrected by the next display-on
            // notification; a spurious ON leaves a lit TV in a dark room until
            // somebody notices.
            if (lastDisplayState_.has_value() && !*lastDisplayState_) {
                plog("PBT_APMRESUMESUSPEND while display is known off — not asserting on");
                bumpGeneration();
                return NO_ERROR;
            }
            plog("PBT_APMRESUMESUSPEND (user-initiated resume)");
            requestState(true, Trigger::Resume);
            return NO_ERROR;

        case PBT_APMRESUMEAUTOMATIC:
            // Fires on *every* resume and explicitly does not imply the user is
            // present — wake timers, maintenance windows and update-driven
            // wakes all raise it. Turning the TV on here is how the daemon ends
            // up doing it at 3am to an empty room, which is the defect the
            // Linux uptime gate exists to prevent. Logged, not acted on: if
            // somebody really did wake the machine, the display comes on and
            // that notification is what turns the TV on.
            //
            // Still a power transition, so the generation advances: anything
            // the worker had in flight across the suspend is now stale. This is
            // in fact the main case the counter exists for — the worker frozen
            // mid-read by the suspend, thawing here.
            bumpGeneration();
            plog("PBT_APMRESUMEAUTOMATIC (resume, presence unknown) — not acted on");
            return NO_ERROR;

        case PBT_POWERSETTINGCHANGE: {
            const auto* setting = static_cast<const POWERBROADCAST_SETTING*>(eventData);
            if (!setting || !IsEqualGUID(setting->PowerSetting, CONSOLE_DISPLAY_STATE)) {
                return NO_ERROR;
            }
            if (setting->DataLength < sizeof(DWORD)) {
                plog("display-state notification with unexpected payload — ignored");
                return NO_ERROR;
            }

            // eventData is valid only for the duration of this call, so the
            // value is copied out before anything else can happen to it.
            DWORD state = 0;
            std::memcpy(&state, setting->Data, sizeof(state));

            switch (state) {
            case DISPLAY_OFF:
                plog("display state = off");
                lastDisplayState_ = false;
                requestState(false, Trigger::DisplayState);
                break;
            case DISPLAY_ON:
                plog("display state = on");
                lastDisplayState_ = true;
                requestState(true, Trigger::DisplayState);
                break;
            case DISPLAY_DIMMED:
                // Dimmed is still lit, and it is the step before off rather
                // than a state of its own. Treated as on.
                plog("display state = dimmed (treated as on)");
                lastDisplayState_ = true;
                requestState(true, Trigger::DisplayState);
                break;
            default:
                plog("display state = " + std::to_string(state) + " (unknown) — ignored");
                break;
            }
            return NO_ERROR;
        }

        default:
            plog("unhandled power event type " + std::to_string(eventType));
            return NO_ERROR;
        }

    default:
        // Claiming NO_ERROR for a control we do not implement tells the SCM we
        // handled something we ignored.
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Every power transition advances the generation, whether or not a send results
// — that is what makes work started before the transition identifiable as stale
// afterwards.
//
// The increment happens under queueMutex_, and the worker reads the value under
// the same lock when it dequeues. Bumping outside the lock leaves a window
// where a transition lands between the worker taking its work and reading the
// generation, so the work is stamped with the *new* value and the staleness
// check afterwards silently passes.
void WindowsPowerMonitor::bumpGeneration() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void WindowsPowerMonitor::requestState(bool on, Trigger why) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        generation_.fetch_add(1, std::memory_order_relaxed);
        pendingOn_  = on;
        pendingWhy_ = why;
    }
    queueCv_.notify_all();
}

void WindowsPowerMonitor::requestShutdown() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        generation_.fetch_add(1, std::memory_order_relaxed);
        shutdownRequested_ = true;
    }
    queueCv_.notify_all();
}

void WindowsPowerMonitor::workerLoop() {
    // Registering for display-state notification is documented to deliver the
    // current value straight away, which is what gives the daemon its opening
    // state. Wait briefly for it rather than assuming it arrived.
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        const bool arrived = queueCv_.wait_for(lock, INITIAL_STATE_WAIT, [this] {
            return pendingOn_.has_value() || shutdownRequested_ || stopRequested_;
        });
        if (!arrived) {
            // Documented fallback: assume the display is on. Wrong only for a
            // service restart that happens while the display is off, and the
            // next display-state change corrects it either way.
            plog("no display state reported at registration — assuming display on");
            pendingOn_  = true;
            pendingWhy_ = Trigger::Startup;
        }
    }

    while (true) {
        std::optional<bool> want;
        Trigger  why        = Trigger::Startup;
        bool     doShutdown = false;
        bool     doStop     = false;
        uint64_t gen        = 0;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return pendingOn_.has_value() || shutdownRequested_ || stopRequested_;
            });
            want = pendingOn_;
            pendingOn_.reset();
            why        = pendingWhy_;
            doShutdown = shutdownRequested_;
            doStop     = stopRequested_;

            // Read under the same lock that publishes it, so this work is
            // stamped with the generation it actually belongs to.
            gen = generation_.load(std::memory_order_relaxed);
        }

        if (doShutdown) {
            // Shutdown sends unconditionally, where a display or suspend event
            // would be skipped as redundant. It is the last chance to leave the
            // TV in the right state, and preshutdown gives minutes rather than
            // the two seconds a suspend does, so the repeat costs nothing worth
            // saving.
            std::cout << "[event] Shutting down\n";
            reportPending();
            if (onShutdown_) {
                try { (void)onShutdown_(); } catch (...) {}
            }
            reportPending();
            break;
        }

        if (want.has_value()) {
            const bool on = *want;

            if (lastAsserted_.has_value() && *lastAsserted_ == on) {
                plog(std::string("TV already ") + (on ? "on" : "off")
                     + " — no command sent");
            } else {
                // Name the actual cause. Reusing "Going to sleep" for a screen
                // that merely blanked would put a plausible lie in the log,
                // which is how the last two hardening passes started.
                switch (why) {
                case Trigger::DisplayState:
                    std::cout << (on ? "[event] Display on\n" : "[event] Display off\n");
                    break;
                case Trigger::Suspend:
                    std::cout << "[event] Going to sleep\n";
                    break;
                case Trigger::Resume:
                    std::cout << "[event] Woke up\n";
                    break;
                case Trigger::Startup:
                    std::cout << (on ? "[event] Startup — display on\n"
                                     : "[event] Startup — display off\n");
                    break;
                }
                reportPending();

                bool confirmed = false;
                try {
                    if (on) { if (onWake_)  confirmed = onWake_(); }
                    else    { if (onSleep_) confirmed = onSleep_(); }
                } catch (...) {
                    confirmed = false;
                }

                if (generation_.load(std::memory_order_relaxed) != gen) {
                    // A power transition happened while this was in flight —
                    // almost certainly the machine suspended mid-send and has
                    // now resumed. Whatever the ACK said, it says nothing about
                    // the state the machine is in now. Forget what we think the
                    // TV is doing so the next assertion is not suppressed.
                    plog("power state changed mid-command — result discarded as stale");
                    lastAsserted_.reset();
                } else if (confirmed) {
                    lastAsserted_ = on;
                } else {
                    // No ACK. Recording the intent here would remember a
                    // command the TV never received and suppress the retry,
                    // leaving it wrong indefinitely.
                    lastAsserted_.reset();
                }
            }
        }

        if (doStop) break;
    }

    plog("worker finished");
    if (workerDone_) SetEvent(workerDone_);
}

void WindowsPowerMonitor::reportStatus(DWORD state, DWORD exitCode) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.dwCurrentState  = state;
    status_.dwWin32ExitCode = exitCode;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        status_.dwWaitHint = PENDING_WAIT_HINT_MS;
        status_.dwCheckPoint++;
    } else {
        status_.dwWaitHint   = 0;
        status_.dwCheckPoint = 0;
    }
    SetServiceStatus(statusHandle_, &status_);
}

void WindowsPowerMonitor::reportPending() {
    // Advancing the checkpoint is how the SCM is told work is still in
    // progress rather than hung. Only meaningful in a _PENDING state.
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (status_.dwCurrentState != SERVICE_STOP_PENDING &&
        status_.dwCurrentState != SERVICE_START_PENDING) {
        return;
    }
    status_.dwCheckPoint++;
    SetServiceStatus(statusHandle_, &status_);
}

#endif // _WIN32
