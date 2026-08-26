#ifdef _WIN32

#include "WindowsPowerMonitor.h"

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <iostream>
#include <string>

#include <dbt.h>

// The service name must match what was used when installing —
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

// GUID_DEVINTERFACE_HID, spelled out for exactly the same reason: the SDK
// declares it in hidclass.h but defines it in hid.lib, which this build does
// not otherwise need to link.
static const GUID HID_DEVICE_INTERFACE = {
    0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }
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

// Seconds rendered the way somebody reading a log wants them: a standby dip and
// an overnight hibernate are both "a resume", and the number is what tells them
// apart, so it has to be legible at both scales.
static std::string formatSeconds(long long totalSeconds) {
    if (totalSeconds < 0) return "unknown";
    if (totalSeconds < 90) return std::to_string(totalSeconds) + "s";

    const long long minutes = totalSeconds / 60;
    const long long seconds = totalSeconds % 60;
    if (minutes < 90) {
        return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
    }
    const long long hours = minutes / 60;
    return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
}

WindowsPowerMonitor::WindowsPowerMonitor(uint16_t vid, uint16_t pid)
    : vid_(vid), pid_(pid) {
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
    if (deviceNotify_) {
        UnregisterDeviceNotification(deviceNotify_);
        deviceNotify_ = nullptr;
    }
    if (workerDone_) {
        CloseHandle(workerDone_);
        workerDone_ = nullptr;
    }
    g_instance = nullptr;
}

void WindowsPowerMonitor::setOnCommand(std::function<bool(const TvCommand&)> cb) {
    onCommand_ = std::move(cb);
}

void WindowsPowerMonitor::setOnDeviceChange(std::function<void(bool)> cb) {
    onDeviceChange_ = std::move(cb);
}

// How long this OS will wait before proceeding without us, for this trigger.
//
// Only the suspend and the shutdown have a deadline at all. A display change,
// a resume and a device arrival are all events nothing is waiting on, and they
// state zero so the transport applies its own default — which matters most on
// exactly those paths, because they are the ones most likely to find the ESP32
// mid-re-enumeration and needing more than a suspend's worth of patience.
std::chrono::milliseconds WindowsPowerMonitor::budgetFor(Trigger why) {
    return why == Trigger::Suspend ? SLEEP_BUDGET : std::chrono::milliseconds::zero();
}

// Names the cause for the [cmd] line. "OFF (sleep)" and "OFF (display off)"
// are different claims about why the TV went off, and putting the wrong one in
// the log is the failure mode two hardening passes were spent removing.
const char* WindowsPowerMonitor::reasonFor(Trigger why, bool on) {
    switch (why) {
    case Trigger::Suspend:       return "sleep";
    case Trigger::Resume:        return "wake";
    case Trigger::DeviceArrival: return "ESP32 reconnected";
    case Trigger::DisplayState:
    default:                     return on ? "display on" : "display off";
    }
}

// Whether an idle screen blank should turn the TV off on this machine.
//
// Only a machine that positively reports classic S3 — and not Modern Standby —
// is left alone, because only there is PBT_APMSUSPEND a signal that actually
// arrives. Everywhere else, including a machine whose capabilities could not be
// read at all, the display is the only evidence there is that nobody is
// watching, and a TV that never switches off fails one of the four things this
// device exists to do.
bool WindowsPowerMonitor::displayOffDrivesOff() const {
    return !(caps_.queried && caps_.s3 && !caps_.modernStandby);
}

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
        //
        // Device events need no flag here: they are delivered because of the
        // RegisterDeviceNotification call, not because the service accepts a
        // control type.
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.dwControlsAccepted = SERVICE_ACCEPT_STOP
                                   | SERVICE_ACCEPT_PRESHUTDOWN
                                   | SERVICE_ACCEPT_POWEREVENT;
    }
    reportStatus(SERVICE_RUNNING);

    // Logged before anything else happens, so every log opens by saying which
    // of the three configurations produced it. Without this line the rest of
    // the log is ambiguous: a missing PBT_APMSUSPEND is a defect on one machine
    // and the documented behaviour on another.
    // Read before the worker starts and never written again, so both threads
    // read it without a lock. It is not only diagnostic: displayOffDrivesOff()
    // is derived from it, so the line below also states which of the two
    // display policies this machine is about to run under.
    caps_ = queryPowerCapabilities();
    plog("machine power model: " + caps_.summary());
    for (const auto& line : caps_.details()) plog(line);
    plog(displayOffDrivesOff()
             ? "  display-off policy: an idle screen blank turns the TV off "
               "(no usable suspend event on this machine)"
             : "  display-off policy: an idle screen blank is ignored — "
               "PBT_APMSUSPEND drives the OFF on this machine");

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

    // Subscribe to HID device arrival and removal. Filtered to this VID/PID in
    // the handler rather than by the OS: the notification filter selects an
    // interface *class*, so every HID device on the machine arrives here and
    // the ones that are not the ESP32 are discarded on inspection.
    {
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
        filter.dbcc_size       = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid  = HID_DEVICE_INTERFACE;

        deviceNotify_ = RegisterDeviceNotificationW(
            reinterpret_cast<HANDLE>(statusHandle_), &filter,
            DEVICE_NOTIFY_SERVICE_HANDLE);

        if (!deviceNotify_) {
            // Not fatal, and quieter than the display-state failure above,
            // because nothing depends on it for correctness: without device
            // notifications the transport still discovers a vanished device by
            // failing a write and reopening, exactly as it does on Linux. What
            // is lost is the prompt retry after the firmware re-enumerates
            // itself following a USB suspend.
            std::cerr << "[monitor] RegisterDeviceNotification failed: " << GetLastError()
                      << " — device arrival and removal will not be seen. The transport\n"
                         "[monitor] still recovers by reopening on a failed write.\n";
        }
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
    if (deviceNotify_) {
        UnregisterDeviceNotification(deviceNotify_);
        deviceNotify_ = nullptr;
    }
    reportStatus(SERVICE_STOPPED);
}

