#pragma once
#ifdef _WIN32

#include <optional>
#include <string>
#include <vector>

// What power model this machine implements.
//
// The daemon behaves identically on all of them — display state is the primary
// signal precisely because it is the one mechanism common to all three. But
// which model a machine is decides which *other* events ever arrive, and
// therefore what a log is evidence of. An absent PBT_APMSUSPEND is a defect on
// an S3 desktop and the expected behaviour on a Modern Standby laptop, and
// nothing in the log tells those two readings apart unless the machine says
// which it is.
//
// Recorded by the daemon at startup rather than reconstructed afterwards from a
// `powercfg /a` run by hand — by then the machine may have been rebooted into a
// different firmware setting, and the log and the capability report would be
// describing different configurations without saying so.
//
// Queries and formats only. It writes nothing itself, so it carries no
// dependency on how this platform happens to log today — the caller owns that.
struct WindowsPowerCapabilities {
    // False when the OS would not answer. Every field below is then unread, and
    // must not be reported as though the machine had said "no" to all of them.
    bool queried = false;

    bool s1 = false;
    bool s2 = false;
    bool s3 = false;                // classic suspend-to-RAM
    bool s4 = false;                // hibernate supported by the platform
    bool hiberFilePresent = false;  // ...and enabled, hiberfil.sys present
    bool modernStandby = false;     // AoAc — S0 low power idle

    // Fast Startup, taken from SYSTEM_POWER_CAPABILITIES::Hiberboot rather than
    // from HiberbootEnabled in the registry. The registry value is the setting;
    // this is the effective state, and the two differ on a machine where
    // hibernation was turned off underneath the setting. Unset only when the
    // capabilities could not be read at all.
    std::optional<bool> fastStartup;

    // One line naming the configuration in the same vocabulary as the brief's
    // three-configuration table, so a log and that table can be read against
    // each other without translation.
    std::string summary() const;

    // The individual readings, one per line, for the log. Separate from
    // summary() because the summary is a judgement — "Modern Standby" — and
    // these are the observations it was made from. When the judgement is
    // surprising, the observations are what settle it.
    std::vector<std::string> details() const;
};

// Best-effort. A machine that refuses to answer yields queried == false rather
// than throwing: not knowing the power model degrades the diagnostics, and
// there is no version of this worth failing to start over.
WindowsPowerCapabilities queryPowerCapabilities();

#endif // _WIN32
