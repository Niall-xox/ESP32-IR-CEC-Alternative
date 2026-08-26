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
// Windows varies along two axes the brief enumerates — S3 versus Modern
// Standby, and Fast Startup on or off — and no single signal covers every
// combination on its own. Two do:
//
//   * Console display state, via
//     RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE), delivered
//     as SERVICE_CONTROL_POWEREVENT / PBT_POWERSETTINGCHANGE.
//   * The classic suspend and resume broadcasts, PBT_APMSUSPEND and
//     PBT_APMRESUMESUSPEND.
//
// The display signal is asymmetric on purpose, and which half is used depends
// on what the machine reports itself to be:
//
//   * **Display on always asserts ON.** The screen lighting up means somebody
//     is at the machine, which is the question every ON has to answer. It is
//     also the only route to an ON at boot: the uptime gate Linux uses cannot
//     work here, because Fast Startup hibernates the kernel session rather than
//     shutting down, so the tick count does not reset and every power-on reads
//     as a service restart.
//
//   * **Display off asserts OFF only where nothing else can.** On a Modern
//     Standby machine there is no classic suspend and PBT_APMSUSPEND may never
//     arrive usefully, so the display blanking is the only signal there is. On
//     a machine that reports classic S3, PBT_APMSUSPEND does arrive and is the
//     honest signal, so an idle screen blank is left alone.
//
// That last distinction is the whole reason the capability report is read.
// Driving OFF from a screen blank everywhere was the earlier design, and it
// took a behaviour forced by one power model and applied it to a machine that
// did not need it — turning the TV off under somebody who is still sitting
// there watching something that failed to assert ES_DISPLAY_REQUIRED. That is
// the device's own primary use case, a PC connected to a television, so the
// failure lands exactly where it hurts most.
//
// On a machine whose capabilities cannot be read, or which reports neither S3
// nor Modern Standby, display-off drives OFF. Unknown means no suspend event
// can be relied on, and "the TV never goes off" fails one of the four things
// this device exists to do, where an occasional early OFF is an annoyance the
// user can undo with their own remote.
//
// PBT_APMRESUMEAUTOMATIC and PBT_APMRESUMECRITICAL are logged and deliberately
// never assert ON: both fire for wake timers and maintenance windows, and
// acting on them is how the daemon ends up turning the TV on at 3am to an empty
// room — the defect the Linux uptime gate exists to prevent, arriving by
// another route.
//
// Overlap between the two sources is free because the IR codes are discrete
// rather than toggle: re-asserting a state the TV is already in is a no-op at
// the TV. lastAsserted_ suppresses the duplicate inside a suspend, where the
// grace period is short — and only there, because outside one the repeat is the
// drift repair discrete codes were chosen to provide.
//
// --- Knowing which machine this is ----------------------------------------
//
// The three configurations are not distinguishable from behaviour alone: an
// absent PBT_APMSUSPEND is a defect on an S3 desktop and correct on a Modern
// Standby laptop. The capability report is read once at startup, logged, and
// then used — it decides the display-off policy above, so it is load-bearing
// rather than merely diagnostic.
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
//   * An arrival re-asserts whatever state was last decided. The firmware
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
// later whether or not anything has finished. SLEEP_BUDGET is sized for that,
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

    void setOnCommand(std::function<bool(const TvCommand&)> cb) override;
    void setOnDeviceChange(std::function<void(bool)> cb) override;

    // Registers and starts the Windows Service control dispatcher.
    // Blocks until the service is stopped by the OS.
    void run() override;

    // Called by the static service entry point and control handler, which
    // Windows requires to be plain functions with no captures.
    void serviceMain(DWORD argc, LPWSTR* argv);
    DWORD serviceCtrlHandler(DWORD control, DWORD eventType, LPVOID eventData);

