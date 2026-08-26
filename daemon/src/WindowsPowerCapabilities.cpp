#ifdef _WIN32

#include "WindowsPowerCapabilities.h"

#include <windows.h>
#include <powrprof.h>

namespace {

// SYSTEM_POWER_CAPABILITIES, declared here in full rather than taken from the
// toolchain's headers.
//
// The two fields this daemon most needs — AoAc, which is how a machine says it
// is Modern Standby, and Hiberboot, which is Fast Startup — were appended by
// Microsoft into bytes an earlier version of the struct reserved as spare. A
// current Windows SDK declares them; mingw-w64's winnt.h still carries the older
// layout and does not, so the cross-compile the tree is checked with would not
// build against the SDK definition even though MSVC would.
//
// Depending on the header therefore means the capability report exists or not
// according to which toolchain compiled it, which is the worst of the options:
// the field that says what kind of machine this is would go missing on the
// builds least likely to be noticed. The layout is documented and stable —
// growth has only ever been into the reserved bytes — so declaring it once here
// makes the reading identical on every toolchain.
//
// The static_assert below is the guard. If a future SDK grows the struct past
// this, the size check fails at compile time rather than the OS quietly
// rejecting an output buffer it considers too small.
struct SystemPowerCapabilitiesFull {
    BOOLEAN PowerButtonPresent;
    BOOLEAN SleepButtonPresent;
    BOOLEAN LidPresent;
    BOOLEAN SystemS1;
    BOOLEAN SystemS2;
    BOOLEAN SystemS3;
    BOOLEAN SystemS4;
    BOOLEAN SystemS5;
    BOOLEAN HiberFilePresent;
    BOOLEAN FullWake;
    BOOLEAN VideoDimPresent;
    BOOLEAN ApmPresent;
    BOOLEAN UpsPresent;
    BOOLEAN ThermalControl;
    BOOLEAN ProcessorThrottle;
    UCHAR   ProcessorMinThrottle;
    UCHAR   ProcessorMaxThrottle;
    BOOLEAN FastSystemS4;
    BOOLEAN Hiberboot;              // Fast Startup
    BOOLEAN WakeAlarmPresent;
    BOOLEAN AoAc;                   // Modern Standby / S0 low power idle
    BOOLEAN DiskSpinDown;
    BYTE    HiberFileType;
    BOOLEAN AoAcConnectivitySupported;
    BYTE    spare3[6];
    BOOLEAN SystemBatteriesPresent;
    BOOLEAN BatteriesAreShortTerm;
    BATTERY_REPORTING_SCALE BatteryScale[3];
    SYSTEM_POWER_STATE AcOnLineWake;
    SYSTEM_POWER_STATE SoftLidWake;
    SYSTEM_POWER_STATE RtcWake;
    SYSTEM_POWER_STATE MinDeviceWakeState;
    SYSTEM_POWER_STATE DefaultLowLatencyWake;
};

// The buffer handed to the OS must be at least the size the OS expects. Both
// definitions describe the same documented structure, so a toolchain whose own
// declaration is *larger* than this one means this one has fallen behind and
// needs the new fields appending.
static_assert(sizeof(SystemPowerCapabilitiesFull) >= sizeof(SYSTEM_POWER_CAPABILITIES),
              "SYSTEM_POWER_CAPABILITIES has grown past the layout declared here — "
              "append the new fields to SystemPowerCapabilitiesFull");

// CallNtPowerInformation returns an NTSTATUS, not a Win32 error. Zero is
// STATUS_SUCCESS; anything else is a failure whose value is not a GetLastError()
// code and must not be printed as one.
constexpr LONG NT_SUCCESS_STATUS = 0;

const char* yesNo(bool v) { return v ? "yes" : "no"; }

} // namespace

WindowsPowerCapabilities queryPowerCapabilities() {
    WindowsPowerCapabilities caps;

    SystemPowerCapabilitiesFull spc = {};
    const LONG status = CallNtPowerInformation(SystemPowerCapabilities,
                                               nullptr, 0,
                                               &spc, sizeof(spc));
    if (status != NT_SUCCESS_STATUS) {
        // queried stays false. The caller reports "unknown", which is the
        // honest answer and distinct from a machine that supports nothing.
        return caps;
    }

    caps.queried          = true;
    caps.s1               = spc.SystemS1 != FALSE;
    caps.s2               = spc.SystemS2 != FALSE;
    caps.s3               = spc.SystemS3 != FALSE;
    caps.s4               = spc.SystemS4 != FALSE;
    caps.hiberFilePresent = spc.HiberFilePresent != FALSE;
    caps.modernStandby    = spc.AoAc != FALSE;
    caps.fastStartup      = (spc.Hiberboot != FALSE);

    return caps;
}

std::string WindowsPowerCapabilities::summary() const {
    if (!queried) {
        return "power model unknown — the OS did not answer";
    }

    // Sleep model first: it is the axis that decides whether the suspend and
    // resume events arrive at all, and therefore the one that changes how the
    // rest of the log should be read.
    //
    // Modern Standby and S3 are mutually exclusive platform properties, so this
    // is a choice between them rather than a list. A machine reporting both, or
    // neither, is a real finding and says so rather than being tidied into the
    // nearest plausible row.
    std::string sleepModel;
    if (modernStandby && s3) {
        sleepModel = "BOTH Modern Standby and S3 reported — unexpected";
    } else if (modernStandby) {
        sleepModel = "Modern Standby (S0 low power idle)";
    } else if (s3) {
        sleepModel = "classic S3 suspend-to-RAM";
    } else if (s1 || s2) {
        sleepModel = "S1/S2 only — no S3, no Modern Standby";
    } else {
        sleepModel = "no supported sleep state";
    }

    std::string hibernate;
    if (s4 && hiberFilePresent)      hibernate = "hibernate available";
    else if (s4)                     hibernate = "hibernate supported but disabled";
    else                             hibernate = "no hibernate";

    std::string boot = "Fast Startup unknown";
    if (fastStartup.has_value()) {
        boot = *fastStartup ? "Fast Startup on" : "Fast Startup off";
    }

    return sleepModel + ", " + hibernate + ", " + boot;
}

std::vector<std::string> WindowsPowerCapabilities::details() const {
    std::vector<std::string> lines;

    if (!queried) {
        lines.push_back("  capabilities unavailable — CallNtPowerInformation failed");
        return lines;
    }

    lines.push_back(std::string("  S1=")   + yesNo(s1)
                  + "  S2="                + yesNo(s2)
                  + "  S3="                + yesNo(s3)
                  + "  S4="                + yesNo(s4));
    lines.push_back(std::string("  hiberfil present=") + yesNo(hiberFilePresent)
                  + "  Modern Standby (AoAc)="         + yesNo(modernStandby));
    if (fastStartup.has_value()) {
        lines.push_back(std::string("  Fast Startup (Hiberboot)=") + yesNo(*fastStartup));
    }
    return lines;
}

#endif // _WIN32
