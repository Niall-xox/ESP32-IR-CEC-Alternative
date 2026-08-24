# Installs the ESP32 IR Remote daemon as a Windows Service.
#
# The equivalent of Linux's postinst plus the unit file, minus a package
# manager. This is the "sc create now, MSI at first release" step from the
# brief: enough to run and test the daemon, not enough to ship it. An installer
# has to do more — upgrade in place, stop the service before replacing the
# binary, and remove its directories on uninstall.
#
# Run from an elevated PowerShell. Windows client defaults to an execution
# policy of Restricted, under which no .ps1 runs at all, so the bypass is not
# optional — and -Scope Process confines it to that one shell rather than
# changing the machine:
#
#   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#   .\install-service.ps1 -BinaryPath C:\path\to\esp32-ir-daemon.exe
#
# Or without touching policy at all, from an elevated prompt:
#
#   powershell -ExecutionPolicy Bypass -File .\install-service.ps1 `
#              -BinaryPath C:\path\to\esp32-ir-daemon.exe
#
# Uninstall:
#   .\install-service.ps1 -Uninstall
#
# That this needs a policy bypass at all is part of why the brief lists an MSI
# as the real answer: an installer a user has to disarm the shell to run is not
# something to hand to anybody else.

[CmdletBinding()]
param(
    [string]$BinaryPath,
    [string]$ServiceName = "esp32-ir-remote",

    # Must match DEVICE_VID / DEVICE_PID in firmware/src/main.cpp and
    # daemon/src/main.cpp. Lowercase or uppercase hex, no 0x. This is the
    # fourth place the pair appears — see the brief's USB device identity
    # table, which now lists all four.
    #
    # Not named -Pid: $PID is a PowerShell automatic variable holding the
    # process ID, and binding a parameter to it fails in ways that are not
    # obvious from the error.
    [string]$VendorId  = "1234",
    [string]$ProductId = "5678",

    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run from an elevated PowerShell session."
}

if ($Uninstall) {
    sc.exe stop   $ServiceName | Out-Null
    sc.exe delete $ServiceName | Out-Null
    Write-Host "Removed service '$ServiceName'."
    Write-Host "The log directory C:\ProgramData\ESP32IRRemote was left in place."
    return
}

if (-not $BinaryPath) { throw "-BinaryPath is required when installing." }
$BinaryPath = (Resolve-Path $BinaryPath).Path
if (-not (Test-Path $BinaryPath)) { throw "No binary at $BinaryPath" }

# --- Create -----------------------------------------------------------------
#
# start= auto rather than delayed-auto. A delayed start would postpone the
# service by up to two minutes, and the first thing it does on starting is read
# the display state and assert the TV to match — so a delayed start is a delayed
# TV, on exactly the boot the feature exists for.
#
# obj= LocalSystem for now. The brief's plan is to move to LocalService once the
# daemon is known to work, mirroring Linux, which ran as root until it did and
# was then moved to the esp32ir account. Whether hidapi's CreateFile open works
# under a low-privilege account is answerable in minutes with a working binary
# and not at all without one.
#
# New-Service rather than `sc.exe create`.
#
# sc.exe parses its own command tail rather than using CommandLineToArgvW, so
# binPath= has to arrive already quoted when the path contains a space — and
# getting a quoted string through PowerShell's native-argument handling
# intact is unreliable enough that it is not worth relying on for the one
# argument whose corruption produces a service that installs cleanly and then
# fails to start with a misleading error.
#
# New-Service takes the path as a real parameter. The embedded quotes below are
# deliberate and are what Windows stores: an unquoted service path with a space
# is the classic unquoted-service-path weakness, where Windows would try
# C:\Program.exe before the real target.
$binaryPathName = '"' + $BinaryPath + '"'

New-Service -Name $ServiceName `
            -BinaryPathName $binaryPathName `
            -DisplayName "ESP32 IR Remote" `
            -Description "Mirrors PC power state to a TV over IR, as a CEC alternative." `
            -StartupType Automatic | Out-Null

# LocalSystem is New-Service's default account, which is what the brief calls
# for now; the move to LocalService comes once the daemon is known to work,
# mirroring Linux, which ran as root until it did and was then moved to the
# esp32ir account. Whether hidapi's CreateFile open succeeds under a
# low-privilege account is answerable in minutes with a working binary and not
# at all without one.
#
# Automatic rather than delayed-auto. A delayed start would postpone the service
# by up to two minutes, and the first thing it does is read the display state
# and assert the TV to match — so a delayed start is a delayed TV, on exactly
# the boot the feature exists for.

# --- Recovery ---------------------------------------------------------------
#
# Mirrors Restart=on-failure / RestartSec=5 on Linux. reset= 86400 means the
# failure count returns to zero after a day without one, so an occasional
# failure does not eventually exhaust the actions.
#
# The brief notes this is still short of what a commercial service specifies:
# there is a decision to make about whether to stop retrying eventually, rather
# than restarting forever.
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/5000/restart/5000 | Out-Null

