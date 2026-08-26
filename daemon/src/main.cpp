// ESP32 IR Remote — PC Daemon
//
// Monitors system power events and sends ON/OFF commands to the ESP32
// over USB HID. Uses a systemd inhibitor lock to delay sleep and shutdown
// until the ESP32 confirms the IR signal has been transmitted via ACK.
//
// Boot     → send ON  (TV on when PC boots; skipped on a mere service restart)
// Sleep    → send OFF, wait for ACK, release inhibitor lock
// Wake     → send ON  (no lock needed — system is already running)
// Shutdown → send OFF, wait for ACK, release inhibitor lock

#include "IPowerMonitor.h"
#include "ITransport.h"
#include "HIDTransport.h"

#ifdef __linux__
#include "LinuxPowerMonitor.h"
#elif defined(_WIN32)
#include "WindowsPowerMonitor.h"
#include <windows.h>
#include <cstdio>
#include <string>
#endif

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

// Must match DEVICE_VID and DEVICE_PID in the firmware.
// Development placeholder values — replace with a registered VID/PID before
// commercial release.
static constexpr uint16_t DEVICE_VID = 0x1234;
static constexpr uint16_t DEVICE_PID = 0x5678;

#ifdef __linux__

// How recently the machine must have booted for a service start to count as a
// boot. Generous on purpose: a slow machine that misses the window merely fails
// to turn the TV on, which the user can undo with their remote, whereas a tight
// window would make that the common case.
static constexpr double BOOT_WINDOW_SECONDS = 180.0;

// True if this process is starting because the machine booted, rather than
// because the service was restarted under a machine that was already up.
//
// The startup ON exists to mirror a boot. It also fired on every `systemctl
// restart`, package upgrade and Restart=on-failure, so a daemon that crashed at
// 3am turned the TV on — and a user who had deliberately switched the TV off
// while leaving the PC running had it switched back on under them.
//
// Linux only, in definition as well as in use. The obvious Windows translation
// — GetTickCount64() — is wrong rather than merely unavailable: Fast Startup
// hibernates the kernel session instead of shutting down, so the tick count
// survives a power cycle and every boot reads as a restart. Windows answers the
// same question from the console display state instead, which is a better
// signal anyway: it asks whether anyone is actually there.
static bool systemJustBooted() {
    std::ifstream uptimeFile("/proc/uptime");
    double uptimeSeconds = 0.0;
    if (uptimeFile >> uptimeSeconds) {
        return uptimeSeconds < BOOT_WINDOW_SECONDS;
    }
    // /proc unreadable — fall through to the old unconditional behaviour rather
    // than skipping the ON a real boot was owed.
    return true;
}

#endif // __linux__

#ifdef _WIN32

// True if --console appeared on the command line. A substring match on the raw
// command line rather than argv parsing, so main()'s signature — and therefore
// the Linux build — stays exactly as it was for one Windows-only flag.
static bool consoleMode() {
    const wchar_t* cmdline = GetCommandLineW();
    return cmdline && wcsstr(cmdline, L"--console") != nullptr;
}

// Only one daemon may hold the ESP32 at a time.
//
// hidapi's Win32 backend opens the device with read and write sharing, so a
// hand-run copy and the installed service can both hold it — and either can
// consume the other's ACK, which is a reply accepted for a command it does not
// answer. That is the failure the sequence byte exists to eliminate, and the
// sequence byte cannot catch this one: the reply is well-formed and correctly
// numbered, just for somebody else's request.
//
// The mutex lives in the global namespace because the service runs in session 0
// and a hand-run copy does not, so a session-local name would not see it. A
// second instance normally gets ERROR_ALREADY_EXISTS; an unprivileged one may
// instead be refused outright by the DACL on the service's mutex, so
// ERROR_ACCESS_DENIED counts as "already running" too.
static HANDLE g_singleInstance = nullptr;

static bool claimSingleInstance() {
    g_singleInstance = CreateMutexW(nullptr, TRUE, L"Global\\ESP32IRRemote_SingleInstance");
    const DWORD err = GetLastError();

    if (g_singleInstance && err != ERROR_ALREADY_EXISTS) return true;

    if (g_singleInstance) {
        CloseHandle(g_singleInstance);
        g_singleInstance = nullptr;
    }
    return false;
}

