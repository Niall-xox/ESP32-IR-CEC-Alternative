#pragma once
#ifdef _WIN32

#include "IPowerMonitor.h"
#include "WindowsPowerCapabilities.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <windows.h>

// IPowerMonitor implementation for Windows.
//
// Runs the daemon as a Windows Service. That is not a packaging preference: the
// power notifications this needs are delivered to a service control handler,
// and a plain background exe does not receive them all.
//
// --- What drives the TV ---------------------------------------------------
//
// The primary signal is the console display state, subscribed to via
// RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE) and delivered as
// SERVICE_CONTROL_POWEREVENT / PBT_POWERSETTINGCHANGE. One mechanism covers all
// three Windows configurations the brief enumerates:
//
//   * Boot, including a Fast Startup boot. Registration reports the current
//     value, so the service learns the display is on and turns the TV on. The
//     uptime gate Linux uses cannot work here — Fast Startup hibernates the
//     kernel session rather than shutting down, so the tick count does not
//     reset and every power-on looks like a service restart.
//   * Modern Standby (S0 low power idle), where no classic suspend occurs and
//     PBT_APMSUSPEND may never arrive usefully. Display state is the documented
//     signal on that power model.
//   * An unattended wake — a maintenance window or a wake timer. The display
//     stays off, so nothing fires. Handling PBT_APMRESUMEAUTOMATIC instead
//     would turn the TV on at 3am with nobody there, which is the same defect
//     the Linux uptime gate exists to prevent.
//
// Suspend and resume events are kept as secondary triggers. PBT_APMSUSPEND
// asserts off — it covers hibernate as well as sleep, since Windows announces
// both the same way — and PBT_APMRESUMESUSPEND, which Windows sends only for a
// user-initiated wake, asserts on. So the daemon still behaves correctly on a
// machine where display-state notifications turn out not to arrive.
// PBT_APMRESUMEAUTOMATIC and PBT_APMRESUMECRITICAL are logged and deliberately
// not acted on.
//
// Overlap between the two sources is free because the IR codes are discrete
// rather than toggle: re-asserting a state the TV is already in is a no-op at
// the TV. lastAsserted_ suppresses the redundant send anyway, so the duplicate
// does not consume a suspend's short grace period.
//
// --- Knowing which machine this is ----------------------------------------
//
// The three configurations are not distinguishable from behaviour alone: an
// absent PBT_APMSUSPEND is a defect on an S3 desktop and correct on a Modern
// Standby laptop. The capability report is read once at startup and logged, so
// every log says which machine produced it rather than leaving that to be
// reconstructed later from a `powercfg /a` nobody ran at the time.
//
// --- Device presence ------------------------------------------------------
//
// RegisterDeviceNotification on the HID interface class reports the ESP32
// arriving and leaving, delivered as SERVICE_CONTROL_DEVICEEVENT. Two things
// depend on it, and both matter most on the path the brief calls the largest
// open risk — USB across standby:
//
//   * A removal invalidates the transport's handle deliberately, rather than
//     leaving it to be discovered by the next write failing.
//   * An arrival re-asserts the current display state. The firmware
//     re-enumerates itself after a USB suspend, so a wake ON issued while the
//     device is still coming back fails; the arrival that follows a second
//     later is what retries it. Without this the TV stays wrong until the next
//     power event, which could be hours.
//
// Both are routed through the worker rather than acted on in the handler, for
// the threading reason below: the transport is single-threaded by design and
// the handler runs on an SCM thread.
//
// --- What this cannot do --------------------------------------------------
//
// There is no Windows equivalent of a logind delay inhibitor for sleep.
// PBT_APMQUERYSUSPEND, which could veto a suspend, was removed after XP.
// PBT_APMSUSPEND is a notification: the machine suspends roughly two seconds
// later whether or not anything has finished. sleepBudget() is sized for that,
// and the ACK remains worth waiting for as the only evidence the IR fired.
// How much of that budget was actually used is measured and logged, because
// the two seconds is guidance rather than a contract.
//
// Shutdown is the opposite case. SERVICE_ACCEPT_PRESHUTDOWN gives minutes,
// which is more headroom than logind's five seconds.
//
// --- Threading ------------------------------------------------------------
//
// Control handlers record the event and return; a single long-lived worker
// thread does the sending. Blocking inside a handler is against the documented
// SCM contract, and it would also couple the handler to a timeout constant in
// HIDTransport.cpp with nothing but a comment connecting them.
//
// One worker serialises every touch of the transport — sends and the
// invalidation a device removal triggers alike — so HIDTransport's
// single-threaded assumption holds by construction and no lock is needed there.
// SERVICE_STATUS is a different matter: the worker advances checkpoints while
// the handler also reports state, so statusMutex_ guards it.
class WindowsPowerMonitor : public IPowerMonitor {
public:
    // The VID/PID are needed to tell this device's arrival and removal from
    // every other HID device on the machine — a notification names the device
    // that changed, and a keyboard being replugged must not invalidate the
    // transport or re-assert the TV.
    WindowsPowerMonitor(uint16_t vid, uint16_t pid);
    ~WindowsPowerMonitor() override;

    void setOnSleep(std::function<bool()> cb) override;
    void setOnWake(std::function<bool()> cb) override;
    void setOnShutdown(std::function<bool()> cb) override;
    void setOnDeviceChange(std::function<void(bool)> cb) override;

    // Registers and starts the Windows Service control dispatcher.
    // Blocks until the service is stopped by the OS.
    void run() override;

