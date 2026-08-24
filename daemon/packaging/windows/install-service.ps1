# Installs the ESP32 IR Remote daemon as a Windows Service.
#
# The equivalent of Linux's postinst plus the unit file, minus a package
# manager. This is the "sc create now, MSI at first release" step from the
# brief: enough to run and test the daemon, not enough to ship it. An installer
# has to do more — upgrade in place, stop the service before replacing the
# binary, and remove its directories on uninstall.
#
# Run from an elevated PowerShell:
#   .\install-service.ps1 -BinaryPath C:\path\to\esp32-ir-daemon.exe
#
# Uninstall:
#   .\install-service.ps1 -Uninstall

[CmdletBinding()]
param(
    [string]$BinaryPath,
    [string]$ServiceName = "esp32-ir-remote",
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
# binPath= needs the quoting below: sc.exe parses its own argument, so a path
# with spaces has to arrive already quoted.
sc.exe create $ServiceName `
    binPath= "`"$BinaryPath`"" `
    DisplayName= "ESP32 IR Remote" `
    start= auto `
    obj= LocalSystem | Out-Null

sc.exe description $ServiceName `
    "Mirrors PC power state to a TV over IR, as a CEC alternative." | Out-Null

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
