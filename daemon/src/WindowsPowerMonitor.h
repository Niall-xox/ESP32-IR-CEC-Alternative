#pragma once
#ifdef _WIN32

#include "IPowerMonitor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
// asserts off, and PBT_APMRESUMESUSPEND — which Windows sends only for a
// user-initiated wake — asserts on, so the daemon still behaves correctly on a
// machine where display-state notifications turn out not to arrive.
// PBT_APMRESUMEAUTOMATIC is logged and deliberately not acted on.
//
// Overlap between the two sources is free because the IR codes are discrete
// rather than toggle: re-asserting a state the TV is already in is a no-op at
// the TV. lastAsserted_ suppresses the redundant send anyway, so the duplicate
// does not consume a suspend's short grace period.
//
// --- What this cannot do --------------------------------------------------
//
// There is no Windows equivalent of a logind delay inhibitor for sleep.
// PBT_APMQUERYSUSPEND, which could veto a suspend, was removed after XP.
// PBT_APMSUSPEND is a notification: the machine suspends roughly two seconds
// later whether or not anything has finished. sleepBudget() is sized for that,
// and the ACK remains worth waiting for as the only evidence the IR fired.
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
// One worker serialises sends, so HIDTransport's single-threaded assumption
// holds by construction and no lock is needed there. SERVICE_STATUS is a
// different matter: the worker advances checkpoints while the handler also
// reports state, so statusMutex_ guards it.
class WindowsPowerMonitor : public IPowerMonitor {
public:
    WindowsPowerMonitor();
    ~WindowsPowerMonitor() override;

    void setOnSleep(std::function<bool()> cb) override;
    void setOnWake(std::function<bool()> cb) override;
    void setOnShutdown(std::function<bool()> cb) override;

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

    enum class Trigger { DisplayState, Suspend, Resume, Startup };

    void workerLoop();
    void requestState(bool on, Trigger why);
    void requestShutdown();
    void bumpGeneration();

    // Reports a service state to the SCM. Advances the checkpoint for the
    // _PENDING states, which is how the SCM is told work is still progressing.
    void reportStatus(DWORD state, DWORD exitCode = NO_ERROR);
    void reportPending();

    std::function<bool()> onSleep_;
    std::function<bool()> onWake_;
    std::function<bool()> onShutdown_;

    SERVICE_STATUS        status_       = {};
    SERVICE_STATUS_HANDLE statusHandle_ = nullptr;
    std::mutex            statusMutex_;

    HPOWERNOTIFY displayNotify_ = nullptr;

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
    // opinion. Written and read only on the SCM handler thread, which delivers
    // controls one at a time, so it needs no lock.
    std::optional<bool> lastDisplayState_;
};

#endif // _WIN32