bool WindowsPowerMonitor::deviceNameMatches(const wchar_t* name) const {
    if (!name) return false;

    wchar_t needle[32];
    std::swprintf(needle, sizeof(needle) / sizeof(needle[0]),
                  L"VID_%04X&PID_%04X",
                  static_cast<unsigned>(vid_), static_cast<unsigned>(pid_));

    // Upper-cased before comparison. The interface path is documented as
    // case-insensitive and Windows is not consistent about it in practice —
    // matching literally works until the day a notification arrives in the
    // other case and the device silently stops being recognised.
    std::wstring haystack(name);
    for (wchar_t& c : haystack) c = static_cast<wchar_t>(std::towupper(c));

    return haystack.find(needle) != std::wstring::npos;
}

void WindowsPowerMonitor::handleDeviceEvent(DWORD eventType, LPVOID eventData) {
    if (eventType != DBT_DEVICEARRIVAL && eventType != DBT_DEVICEREMOVECOMPLETE) {
        return;
    }

    const auto* hdr = static_cast<const DEV_BROADCAST_HDR*>(eventData);
    if (!hdr || hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) return;

    const auto* iface = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);
    if (!deviceNameMatches(iface->dbcc_name)) return;

    if (eventType == DBT_DEVICEARRIVAL) {
        plog("ESP32 arrived");

        // Re-assert whatever this daemon last decided the TV should be. That is
        // desiredOn_ and deliberately not lastDisplayState_: on a machine where
        // an idle blank is ignored the two disagree, and re-asserting the
        // display state would switch the TV off on a replug for a reason the
        // policy had already rejected.
        //
        // This is the retry that makes a wake survive the firmware
        // re-enumerating itself: the wake ON was issued while the device was
        // still coming back and failed, and the removal cleared lastAsserted_,
        // so this send is not suppressed as redundant.
        if (desiredOn_.has_value()) {
            requestState(*desiredOn_, Trigger::DeviceArrival);
        } else {
            plog("no state decided yet — nothing to re-assert");
        }
    } else {
        plog("ESP32 removed");

        // Recorded, not acted on here. The transport is single-threaded by
        // design and this is the SCM handler thread; the worker performs the
        // invalidation.
        requestDeviceInvalidate();
    }
}