# --- Preshutdown timeout ----------------------------------------------------
#
# Set explicitly rather than inherited. There is no sc.exe verb for it — the
# documented API is ChangeServiceConfig2 with SERVICE_CONFIG_PRESHUTDOWN_INFO,
# and this registry value is what that writes.
#
# 60s is far more than the send needs (the budget is 4s) and far less than the
# 3-minute default, which would hold up a shutdown that long if the daemon ever
# hung. Being explicit is the point: the timeout is now a decision rather than
# whatever the OS defaults to this year.
$key = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
New-ItemProperty -Path $key -Name "PreshutdownTimeout" `
                 -Value 60000 -PropertyType DWord -Force | Out-Null

# --- USB selective suspend --------------------------------------------------
#
# Windows powers down an idle USB device when nothing holds a handle open, and
# this ESP32 does not come back from it: the device NAKs its OUT endpoint
# forever, so every write fails with a WaitForSingleObject timeout while
# hid_open still succeeds. It stays that way until the device is physically
# re-enumerated, which is why the symptom looks like "works for a while, then
# refuses until replugged".
#
# Diagnosed 2026-08-24 on the Modern Standby test laptop. The global power
# scheme setting was not enough — a per-device value overrides it, and on a
# laptop the AC and DC values are separate, so a scheme change made on battery
# power silently does nothing.
#
# This is a workaround for a firmware defect, not a fix. The device ought to
# resume. It is also per-instance: a different physical unit enumerates with a
# new serial and gets Windows' defaults again. The real answer is an INF that
# carries these values for the hardware ID, which arrives with the driver
# package and code signing already listed as gaps.
function Disable-UsbSelectiveSuspend {
    param([string]$Vid, [string]$Pid)

    $hardwareKey = "VID_$Vid&PID_$Pid"
    $enumRoot    = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"

    # Collected first, then written. A counter incremented inside a piped
    # ForEach-Object block lands in the wrong scope, and the function would
    # report zero while having done the work — a lie in the one message
    # telling the operator whether the fix was applied.
    $paramKeys = @()
    foreach ($hw in @(Get-ChildItem $enumRoot -ErrorAction SilentlyContinue |
                      Where-Object { $_.PSChildName -like "$hardwareKey*" })) {
        foreach ($instance in @(Get-ChildItem $hw.PSPath -ErrorAction SilentlyContinue)) {
            $params = Join-Path $instance.PSPath "Device Parameters"
            if (Test-Path $params) { $paramKeys += $params }
        }
    }

    foreach ($params in $paramKeys) {
        New-ItemProperty -Path $params -Name "SelectiveSuspendEnabled" `
                         -Value 0 -PropertyType DWord -Force | Out-Null
        New-ItemProperty -Path $params -Name "EnhancedPowerManagementEnabled" `
                         -Value 0 -PropertyType DWord -Force | Out-Null
    }

    return $paramKeys.Count
}

$suspendApplied = Disable-UsbSelectiveSuspend -Vid $VendorId -Pid $ProductId
if ($suspendApplied -gt 0) {
    Write-Host "Disabled USB selective suspend on $suspendApplied device instance(s)."
    Write-Host "Unplug and replug the ESP32 for that to take effect."
} else {
    # Not fatal: the device simply is not plugged in. Saying so is the point —
    # a silent no-op here reappears later as the intermittent write failure,
    # with nothing connecting the two.
    Write-Warning ("No USB device matching VID_{0}&PID_{1} is present, so selective " +
                   "suspend was not disabled for it. Plug the ESP32 in and re-run " +
                   "this script, or writes will start failing once it has been " +
                   "idle." -f $VendorId, $ProductId)
}

# --- Log directory ----------------------------------------------------------
#
# Created here rather than left to the daemon so the ACL is set once, by the
# installer, while it still has the privilege to do it. Matters for the move to
# LocalService: that account cannot create a directory under %ProgramData%
# itself, and a service with nowhere to log fails silently by definition.
$logDir = Join-Path $env:ProgramData "ESP32IRRemote"
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
icacls $logDir /grant "*S-1-5-19:(OI)(CI)M" | Out-Null   # NT AUTHORITY\LOCAL SERVICE
icacls $logDir /grant "*S-1-5-18:(OI)(CI)F" | Out-Null   # NT AUTHORITY\SYSTEM

sc.exe start $ServiceName | Out-Null

Write-Host "Installed and started '$ServiceName'."
Write-Host "Log: $logDir\daemon.log"
Write-Host ""
Write-Host "Follow it with:"
Write-Host "  Get-Content '$logDir\daemon.log' -Wait -Tail 20"