private:
    // Time a Windows suspend actually leaves us. Microsoft's guidance is about
    // two seconds; this is deliberately under that, because overrunning it does
    // not delay the suspend, it just means blocking across one.
    static constexpr auto SLEEP_BUDGET = std::chrono::milliseconds(1500);

    // Well inside the 60s PreshutdownTimeout the installer sets, leaving the
    // SCM most of its allowance even if this runs to the end. Sized to cover a
    // device that is re-enumerating — about a second — several times over,
    // rather than to use everything available.
    static constexpr auto SHUTDOWN_BUDGET = std::chrono::milliseconds(20000);

    // How long the worker waits at startup for the display-state notification
    // that registration should deliver, before logging that the opening state
    // is unknown. Nothing is asserted either way — see workerLoop().
    static constexpr auto INITIAL_STATE_WAIT = std::chrono::milliseconds(2000);

    // Reported to the SCM as the time work may take. It has to exceed the
    // longest a single send can block for, which is SHUTDOWN_BUDGET: the
    // checkpoint only advances either side of a send, so a shorter hint lets
    // the SCM decide the service has hung while it is doing exactly what
    // preshutdown gave it the time for.
    static constexpr DWORD PENDING_WAIT_HINT_MS = 30000;
    static_assert(PENDING_WAIT_HINT_MS > SHUTDOWN_BUDGET.count(),
                  "the wait hint must outlast the longest send, or the SCM will "
                  "call a working shutdown send a hang");

    enum class Trigger { DisplayState, Suspend, Resume, DeviceArrival };

    void workerLoop();
    void requestState(bool on, Trigger why);
    void requestShutdown();
    void requestDeviceInvalidate();
    void bumpGeneration();

    // How long this OS will wait for us, and what to call it in the log — both
    // properties of the trigger rather than of the direction. A screen blanking
    // and a suspend both send OFF and are not remotely the same event.
    static std::chrono::milliseconds budgetFor(Trigger why);
    static const char*               reasonFor(Trigger why, bool on);

    // Whether an idle screen blank should turn the TV off on this machine.
    // Answered from the capability report — see the class comment.
    bool displayOffDrivesOff() const;

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

    std::function<bool(const TvCommand&)> onCommand_;
    std::function<void(bool)>             onDeviceChange_;

    SERVICE_STATUS        status_       = {};
    SERVICE_STATUS_HANDLE statusHandle_ = nullptr;
    std::mutex            statusMutex_;

    HPOWERNOTIFY displayNotify_ = nullptr;
    HDEVNOTIFY   deviceNotify_  = nullptr;

    // Read once at startup, logged, and then consulted on every display-off
    // notification. Written before the worker starts and never again, so the
    // worker and the handler both read it without a lock.
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
    Trigger             pendingWhy_ = Trigger::DisplayState;
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

    // What the TV was last told to be *and confirmed as*, so a repeat assertion
    // can be skipped inside a suspend. Unset means unknown — before the first
    // command, after one whose ACK never arrived, and after a device removal.
    std::optional<bool> lastAsserted_;

    // The last state the policy decided the TV should be in, whether or not the
    // send succeeded. Three distinct things are tracked here and conflating any
    // two of them has already caused a bug:
    //
    //   lastDisplayState_ — what the machine said about its own screen
    //   desiredOn_        — what this daemon concluded the TV should be
    //   lastAsserted_     — what the TV actually confirmed
    //
    // A device arrival re-asserts desiredOn_, not lastDisplayState_. On a
    // machine where an idle blank deliberately does *not* turn the TV off, the
    // two disagree, and re-asserting the display state would switch the TV off
    // on a replug for a reason the policy had already rejected.
    //
    // Written and read only on the SCM handler thread, which delivers controls
    // one at a time, so neither needs a lock.
    std::optional<bool> desiredOn_;

    // Last display state reported by the OS, or unset if it has never reported
    // one. Kept even where it does not drive an OFF, because it is what lets
    // PBT_APMRESUMESUSPEND stay a fallback rather than a second opinion: a
    // resume raised while the screen is still dark is a DRIPS exit, not
    // somebody walking into the room.
    std::optional<bool> lastDisplayState_;
};

#endif // _WIN32