void WindowsPowerMonitor::logAwayTime() {
    if (!suspendWallAt_.has_value()) return;

    const auto away = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - *suspendWallAt_);
    plog("away for " + formatSeconds(away.count()));
    suspendWallAt_.reset();
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

    case SERVICE_CONTROL_DEVICEEVENT:
        handleDeviceEvent(eventType, eventData);
        return NO_ERROR;

    case SERVICE_CONTROL_POWEREVENT:
        switch (eventType) {

        case PBT_APMSUSPEND:
            // A notification, not a request: the machine goes down about two
            // seconds from now whatever we do. Covers hibernate as well as
            // sleep — Windows announces both this way and does not say which,
            // which costs nothing because the answer is OFF either way.
            // Secondary to display state, which normally fires first;
            // lastAsserted_ absorbs the overlap.
            plog("PBT_APMSUSPEND (sleep or hibernate)");
            suspendWallAt_ = std::chrono::system_clock::now();
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
            logAwayTime();
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
            logAwayTime();
            plog("PBT_APMRESUMEAUTOMATIC (resume, presence unknown) — not acted on");
            return NO_ERROR;

        case PBT_APMRESUMECRITICAL:
            // The machine came back from a power loss it never got to announce
            // — a dead battery that hibernated in an emergency, or mains lost
            // on a desktop. Nothing was sent on the way down, so the TV is
            // whatever it was, and the same presence argument as
            // PBT_APMRESUMEAUTOMATIC applies: a machine restoring itself is not
            // evidence anybody is there. The display-state notification that
            // follows a real wake is what acts.
            //
            // Deprecated by Microsoft and still delivered by some systems,
            // which is reason to name it rather than let it fall through to the
            // unhandled branch and be logged as an unknown number.
            bumpGeneration();
            logAwayTime();
            plog("PBT_APMRESUMECRITICAL (resume after unannounced power loss) — not acted on");
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
                // Recorded either way, because PBT_APMRESUMESUSPEND defers to
                // it — but only acted on where the display is the only signal
                // this machine has. On a classic S3 machine PBT_APMSUSPEND
                // arrives and is the honest cause, so an idle blank under
                // somebody who is still watching is left alone.
                if (displayOffDrivesOff()) {
                    requestState(false, Trigger::DisplayState);
                } else {
                    plog("  ignored — this machine has S3, so the suspend "
                         "event drives the OFF");
                    bumpGeneration();
                }
                break;
            case DISPLAY_ON:
                // Always acted on, on every machine. The screen lighting up is
                // the most direct evidence there is that somebody is present,
                // which is the question every ON has to answer.
                plog("display state = on");
                logAwayTime();
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
    // What the policy has decided, whether or not the send that follows
    // succeeds. Read back by a later device arrival — see handleDeviceEvent().
    desiredOn_ = on;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);

        // A device arriving is not a power transition, and bumping the
        // generation for it would discard a send that was in flight and
        // correct — logging "power state changed mid-command" about a machine
        // whose power state did nothing of the sort. The same argument already
        // keeps requestDeviceInvalidate() from bumping it.
        if (why != Trigger::DeviceArrival) {
            generation_.fetch_add(1, std::memory_order_relaxed);
        }

        pendingOn_  = on;
        pendingWhy_ = why;

        // Stamped here rather than in the handler so it is written under the
        // same lock the worker reads it with, and so it cannot be attributed to
        // a send that a later event replaced.
        if (why == Trigger::Suspend) {
            suspendAnnouncedAt_ = std::chrono::steady_clock::now();
        }
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

// A device removal, queued for the worker. Deliberately does *not* advance the
// generation: the device going away says nothing about what state the machine
// is in, and discarding an in-flight send's result on that basis would forget a
// correct answer.
void WindowsPowerMonitor::requestDeviceInvalidate() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        deviceGone_ = true;
    }
    queueCv_.notify_all();
}

