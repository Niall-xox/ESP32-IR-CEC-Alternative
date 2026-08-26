# Captures what a Windows machine is and what the daemon is doing on it.
#
# The Windows work has to be proven on three configurations — classic S3,
# hibernate, and Modern Standby — and a machine can only ever be one of them,
# because the sleep model is a property of the platform firmware. So the
# evidence arrives from different machines on different days, and the thing that
# makes it add up to a verification record rather than three anecdotes is that
# each machine reports itself the same way.
#
# That is all this does: read the state, write it down in a fixed order, and
# save it next to the log. It changes nothing.
#
#   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#   .\verify-windows.ps1
#   .\verify-windows.ps1 -Report C:\temp\desktop-s3.txt
#
# Does not need elevation to read most of this, but the service configuration
# and the device registry values are not readable without it — run it elevated
# and it reports everything, run it unprivileged and it says which parts it
# could not see rather than reporting them as absent.

[CmdletBinding()]
param(
    [string]$ServiceName = "esp32-ir-remote",
    [string]$VendorId    = "1234",
    [string]$ProductId   = "5678",

    # Where to write a copy. Defaults to the log directory, so the report and
    # the log it refers to stay together.
    [string]$Report,

    # How much of the daemon log to include.
    [int]$LogLines = 60
)

$ErrorActionPreference = "Continue"

$logDir  = Join-Path $env:ProgramData "ESP32IRRemote"
$logFile = Join-Path $logDir "daemon.log"

if (-not $Report) {
    $stamp  = Get-Date -Format "yyyyMMdd-HHmmss"
    $Report = Join-Path $logDir "verify-$($env:COMPUTERNAME)-$stamp.txt"
}

