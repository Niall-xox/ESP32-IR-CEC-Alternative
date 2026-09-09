#ifdef _WIN32

#include "WindowsPowerCapabilities.h"

#include <windows.h>
#include <powrprof.h>

namespace {

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
    BOOLEAN Hiberboot;
    BOOLEAN WakeAlarmPresent;
    BOOLEAN AoAc;
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

static_assert(sizeof(SystemPowerCapabilitiesFull) >= sizeof(SYSTEM_POWER_CAPABILITIES),
              "SYSTEM_POWER_CAPABILITIES has grown past the layout declared here — "
              "append the new fields to SystemPowerCapabilitiesFull");

constexpr LONG NT_SUCCESS_STATUS = 0;

const char* yesNo(bool v) { return v ? "yes" : "no"; }

}

WindowsPowerCapabilities queryPowerCapabilities() {
    WindowsPowerCapabilities caps;

    SystemPowerCapabilitiesFull spc = {};
    const LONG status = CallNtPowerInformation(SystemPowerCapabilities,
                                               nullptr, 0,
                                               &spc, sizeof(spc));
    if (status != NT_SUCCESS_STATUS) {

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

#endif