// %ProgramData%\ESP32IRRemote — writable by a service under any account,
// unlike the binary's own directory under Program Files. The account this
// service runs as and where it can put a log are one decision, not two.
//
// Read from the environment rather than through SHGetKnownFolderPath. The
// KNOWNFOLDERID constants are declared by the SDK but defined in a library
// whose name and availability vary by SDK version, which turns one log path
// into a link-time question — the same one already avoided by spelling out
// GUID_CONSOLE_DISPLAY_STATE. ProgramData is a system-wide variable present in
// the environment block every service inherits, and it is not user-redirectable
// the way the per-user known folders are, so the indirection buys nothing here.
static std::wstring logDirectory() {
    wchar_t env[MAX_PATH] = {0};
    const DWORD len = GetEnvironmentVariableW(L"ProgramData", env, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(env) + L"\\ESP32IRRemote";
    }
    return L"C:\\ProgramData\\ESP32IRRemote";
}

// A Windows service has no console, so every std::cout would go nowhere.
// Reopening stdout and stderr onto a file keeps every existing log statement in
// the daemon working untouched, and keeps the output comparable with what Linux
// writes to the journal. std::cout is synced with stdio by default, which is
// what makes redirecting the C stream carry the C++ one with it.
static void redirectLogsToFile() {
    const std::wstring dir  = logDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring path = dir + L"\\daemon.log";
    const std::wstring prev = dir + L"\\daemon.log.old";

    // Size check before opening — insurance, not a rotation scheme. At a
    // handful of lines a day this is under a megabyte a year. It has to happen
    // before the file is opened: Windows has no rename-over-open, so a crashed
    // instance still holding the file during the restart delay makes this fail,
    // which is tolerated rather than treated as an error.
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const ULONGLONG size =
            (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > 1024ULL * 1024ULL) {
            MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    FILE* out = nullptr;
    FILE* err = nullptr;
    _wfreopen_s(&out, path.c_str(), L"a", stdout);
    _wfreopen_s(&err, path.c_str(), L"a", stderr);
}

#endif // _WIN32

int main() {
    // Flush stdout on every write.
    //
    // std::cout is fully buffered whenever stdout is not a terminal, which is
    // exactly the case under systemd — stdout there is a pipe to the journal.
    // Left alone, log lines sit in a 4KB buffer for months of normal operation,
    // and anything written just before systemd SIGTERMs us on shutdown is lost
    // outright, including the OFF confirmation. std::cerr is unbuffered and
    // tied to std::cout, so errors always got through while successes vanished.
    // This daemon logs a handful of lines a day, so flushing every time costs
    // nothing.
    std::cout << std::unitbuf;

#ifdef _WIN32
    // Both of these happen before the first log line: the destination has to be
    // decided before anything is written to it, and a second instance must not
    // get as far as opening the device.
    const bool console = consoleMode();
    if (console) {
        // The log strings are UTF-8, because the sources are and Linux writes
        // them to the journal unchanged. A Windows console defaults to an OEM
        // code page and renders every em-dash as "ÔÇö". Switching the console
        // rather than the strings keeps both platforms emitting identical
        // bytes, which is the property that makes their logs comparable.
        //
        // The log file needs no equivalent: it receives the same UTF-8 bytes.
        // Readers do have to be told — Get-Content in Windows PowerShell 5.1
        // assumes the ANSI code page and needs -Encoding UTF8.
        SetConsoleOutputCP(CP_UTF8);
    } else {
        redirectLogsToFile();
    }

    if (!claimSingleInstance()) {
        std::cerr << "[error] Another instance is already running — refusing to start.\n"
                     "[error] Stop the esp32-ir-remote service before running by hand;\n"
                     "[error] two openers of the same HID device steal each other's ACKs.\n";
        return 1;
    }
#endif

    // --- Transport ---
    std::unique_ptr<ITransport> transport;
    try {
        transport = std::make_unique<HIDTransport>(DEVICE_VID, DEVICE_PID);
    } catch (const std::exception& e) {
        std::cerr << "[error] " << e.what() << "\n";
        return 1;
    }

    // --- Power monitor ---
    std::unique_ptr<IPowerMonitor> monitor;

#ifdef __linux__
    try {
        monitor = std::make_unique<LinuxPowerMonitor>();
        std::cout << "[monitor] Connected to systemd-logind\n";
    } catch (const std::exception& e) {
        std::cerr << "[error] Failed to connect to D-Bus: " << e.what() << "\n";
        return 1;
    }
#elif defined(_WIN32)
    monitor = std::make_unique<WindowsPowerMonitor>(DEVICE_VID, DEVICE_PID);
    std::cout << "[monitor] Windows Service power monitor initialised\n";
#else
    std::cerr << "[error] No power monitor available for this platform\n";
    return 1;
#endif

    // --- Wire up callbacks ---
    // send() blocks until ACK is received from the ESP32, so by the time each
    // callback returns the IR signal has been confirmed transmitted.
    // LinuxPowerMonitor releases the inhibitor lock immediately after the callback.
    //
    // The outcome is always reported honestly. A failed command previously
    // logged as though it had succeeded, which sent debugging in entirely the
    // wrong direction — an unplugged ESP32 looked identical to a working one.
    // Returns what it was given, so a caller that needs to know whether the
    // device confirmed can have it without a second variable. The Windows
    // monitor suppresses repeat commands and must not remember an unconfirmed
    // one as the TV's new state.
    auto report = [](const char* cmd, const char* event, bool ok) {
        if (ok) {
            std::cout << "[cmd] " << cmd << " sent and ACK received (" << event << ")\n";
        } else {
            std::cerr << "[cmd] " << cmd << " FAILED — no ACK, TV state not changed ("
                      << event << ")\n";
        }
        return ok;
    };

    // The sleep path is the one event a platform may not be able to wait out.
    // The monitor answers how long it has, because it is the thing that knows
    // why the callback is running — a transport has no idea. Linux does not
    // override it and gets the transport's default; Windows asks for less,
    // because a suspend proceeds about two seconds after announcing itself.
    monitor->setOnSleep([&]() {
        return report("OFF", "sleep", transport->send("OFF", monitor->sleepBudget()));
    });

    monitor->setOnWake([&]() {
        return report("ON", "wake", transport->send("ON"));
    });

    monitor->setOnShutdown([&]() {
        return report("OFF", "shutdown", transport->send("OFF"));
    });

    // Device presence, where the platform reports it. No platform conditional:
    // the setter is a no-op on an implementation that never calls back, which
    // is what Linux does — there the transport reopens on a failed write, as it
    // always has.
    //
    // Only the removal is acted on here. An arrival needs the TV re-asserted
    // rather than the handle touched, and the monitor already owns that
    // decision because only it knows what state the display is in.
    //
    // The monitor guarantees this runs on the same thread as the power
    // callbacks, which is what makes calling into the transport here safe —
    // HIDTransport is single-threaded by design and holds no lock.
    monitor->setOnDeviceChange([&](bool present) {
        if (!present) transport->invalidate();
    });

#ifdef __linux__
    // Send ON at startup, but only when the service start actually corresponds
    // to the machine booting — see systemJustBooted().
    //
    // Linux only. Windows has no working equivalent of the uptime reading —
    // Fast Startup hibernates the kernel session rather than shutting down, so
    // the tick count does not reset and every power-on looks like a restart.
    // WindowsPowerMonitor asserts the opening state from the console display
    // state instead, which answers "is anyone there?" directly.
    if (systemJustBooted()) {
        report("ON", "startup", transport->send("ON"));
    } else {
        std::cout << "[cmd] ON skipped — service restarted on an already-running "
                     "system, not a boot\n";
    }
#endif

#ifdef _WIN32
    // --console is a reachability check, not a way to run the daemon: power
    // events are delivered to services only, so there is nothing for a
    // foreground process to listen to. Sending one command and reporting the
    // ACK is the useful thing it can do, and it needs no service installed.
    if (console) {
        std::cout << "[sys] Console mode — checking the ESP32 responds, then exiting\n";
        const bool ok = report("ON", "console check", transport->send("ON"));
        return ok ? 0 : 1;
    }
#endif

    // Blocks here until the process is killed by systemd on shutdown.
    std::cout << "[sys] Daemon running\n";
    monitor->run();

    return 0;
}