void WindowsPowerMonitor::workerLoop() {
    // Registering for display-state notification is documented to deliver the
    // current value straight away, which is what gives the daemon its opening
    // state. Wait briefly for it rather than assuming it arrived.
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait_for(lock, INITIAL_STATE_WAIT, [this] {
            return pendingOn_.has_value() || shutdownRequested_ || stopRequested_ || deviceGone_;
        });

        // The diagnostic below turns on whether a *display state* arrived, not
        // on whether the wait was cut short. A device removal or a stop landing
        // inside the first two seconds also ends the wait, and reading that as
        // "the opening state arrived" would silence the loudest line in the
        // design at exactly the moment it was earned.
        if (!pendingOn_.has_value()) {
            // Nothing is asserted. This used to assume the display was on, on
            // the grounds that it would be wrong only for a service restart
            // happening while the display was off — but that is precisely the
            // 3am case the whole design exists to avoid, and the installer
            // configures restart-on-failure, so an unattended restart is a
            // configured behaviour rather than a hypothetical.
            //
            // The asymmetry that guards PBT_APMRESUMESUSPEND applies here with
            // equal force: a spurious OFF is corrected by the next display-on
            // notification, while a spurious ON leaves a lit TV in a dark room
            // until somebody notices. Given a choice between missing an ON the
            // user can undo with their own remote, and asserting one nobody
            // asked for, the first is the smaller failure.
            //
            // The cost is real and worth naming: on a machine where the initial
            // value genuinely never arrives, the TV is not driven until the
            // first display-state *transition*. Reaching this line at all means
            // the assumption the boot ON rests on is false on this machine, so
            // it is a diagnostic rather than a mode to run in — which is why it
            // is logged this loudly.
            plog("no display state reported at registration — opening state "
                 "UNKNOWN, asserting nothing");
            plog("  the TV will be driven from the first display-state change");
            plog("  if this line appears on a normal boot, the boot ON needs "
                 "rethinking — see the brief");
        }
    }

    while (true) {
        std::optional<bool> want;
        Trigger  why          = Trigger::DisplayState;
        bool     doShutdown   = false;
        bool     doStop       = false;
        bool     doInvalidate = false;
        uint64_t gen          = 0;
        std::optional<std::chrono::steady_clock::time_point> suspendAt;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return pendingOn_.has_value() || shutdownRequested_
                    || stopRequested_ || deviceGone_;
            });
            want = pendingOn_;
            pendingOn_.reset();
            why          = pendingWhy_;
            doShutdown   = shutdownRequested_;
            doStop       = stopRequested_;
            doInvalidate = deviceGone_;
            deviceGone_  = false;
            suspendAt    = suspendAnnouncedAt_;

            // Read under the same lock that publishes it, so this work is
            // stamped with the generation it actually belongs to.
            gen = generation_.load(std::memory_order_relaxed);
        }

        // Before any send: the whole point of the notification is that the
        // handle a send would use is already dead.
        if (doInvalidate) {
            if (onDeviceChange_) {
                try { onDeviceChange_(false); } catch (...) {}
            }

            // What the TV is doing is now unknown, and must not be remembered.
            //
            // While the device is gone the daemon has no channel to the TV at
            // all, so anything could have happened to it — most obviously
            // somebody using the TV's own remote, which is why they unplugged
            // the ESP32 in the first place. Keeping lastAsserted_ across that
            // gap means the arrival re-assert is suppressed as redundant
            // against a state nobody has been in a position to observe, and the
            // TV stays wrong until the next power event.
            lastAsserted_.reset();
        }

        if (doShutdown) {
            // Shutdown sends unconditionally, where a suspend would be skipped
            // as redundant. It is the last chance to leave the TV in the right
            // state — nothing runs afterwards to correct it — and preshutdown
            // gives minutes rather than the two seconds a suspend does, so the
            // repeat costs nothing worth saving.
            std::cout << "[event] Shutting down\n";
            reportPending();
            if (onCommand_) {
                try {
                    (void)onCommand_(TvCommand{false, "shutdown", SHUTDOWN_BUDGET});
                } catch (...) {}
            }
            reportPending();
            break;
        }

        if (want.has_value()) {
            const bool on = *want;

            // Suppressing a repeat is an optimisation with a cost, so it is
            // applied only where it buys something.
            //
            // The reason it exists is the suspend grace period: the machine
            // goes down about two seconds after announcing it, and two IR
            // transmissions do not fit comfortably in that. Nowhere else is
            // under time pressure.
            //
            // Everywhere else it is actively harmful, because it argues against
            // the reason the IR codes are discrete rather than toggle. Discrete
            // codes were chosen so the TV reaches the right state regardless of
            // prior drift — and drift is exactly what happens when somebody
            // picks up the TV's own remote. Re-asserting a state the TV is
            // already in is a no-op at the TV; re-asserting one it has drifted
            // out of is the repair. Skipping the send throws the repair away to
            // save time that, outside a suspend, nothing is asking for.
            const bool redundant = lastAsserted_.has_value() && *lastAsserted_ == on;
            if (redundant && why == Trigger::Suspend) {
                plog(std::string("TV already ") + (on ? "on" : "off")
                     + " — no command sent (inside the suspend grace period)");
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
                case Trigger::DeviceArrival:
                    std::cout << (on ? "[event] ESP32 reconnected — re-asserting on\n"
                                     : "[event] ESP32 reconnected — re-asserting off\n");
                    break;
                }
                reportPending();

                // The reason and the deadline both travel with the command.
                // Only the suspend has a deadline; everything else here is an
                // event nothing is waiting on, and giving a screen blank a
                // suspend's 1.5 seconds was a real defect — it applied the one
                // path's time pressure to every path routed through the same
                // callback.
                bool confirmed = false;
                if (onCommand_) {
                    try {
                        confirmed = onCommand_(
                            TvCommand{on, reasonFor(why, on), budgetFor(why)});
                    } catch (...) {
                        confirmed = false;
                    }
                }

                // How much of the grace period the send actually used. The two
                // seconds Microsoft documents is guidance rather than a
                // contract, and this is the only way to find out what a given
                // machine really allows — measured on the machine, in the log,
                // rather than assumed.
                if (why == Trigger::Suspend && suspendAt.has_value()) {
                    const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - *suspendAt);
                    plog("suspend send took " + std::to_string(used.count()) + "ms of the "
                         + std::to_string(SLEEP_BUDGET.count()) + "ms budget");
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