    // Windows suspends about two seconds after announcing it and cannot be
    // delayed, so a sleep send has to fail fast rather than block through the
    // transition. Shutdown and wake are not constrained this way and use the
    // transport's default.
    std::chrono::milliseconds sleepBudget() const override { return SLEEP_BUDGET; }

    // Called by the static service entry point and control handler, which
    // Windows requires to be plain functions with no captures.
    void serviceMain(DWORD argc, LPWSTR* argv);
    DWORD serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData);

private:
    // Time a Windows suspend actually leaves us. Microsoft's guidance is about
    // two seconds; this is deliberately under that, because overrunning it does
    // not delay the suspend, it just means blocking across one.
    static constexpr auto SLEEP_BUDGET = std::chrono::milliseconds(1500);

    // How long the worker waits at startup for the display-state notification
    // that registration should deliver, before falling back to asserting on.
    static constexpr auto INITIAL_STATE_WAIT = std::chrono::milliseconds(2000);

    // Reported to the SCM as the time work may take. Generous: exceeding it is
    // what makes the SCM decide the service has hung.
    static constexpr DWORD PENDING_WAIT_HINT_MS = 15000;

    enum class Trigger { DisplayState, Suspend, Resume, Startup, DeviceArrival };

    void workerLoop();
    void requestState(bool on, Trigger why);
    void requestShutdown();
    void requestDeviceInvalidate();
    void bumpGeneration();

    // True if a device-notification name refers to this ESP32 rather than to
    // some other HID device. Compares the VID/PID substring the interface path
    // carries, case-insensitively — Windows is not consistent about the case of
    // the hex digits between the arrival and removal notifications for the same
    // device.
    bool deviceNameMatches(const wchar_t* name) const;

    // Handles SERVICE_CONTROL_DEVICEEVENT. Split out because the payload
    // decoding is the same shape as the power-setting one and reads badly
    // nested three switches deep.
    void handleDeviceEvent(DWORD eventType, LPVOID eventData);

    // Reports how long the machine was away, from the last suspend
    // announcement to now. A Modern Standby machine dipping in and out of DRIPS
    // and a machine that genuinely hibernated overnight produce the same
    // sequence of events and differ only in this number.
    void logAwayTime();

    // Reports a service state to the SCM. Advances the checkpoint for the
    // _PENDING states, which is how the SCM is told work is still progressing.
    void reportStatus(DWORD state, DWORD exitCode = NO_ERROR);
    void reportPending();

    uint16_t vid_;
    uint16_t pid_;

    std::function<bool()>         onSleep_;
    std::function<bool()>         onWake_;
    std::function<bool()>         onShutdown_;
    std::function<void(bool)>     onDeviceChange_;

    SERVICE_STATUS        status_       = {};
    SERVICE_STATUS_HANDLE statusHandle_ = nullptr;
    std::mutex            statusMutex_;

    HPOWERNOTIFY displayNotify_ = nullptr;
    HDEVNOTIFY   deviceNotify_  = nullptr;

    // Read once at startup and logged. Kept because the summary is worth
    // repeating alongside a surprising event rather than only at line one of a
    // log that may since have rotated.
    WindowsPowerCapabilities caps_;

    // Signalled by the worker once it has finished and is exiting, so
    // serviceMain reports SERVICE_STOPPED exactly once and from one place.
    HANDLE workerDone_ = nullptr;

    std::thread             worker_;
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;

    // The queue holds a desired state, not a backlog. ON and OFF are idempotent
    // assertions about what the TV should be rather than commands that each
    // need executing, so a newer request replaces an older one. Replaying a
    // backlog after a resume would fire stale commands.
    std::optional<bool> pendingOn_;
    Trigger             pendingWhy_ = Trigger::Startup;
    bool                shutdownRequested_ = false;
    bool                stopRequested_     = false;

    // A device removal seen but not yet acted on. The transport may only be
    // touched from the worker, so the handler records this and returns.
    bool                deviceGone_ = false;

    // When PBT_APMSUSPEND arrived, so the worker can report how much of the
    // grace period the send actually consumed. steady_clock because this
    // measures an interval the machine is awake for throughout; the wall-clock
    // member below measures the part it is not.
    std::optional<std::chrono::steady_clock::time_point> suspendAnnouncedAt_;

    // Wall-clock time of the last suspend announcement, used to report how long
    // the machine was away. Distinguishes a brief Modern Standby cycle from a
    // real hibernate in a log where both otherwise look identical. Written and
    // read only on the SCM handler thread.
    std::optional<std::chrono::system_clock::time_point> suspendWallAt_;

    // Bumped on every power transition. The worker captures it before a send
    // and rechecks afterwards: a send that spanned a suspend proves nothing
    // about the state the machine is in now, whatever the clock did across the
    // transition. Correct without depending on whether steady_clock advances
    // while suspended.
    std::atomic<uint64_t> generation_{0};

    // What the TV was last told to be, so a repeat assertion is skipped.
    // Unset means unknown — after startup, or after a send whose result was
    // discarded as stale.
    std::optional<bool> lastAsserted_;

    // Last display state reported by the OS, or unset if it has never reported
    // one. Distinct from lastAsserted_, which is what the *TV* was told: this
    // is what the *machine* said about itself.
    //
    // It exists so the resume events can stay a fallback rather than a second
    // opinion, and so a device arrival has a state to re-assert. Written and
    // read only on the SCM handler thread, which delivers controls one at a
    // time, so it needs no lock.
    std::optional<bool> lastDisplayState_;
};

#endif // _WIN32