$elevated = ([Security.Principal.WindowsPrincipal] `
             [Security.Principal.WindowsIdentity]::GetCurrent()
            ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$out = New-Object System.Collections.Generic.List[string]
function Say([string]$line = "") { $out.Add($line); Write-Host $line }

Say "ESP32 IR Remote — Windows verification report"
Say ("Generated  : " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Say ("Machine    : $env:COMPUTERNAME")
Say ("Elevated   : $elevated")
if (-not $elevated) {
    Say "             (service config and device registry values need elevation)"
}
Say ("Windows    : " + (Get-CimInstance Win32_OperatingSystem).Caption +
     " build " + [System.Environment]::OSVersion.Version.Build)
Say ""

# --- 1. Which of the three configurations is this ---------------------------
#
# The single most important thing in the report. Everything below is read
# against it: an absent PBT_APMSUSPEND is a defect on an S3 desktop and the
# documented behaviour on a Modern Standby laptop.
Say "== Power model =="
Say ""
Say "powercfg /a:"
try {
    powercfg /a 2>&1 | ForEach-Object { Say ("  " + $_) }
} catch {
    Say "  powercfg unavailable: $_"
}
Say ""

# Fast Startup is the other axis, and it is not in `powercfg /a` output.
$hiberboot = $null
try {
    $hiberboot = (Get-ItemProperty `
        -Path "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power" `
        -Name "HiberbootEnabled" -ErrorAction Stop).HiberbootEnabled
} catch { }

if ($null -ne $hiberboot) {
    $state = if ($hiberboot -eq 1) { "on" } else { "off" }
    Say "Fast Startup (HiberbootEnabled): $state"
} else {
    # Absent is not the same as off — the documented default is enabled wherever
    # hibernation is available.
    Say "Fast Startup (HiberbootEnabled): value absent (defaults to on where hibernate exists)"
}
Say ""

# --- 2. The service ---------------------------------------------------------
Say "== Service '$ServiceName' =="
Say ""
$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $svc) {
    Say "  NOT INSTALLED"
} else {
    Say ("  Status     : " + $svc.Status)
    Say ("  StartType  : " + $svc.StartType)

    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    try {
        $cfg = Get-ItemProperty -Path $key -ErrorAction Stop
        Say ("  ImagePath  : " + $cfg.ImagePath)
        if ($null -ne $cfg.PreshutdownTimeout) {
            Say ("  Preshutdown: " + $cfg.PreshutdownTimeout + " ms")
        } else {
            Say "  Preshutdown: not set (inherits the 3-minute default)"
        }
    } catch {
        Say "  registry configuration unreadable (needs elevation)"
    }

    if ($elevated) {
        Say "  Recovery   :"
        sc.exe qfailure $ServiceName 2>&1 | ForEach-Object { Say ("    " + $_) }
    }
}
Say ""

# --- 3. The device ----------------------------------------------------------
Say "== ESP32 (VID_$VendorId & PID_$ProductId) =="
Say ""
$hwPattern = "*VID_$VendorId&PID_$ProductId*"
$devices = @(Get-PnpDevice -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -like $hwPattern })

if ($devices.Count -eq 0) {
    Say "  NOT PRESENT — plug the ESP32 in before testing anything below."
} else {
    foreach ($d in $devices) {
        Say ("  " + $d.Status.ToString().PadRight(8) + " " + $d.Class.ToString().PadRight(10) +
             " " + $d.InstanceId)
    }
}
Say ""

# The selective-suspend values the installer writes. Per-instance, so a
# different physical unit gets Windows' defaults back — which is exactly the
# kind of thing that turns into an intermittent failure nobody connects to
# having swapped the board.
if ($elevated) {
    Say "  Selective suspend (per device instance):"
    $enumRoot = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"
    $found = $false
    foreach ($hw in @(Get-ChildItem $enumRoot -ErrorAction SilentlyContinue |
                      Where-Object { $_.PSChildName -like "VID_$VendorId&PID_$ProductId*" })) {
        foreach ($instance in @(Get-ChildItem $hw.PSPath -ErrorAction SilentlyContinue)) {
            $params = Join-Path $instance.PSPath "Device Parameters"
            if (-not (Test-Path $params)) { continue }
            $found = $true
            $p  = Get-ItemProperty -Path $params -ErrorAction SilentlyContinue
            $ss = if ($null -ne $p.SelectiveSuspendEnabled) { $p.SelectiveSuspendEnabled } else { "unset" }
            $ep = if ($null -ne $p.EnhancedPowerManagementEnabled) { $p.EnhancedPowerManagementEnabled } else { "unset" }
            Say ("    " + $instance.PSChildName +
                 "  SelectiveSuspendEnabled=$ss  EnhancedPowerManagementEnabled=$ep")
        }
    }
    if (-not $found) { Say "    no registry instances (device has never been plugged into this machine)" }
    Say ""
}

# --- 4. The log -------------------------------------------------------------
Say "== Daemon log (last $LogLines lines) =="
Say ""
if (Test-Path $logFile) {
    Say ("  $logFile")
    Say ""
    Get-Content $logFile -Tail $LogLines -Encoding UTF8 |
        ForEach-Object { Say ("  " + $_) }
} else {
    Say "  No log at $logFile — the service has never run on this machine."
}
Say ""

# --- 5. What still needs doing by hand --------------------------------------
#
# Listed rather than checked, because none of it can be read out of the
# registry: every row needs somebody to put the machine into a state and watch
# what the TV does.
Say "== Manual checks — record the result against this machine =="
Say ""
Say "  [ ] Boot            TV comes on, log shows 'display state = on' at registration"
Say "  [ ] Display timeout TV goes off when the screen blanks"
Say "  [ ] Sleep           TV off; log shows the suspend send and how much budget it used"
Say "  [ ] Wake            TV on; check whether the ESP32 re-enumerated (arrival line)"
Say "  [ ] Hibernate       as sleep, but 'away for' should show the real elapsed time"
Say "  [ ] Resume from hib TV on, and the device arrival re-assert lands"
Say "  [ ] Shutdown        TV off before the machine goes; preshutdown gives minutes"
Say "  [ ] Unattended wake TV stays OFF — this is the 3am case, and the one that"
Say "                      is wrong if PBT_APMRESUMEAUTOMATIC is ever acted on"
Say ""

# --- Save -------------------------------------------------------------------
try {
    $dir = Split-Path -Parent $Report
    if ($dir -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    # UTF8 to match what the daemon writes, so the report and the log excerpt
    # inside it do not disagree about how an em-dash is encoded.
    $out | Out-File -FilePath $Report -Encoding UTF8
    Write-Host ""
    Write-Host "Report written to $Report"
} catch {
    Write-Warning "Could not write the report to $Report : $_"
}
