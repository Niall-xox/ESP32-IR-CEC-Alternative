# ESP32 IR Remote — Project Outline

Working reference for the project. Covers what the device does, how the two
halves talk to each other, the rules the implementation has to keep, and how to
build it on each platform.

Not user documentation — that comes later, separately.

**Contents**

- [Overview](#overview)
- [Current status](#current-status)
- [Hardware](#hardware)
- [Architecture](#architecture)
- [Communication protocol](#communication-protocol)
- [Firmware](#firmware) — [profiles](#manufacturer-profiles) · [button & display](#button--display-behaviour) · [storage](#storage-littlefs) · [web UI](#web-ui) · [source map](#firmware-source-map)
- [Daemon](#daemon) — [power events](#power-event-detection-linux) · [cross-platform](#cross-platform-design) · [Windows](#windows-status) · [source map](#daemon-source-map)
- [Windows — in progress](#windows--in-progress) ← **current focus**
- [Invariants](#invariants)
- [Building](#building) — [firmware](#firmware-build) · [Linux](#daemon--linux) · [NixOS](#nixos) · [Windows](#windows) · [packaging](#packaging--ci) · [sdbus-c++](#the-sdbus-c-constraint)
- [Known issues and deferred work](#known-issues-and-deferred-work)
- [History](#history)

---

## Overview

A USB-connected device that synchronises a TV's power state with a connected PC,
using the TV's built-in IR remote compatibility. This replicates the behaviour of
HDMI-CEC without requiring CEC support on either device.

| PC Event               | TV Action    |
|------------------------|--------------|
| Power on from shutdown | Turn TV on   |
| Wake from sleep        | Turn TV on   |
| Shutdown               | Turn TV off  |
| Enter sleep            | Turn TV off  |

**On Windows the display coming on also turns the TV on**, on every machine: a
screen lighting up is the most direct evidence there is that somebody is at the
PC, and on a Fast Startup machine it is the only route to an ON at boot.

**The screen *blanking* turns the TV off only where nothing else can** — a
Modern Standby machine, which never enters a classic suspend and so raises no
usable suspend event. A machine that reports classic S3 gets its OFF from the
suspend event instead, and an idle screen blank is ignored there, so the
behaviour matches Linux. The daemon reads which kind of machine it is at startup
and says so in its log. See
[driving from display state](#driving-from-display-state).

That split matters because the divergence is not free. On a Modern Standby
machine the TV switches off when the screen blanks, including under somebody who
is still sitting there watching something that failed to assert
`ES_DISPLAY_REQUIRED`. That is a real cost accepted only where the alternative
is a TV that never switches off at all.

Existing solutions (e.g. Wake-on-LAN, network-based control) are unreliable and
dependent on network state. This device operates at the USB layer, making it
robust and network-independent.

---

## Current status

**Working and verified on hardware:** USB HID transport with ACK-based inhibitor
release, multi-manufacturer profiles on LittleFS, the button, the OLED, WiFi
config mode and the web UI. Deployed on NixOS via the flake module and confirmed
running as an unprivileged service.

**Second hardening pass — verified on hardware 2026-08-24.** The sequenced
protocol, the uptime gate, the unconfigured-profile guard and the full
sleep/wake/shutdown/boot cycle have all been exercised against the real device
and daemon. Two items remain untested, neither on the power-sync path — see
[Still unverified on hardware](#still-unverified-on-hardware).

**Windows — the Modern Standby machine is fully verified, 2026-08-27.** One
binary covers classic S3, hibernate (S4) and Modern Standby (S0 low power idle).
The first install found [four defects](#the-first-install--four-defects) — three
of which would have stopped anybody installing it at all — and after those, every
row that machine can answer has been answered: all eleven per-machine tests, plus
Fast Startup boot, hibernate, a full Modern Standby cycle and an unattended wake.
The suspend grace period has its first real measurement, 206ms of 1500ms. Two
rows remain and both need an S3 machine with Fast Startup off. Pick it up at
[Resume here](#resume-here).

**Reviewed against the project's aims 2026-08-26, and two things changed.** The
brief had grown detailed enough to be internally consistent while drifting from
what the [Overview](#overview) table promises. Reading it back against those four
rows found an idle screen blank turning the TV off on machines that had a better
signal available, and a screen blank inheriting a suspend's 1500ms deadline
while nothing was waiting for it. Both are fixed, in the brief and in the code,
and both were exercised on 2026-08-27 — the display-off policy chose the right
branch and every command in the log names its own trigger rather than
inheriting `(sleep)`. The full reasoning is under
[the reversals](#five-decisions-the-sanity-check-reversed).

**Firmware flashed 2026-08-26, and Linux re-verified on it.** A full S3
sleep/wake cycle behaves exactly as it did before — see
[the second-pass table](#verified-on-hardware--second-pass).

The USB suspend recovery did **not** fire during that cycle, which the previous
version of this brief predicted it would. The prediction was wrong and the
reason is worth keeping: see
[what Linux actually did](#what-linux-actually-does-across-a-suspend). It was
first exercised on Windows on 2026-08-26: the device is cycled across a Modern
Standby resume, re-enumerates and answers.

**Three things block any public release:**

1. Registered VID/PID — currently placeholder `1234:5678`.
2. Confirmed discrete IR codes for Samsung, Sony, TCL and Hisense.
3. A LICENSE and a README — both packages already declare MIT.

Verification detail and the full outstanding list are in
[Known issues and deferred work](#known-issues-and-deferred-work).

---

## Hardware

- **ESP32-S3 SuperMini** — microcontroller running the firmware
- **IR LED** — transmits IR signals to the TV (GPIO 4)
- **OLED display (128×32, SSD1306)** — status feedback (SDA: GPIO 2, SCL: GPIO 3)
- **Tactile button** — profile cycling, config mode, factory reset (GPIO 5, GND)

**On the board name.** PlatformIO has no board definition for the ESP32-S3
SuperMini, so `platformio.ini` builds for `lolin_s3_mini` as a close-enough
match — same ESP32-S3 target, and the pin assignments above are set explicitly
in the firmware rather than inherited from the board definition. This is
deliberate, not an oversight. The CAD directory likewise models the housing
around an `ESP32-S3_Zero.step`, another similarly-sized S3 board used as a
stand-in.

### CAD sources

`3D modeling/` holds `ESP32-Remote-Housing.FCStd` — the FreeCAD master, and the
only housing file that is tracked — alongside `.STEP` component models for the
board, IR transmitter, OLED and button.

`.FCBak` and `.stl` are gitignored. FreeCAD writes a backup beside the master on
every save, and mesh exports are derived files that go stale the moment the model
changes; re-export from the `.FCStd` rather than trusting a committed one.
`git add -f` if a specific export ever needs shipping.

---

## Architecture

Two components:

**PC daemon** — a background process that monitors system power events and sends
commands to the ESP32 over USB. C++17, cross-platform.

**ESP32 firmware** — receives commands over USB HID, transmits the IR signal for
the active manufacturer profile, updates the OLED, and answers ACK or ERR.

---

## Communication protocol

### Wire format

Transport is USB HID, 64-byte vendor-defined reports. Both directions share one
payload layout:

| Byte | Daemon → ESP32 | ESP32 → Daemon |
|------|----------------|----------------|
| `[0]` | sequence, 1–255, never 0 | the same sequence, echoed unchanged |
| `[1..]` | NUL-terminated command | NUL-terminated response |

| Command | Response | Action |
|---------|----------|--------|
| `ON`  | `ACK` | Fire IR ON code for active profile |
| `OFF` | `ACK` | Fire IR OFF code for active profile |
| `ON` / `OFF`, code unset | `ERR` | Active profile's code for that direction is `0x0` — nothing transmitted |
| unknown | `ERR` | Unknown command received |

- IR dispatch is synchronous. ACK is sent only after the signal has fully
  transmitted, so the daemon can release its inhibitor lock the instant it
  arrives. No fixed delays anywhere in the pipeline.
- The sequence byte is echoed, never interpreted — the ESP32 needs no state to
  support it. It exists so the daemon can tell a reply to *this* command from a
  late reply to an earlier one.
- The daemon discards any reply whose sequence does not match, and keeps waiting
  for the right one until its budget expires.
- Communication is daemon-initiated only. The ESP32 never sends unsolicited
  reports.

### Version lockstep is deliberate

**Firmware and daemon must be updated together**, and the protocol carries no
compatibility path for a mismatched pair. This is a decision, not an oversight —
please do not "fix" it without reading the rest of this section.

Both halves ship together and the project is pre-release, so there is no
installed base to strand. Carrying fallbacks for versions nobody is running
would be machinery earning nothing, and it has a real cost beyond the code:
a compatibility path makes "the reply could not be correlated" into a silent,
accepted state. That is the same category of defect the sequence byte exists to
eliminate — a weaker guarantee that still reports success. Lockstep keeps the
guarantee unconditional, which is both easier to reason about and easier to keep
true.

A mismatch is loud rather than mysterious. The daemon logs:

```
[transport] Discarding unmatched reply (seq 79, expected 3) — stale reply,
            or firmware too old for the sequenced protocol
[transport] No response received for command: ON
```

That names the cause directly, which is the genuinely useful part of
compatibility — knowing *why* it stopped — at no cost. Reflash and it is fixed.

**What would change the calculus:** the daemon updating independently in the
field. It installs from a package and updates whenever the distribution ships
one; the firmware is flashed by hand over USB and in practice hardly ever is.
Once there are users who upgrade one without the other, a flag day stops being
acceptable and the wire format needs an additive path — new fields placed where
an older peer skips them, behind a string terminator or a field it already
ignores, with a reserved value meaning "absent".

**Trigger to revisit:** the first packaged release that reaches someone else.
Not before.

### Timing budget

`send()` runs inside a sleep or shutdown handler while a logind delay inhibitor
is held, and logind stops waiting after `InhibitDelayMaxSec` — **5 seconds by
default**. Every step of a send therefore draws on one shared 4-second budget:
device open, reopen-after-write-failure, and the ACK wait.

`SEND_BUDGET` in `HIDTransport.cpp` is the single knob. Raising it above 5s
reintroduces the problem regardless of what the individual timeouts say.

### USB device identity

The daemon finds the device by VID/PID — no port numbers or paths.

| Field | Value | Notes |
|-------|-------|-------|
| VID   | `0x1234` | Placeholder — replace before any release |
| PID   | `0x5678` | Placeholder — replace before any release |
| Product name | `ESP32 IR Remote` | Shown in system device list |
| Manufacturer | `ESP32-IR-CEC` | Shown in system device list |

These must match in **five** places:

| File | Form |
|---|---|
| `firmware/src/main.cpp` | `0x` hex |
| `daemon/src/main.cpp` | `0x` hex |
| `99-esp32-ir-remote.rules` | lowercase hex, no `0x` |
| `packaging/windows/install-service.ps1` | hex, no `0x`, as `-VendorId` / `-ProductId` defaults |
| `packaging/windows/verify-windows.ps1` | hex, no `0x`, same defaults |

The last two are Windows-side and newer than the rest. The installer needs the
pair to find the device's registry key and disable USB selective suspend on it;
the verification script needs it to report whether the device is present and
what those registry values currently say.

That the count has gone from three to four to five is the argument for a single
generated header rather than five hand-kept copies — and the reason the
placeholder pair is listed as
[blocking release](#blocking-release) rather than as a tidy-up. Five places is
where "change them together" stops being advice and starts being a defect
waiting for the one that gets missed.

For open-source release, a free registered pair is available from
[pid.codes](https://pid.codes). Commercial release needs a USB-IF VID.

---

## Firmware

### Manufacturer profiles

IR codes are stored per-profile in `/profiles.json` on LittleFS:

- `name` — display name (e.g. `LG`)
- `protocol` — `NEC`, `SAMSUNG` or `SONY`
- `on_code` / `off_code` — discrete power codes
- `visible` — whether the profile appears in the button cycle

Discrete on/off codes are preferred over toggle codes: they guarantee the correct
TV state regardless of any prior state drift.

Defaults are hardcoded in firmware and written to LittleFS on first boot. Factory
reset rewrites from these.

| Profile  | Protocol | ON Code      | OFF Code     | Notes |
|----------|----------|--------------|--------------|-------|
| LG       | NEC      | `0x20DF23DC` | `0x20DFA35C` | Confirmed working on LG C2 |
| Samsung  | SAMSUNG  | placeholder  | placeholder  | |
| Sony     | SONY     | placeholder  | placeholder  | |
| TCL      | NEC      | placeholder  | placeholder  | |
| Hisense  | NEC      | placeholder  | placeholder  | |

`0x20DFB34C` was tested as an LG ON code but triggers a smart TV network prompt
on the C2 — it maps to a smart home power-on mode, not plain power-on.

**`0x0` means "not configured", and this is enforced:**

- Firmware refuses to transmit, answers `ERR`, OLED shows `Not Configured`
- Daemon logs `FAILED — no ACK, TV state not changed`
- Web UI shows a `No IR code` badge

Filling in real codes needs no code change — the guard simply stops firing.

The list is capped at 32 profiles (`Profiles::MAX_PROFILES`), enforced on load
and on POST.

### Button & display behaviour

Thresholds (300ms / 5s / 8s / 23s) live in `HoldTimings.h`, shared by `Button`
(which decides what a hold means) and `Display` (which draws bars spanning the
same windows).

- **Press** — released within 300ms
- **Hold** — held beyond 300ms

#### At boot

Status screen drawn as soon as display and profiles are up. With always-on
enabled it stays; with it disabled it shows for 2 seconds as a boot confirmation
then turns off.

#### Normal operation — always-on disabled (default)

| Event | Behaviour |
|---|---|
| Press 1 (display off) | Show `Profile: LG`, 2s timer, then off |
| Press 2+ (within timer) | Cycle to next visible profile, reset timer |
| IR command | Show `TV On` / `TV Off`, off after 2s |

#### Normal operation — always-on enabled

OLED on continuously showing `Profile: LG`.

| Event | Behaviour |
|---|---|
| Press 1 | Show status screen |
| Press 2+ | Cycle to next visible profile |
| IR command | Show `TV On` / `TV Off`, back to status after 2s |

#### Hold behaviour (both modes)

- **300ms** — progress bar appears, text above:
  ```
  Enter Wireless Config Mode?
  [▓▓░░░]  ← 5 blocks, 1 per second
  ```
- **Released before 5s** — status screen, next press cycles
- **5s** — bar full, text changes to `Release To Enter / Wireless Config!`
- **Released 5s–8s** — enter wireless config mode
- **Held to 8s** — factory reset screen:
  ```
  Hold To Factory Reset
  [▓▓▓░░░░░░░░░░░░░░░░░]  ← 15 segments, 1 per second
  ```
- **Released 8s–23s** — cancelled, status screen
- **Held to 23s** — factory reset triggers automatically

Whenever a hold is cancelled or completes, the display returns to the status
screen and the next press cycles profiles.

#### Wireless config mode

OLED on continuously (cannot be disabled):
```
Profile: LG
WiFi: Active
192.168.4.1
```

| Event | Behaviour |
|---|---|
| Press 1 | Show status screen |
| Press 2+ | Show `Hold Button to Exit / Wireless Mode to / Switch Profiles` for 2s |
| Hold | Same bars, but `Exit Wireless Config Mode?` and `Release To Exit` |
| Released 5s–8s | Exit config mode, return to normal operation |
| Held to 23s | Factory reset |

Also exits via the web UI's `Save and Exit`.

### Storage (LittleFS)

`/profiles.json` — array of profile objects:
```json
[
  { "name": "LG", "protocol": "NEC", "on": "0x20DF23DC", "off": "0x20DFA35C", "visible": true }
]
```

`/settings.json`:
```json
{ "active_profile": 0, "display_always_on": false }
```

Both written from hardcoded defaults on first boot; factory reset rewrites both.

**Writes are atomic and length-checked.** Serialised to `<name>.tmp`, then
renamed over the destination, and only promoted if the bytes written match
`measureJson()`. The rename makes the *replacement* atomic; it does nothing
about whether the contents are complete, and a full or failing flash returns a
partial write with a non-zero count. A stray `.tmp` after power loss is harmless
and overwritten by the next save.

### Web UI

Served from LittleFS at `192.168.4.1` when wireless config mode is active. AP is
`ESP32-IR-Remote` / `irremote123`.

**Profiles** — view, select active, add, edit, delete, show/hide from the button
cycle, `No IR code` badge on unset profiles. Deleting the *active* profile falls
back to the preceding entry and says which profile is now active.

**Settings** — display always on toggle.

**Actions** — Save · Restore · Factory Reset · Save and Exit.

**Validation** — saving is refused, with the offending profile named, if a
profile has an empty name or a code that is not hex (`0x20DF23DC` or bare
`20DF23DC`, up to 32 bits). The firmware parses an unusable code as `0x0` and
treats that as unconfigured, so a typo cannot corrupt anything — but it would
produce a profile that silently declines to transmit, and the browser is where
the user can still see what they typed.

### Firmware source map

```
firmware/
  platformio.ini
  data/index.html                (web UI, served from LittleFS in WiFi mode)
  src/
    main.cpp                     (HID setup, IR dispatch, button callbacks, WiFi lifecycle)
    Button.h / .cpp              (non-blocking press/hold detection, debounced)
    Display.h / .cpp             (OLED state machine, timer-based expiry)
    Profiles.h / .cpp            (LittleFS profile + settings storage)
    HoldTimings.h                (hold thresholds, shared by Button and Display)
```

| File | Notes |
|------|-------|
| `platformio.ini` | Platform, libraries and partition table all pinned exactly. |
| `main.cpp` | Refuses to transmit an unset code and answers `ERR`. Echoes the protocol sequence byte. |
| `Profiles.h/.cpp` | `toJson`/`fromJson` are the only places profiles cross the JSON boundary, so the on-disk format and the web API cannot drift. `getActive()` falls back to a built-in profile if storage is unavailable. |

---

## Daemon

### Power event detection (Linux)

Uses the **systemd-logind D-Bus API** — the same mechanism GNOME and KDE use.
Subscribes to `PrepareForSleep` and `PrepareForShutdown` on
`org.freedesktop.login1`.

**Inhibitor lock lifecycle:**

1. Acquired at construction, held continuously.
2. On a sleep/shutdown signal the callback runs to completion — which means the
   ESP32 has ACKed — then the lock is released and systemd proceeds.
3. Re-acquired on resume (`PrepareForSleep(false)`), ready for the next sleep.

`PrepareForShutdown(false)` — a scheduled shutdown being cancelled — is
deliberately not handled. See
[Known issues](#known-issues-and-deferred-work) for why.

**Startup ON is gated on uptime.** The service starting only implies a boot if
the machine actually just booted — `Restart=on-failure`, package upgrades and
`systemctl restart` all start the service too. The daemon reads `/proc/uptime`
and skips the ON if the system has been up longer than 180 seconds. The window
is deliberately generous: a slow machine that misses it merely fails to turn the
TV on, which the user can undo with their remote, whereas a tight window would
make that the common case.

### Cross-platform design

Two platform-specific concerns sit behind abstract interfaces:

```
TvCommand       — { bool on; const char* reason; milliseconds budget; }
IPowerMonitor   — raises OnCommand(TvCommand),
                  and OnDeviceChange where the platform reports it
ITransport      — bool send(cmd, budget = 0)
                  void invalidate()
```

`main.cpp` only ever touches these. Platform implementations are compiled in or
out by CMake.

**One callback, not three.** `OnSleep`, `OnWake` and `OnShutdown` were separate,
and the send budget came from two extra virtuals on `IPowerMonitor`. That put
the deadline on an event *class* rather than on the event, and the difference
stopped being academic the moment Windows began driving the TV from display
state — see the fifth row of
[the reversals](#five-decisions-the-sanity-check-reversed). Both the reason and
the deadline are known only to whatever raised the event, so they travel with it.

`OnDeviceChange` remains defaulted, so a platform with no device notification
implements nothing and the transport goes on discovering a vanished device by
failing a write. Linux takes that default. Linux also states a zero budget on
every command — logind holds the system for `InhibitDelayMaxSec` for sleep and
shutdown alike, and the transport's default is already sized against it.

| Concern         | Linux                      | Windows                          |
|-----------------|----------------------------|----------------------------------|
| Power events    | sdbus-c++ / systemd-logind | Win32 Service API                |
| Presence signal | *(none — sleep state only)* | console display state (ON always; OFF only where no suspend event exists) |
| Device presence | *(none — reopen on failure)* | `SERVICE_CONTROL_DEVICEEVENT`   |
| Sleep budget    | 4s, backed by a delay inhibitor | ~1.5s, and nothing can delay a suspend |
| Display / wake / arrival budget | *(n/a)*     | zero — nothing is waiting, so the transport's own default applies |
| Shutdown budget | 4s, same delay inhibitor   | 20s, against a 60s preshutdown timeout |
| HID transport   | hidapi (hidraw backend)    | hidapi (Win32 backend)           |
| Logging         | stdout → journal           | stdout redirected to a file      |
| Build system    | CMake                      | CMake                            |

hidapi is cross-platform, so `HIDTransport` is unchanged on both — the only
conditional in it is a hidapi *version* guard, not a platform one. Only
`IPowerMonitor` needs a platform-specific implementation.

Logging is the one concern still handled by a conditional in `main.cpp` rather
than behind an interface. That is deliberate sequencing rather than an
oversight — see
[Planned: abstract logging](#planned-abstract-logging-behind-an-interface).

The transport seam is also what makes the CDC path viable: swapping
`HIDTransport` for `archive/SerialTransport` in `main.cpp` is the entire change
needed to support a board that cannot present as USB HID (a classic ESP32, or an
ESP8266 behind a USB-UART bridge). That transport is one-way, so its `true`
means "bytes written", not "IR fired" — a weaker guarantee, and its header says
so.

### Windows status

**Verified on Modern Standby hardware; unverified on S3.**
`WindowsPowerMonitor.*` was rewritten on 2026-08-24 and finished on 2026-08-26,
and installed for the first time later that day on the Modern Standby laptop —
which found [four defects](#the-first-install--four-defects) and then started,
reported its power model correctly, read the display state at registration and
survived five consecutive restarts. By the end of 2026-08-27 it had also handled
sleeps, resumes, a shutdown, a Fast Startup boot, a hibernate, three Modern
Standby cycles and an unattended maintenance wake, and every row that machine
can answer had been answered. The power events it handles on an **S3** machine
remain unverified assumptions, and two rows exist to settle them.

The full picture, including which of the three power models each part covers, is
in [Windows — in progress](#windows--in-progress).

The `IPowerMonitor` boundary did its job: `ITransport`, `IPowerMonitor` and
`HIDTransport` carry no platform conditionals. But the original expectation of
"no changes outside `WindowsPowerMonitor`" was wrong, and stayed wrong — the
send budget became a parameter on `send()`, the transport gained `invalidate()`,
the monitor interface gained a device-presence callback, and the three power
callbacks eventually collapsed into one carrying a `TvCommand`. Everything
optional is defaulted so Linux is untouched, and the one change that was not
optional made the Linux implementation shorter rather than longer. That is the
shape the boundary was supposed to allow, and did.

### Daemon source map

```
daemon/
  CMakeLists.txt                 (build + install() rules + CPack config)
  VERSION                        (single source of the version)
  esp32-ir-remote.service.in     (unit template — CMake substitutes the bin path)
  99-esp32-ir-remote.rules       (udev rule — lets the daemon run unprivileged)
  src/
    main.cpp                     (wires transport + monitor, runs the event loop)
    ITransport.h                 (abstract: send(cmd, budget) -> bool)
    IPowerMonitor.h              (abstract: TvCommand + one command callback + run())
    HIDTransport.h / .cpp        (USB HID, sequence-correlated, budgeted)
    LinuxPowerMonitor.h / .cpp   (logind over D-Bus, inhibitor lock)
    WindowsPowerMonitor.h / .cpp (Win32 Service API — display state, device events)
    WindowsPowerCapabilities.h / .cpp
                                 (which of the three power models this machine is)
  packaging/
    postinst / prerm             (deb + rpm)
    windows/install-service.ps1  (service install, upgrade and removal)
    windows/verify-windows.ps1   (per-machine verification report)
    PKGBUILD / *.install         (Arch)
  archive/
    SerialTransport.h / .cpp     (USB CDC — not compiled, for non-HID boards)
```

| File | Notes |
|------|-------|
| `VERSION` | Read by `CMakeLists.txt` and `flake.nix`. `PKGBUILD` is the one copy that must be updated by hand — makepkg needs a literal before it has fetched anything. |
| `esp32-ir-remote.service.in` | Starts after `dbus.socket` and logind. Runs unprivileged as `esp32ir` with sandboxing. CMake substitutes the binary path so the unit is correct under both `/usr/local` and `/usr`. |
| `99-esp32-ir-remote.rules` | Grants the `esp32ir` group the device's hidraw node. Without it `/dev/hidraw*` is root-only and the daemon cannot run unprivileged. |
| `HIDTransport.h/.cpp` | Drains stale reports before each write; discards replies carrying another sequence. Reopens automatically after wake or replug. Single-threaded, no background polling. |

---

## Windows — in progress

**The Modern Standby machine is finished.** Installed for the first time on
2026-08-26 — a first install that found
[four defects](#the-first-install--four-defects), three of which would have
stopped anybody installing this at all, and none of which any compiler could
see. By the end of 2026-08-27 every row that machine can answer had been
answered: all eleven per-machine tests, plus 13, 14, 15 and 16, with 17 closed
as out of scope.

What is left is the **S3 machine**, and only two rows genuinely need it —
[test 11 and test 12](#resume-here). Neither is reachable on hardware with no
S3 and Fast Startup on, which is every machine tested so far.

### Where it stands

| | State |
|---|---|
| Windows build | **Works.** Builds under MSVC via vcpkg, and cross-compiles clean under `-Wall -Wextra` for x86_64-w64-mingw32, linking a complete PE. |
| Device reachable | **Verified 2026-08-24, re-verified 2026-08-26** against the reflashed firmware. `--console` opens the device, sends `ON`, receives the ACK, and the TV responds. |
| S3 suspend | **Written; the events seen, the S3 path not.** `PBT_APMSUSPEND` and `PBT_APMRESUMESUSPEND` both fired on 2026-08-26 — on a Fast Startup shutdown, not an S3 suspend — and both behaved correctly, the resume declining to assert `ON` while the display was known off. A real S3 suspend still needs the S3 machine. |
| Hibernate (S4) | **Verified 2026-08-26**, both ways: a Fast Startup shutdown (`boot type 0x1`, `away for 23s`) and a real `shutdown /h` hibernate (`boot type 0x2`, `away for 62s`). The second produced the project's first grace-period measurement and disproved the expectation that the device would re-enumerate across it. |
| Modern Standby | **Verified 2026-08-26** — three real cycles, one of 4h11m, including an unattended maintenance wake that correctly left the TV off. Display state is the only signal that fires on S0ix, and it carried the whole thing. See [what the cycles showed](#what-the-modern-standby-cycles-actually-showed). |
| Power model reporting | **New 2026-08-26.** The daemon reads `SYSTEM_POWER_CAPABILITIES` at startup and logs which of the three this machine is. |
| Device arrival/removal | **New 2026-08-26.** `RegisterDeviceNotification` on the HID interface class, filtered to this VID/PID. |
| Grace-period measurement | **New 2026-08-26, and it has produced its first number the same day: 206ms of 1500ms.** Each suspend send reports how many of its budgeted milliseconds it used. It only reports when a suspend send actually happens, which needs the suspend to arrive before the display-off `OFF` has been confirmed. |
| USB suspend defect | **Fixed in firmware, and the fix exercised 2026-08-26.** The device was gone for 4h11m across a Modern Standby cycle, re-enumerated on resume and answered — the recovery path Linux never triggers, run for real. The arrival came 12.6s after the display did, [once and unexplained](#what-the-modern-standby-cycles-actually-showed); every other reconnection in the log is between 0.4s and 3.5s. |
| Service | **Installed, started and restarted 2026-08-26** on the Modern Standby laptop, after four defects were fixed. Five consecutive restarts, each re-asserting `ON`. MSVC `/W4` reports nothing, so gcc's `-Wall -Wextra` was not hiding anything. |
| Display-state design | **Answered 2026-08-26, and the assumption held.** Registration *does* deliver the current value: the first service start logged `display state = on` and drove an `ON` from it within 2ms. The boot `ON` works as designed, and the fallback the [sanity check](#five-decisions-the-sanity-check-reversed) built for the other outcome has not been needed. |
| Display-off scoping | **Changed 2026-08-26, and the branch verified the same day.** An idle screen blank asserts OFF only on a machine with no usable suspend event; the capability report decides, and the daemon logs which branch it took. The Modern Standby laptop correctly reports `an idle screen blank turns the TV off (no usable suspend event on this machine)`. The blank itself has not been tested. |
| Per-event budgets | **Changed 2026-08-26, and the reason half verified the same day.** The budget and the reason travel with each command. Every command in the log names its own trigger — `(display off)`, `(display on)`, `(ESP32 reconnected)` — and not one is mislabelled `(sleep)`, which is the defect this replaced. The *budget* half is still unmeasured: no send has yet happened under the suspend deadline, because the display-off always gets there first. |

`WindowsPowerMonitor.*` was rewritten against the design in this section. It
compiles clean under `-Wall -Wextra` via an x86_64-w64-mingw32 cross-compile and
links as a complete PE with every import resolving — `CallNtPowerInformation`
from `POWRPROF.dll`, `RegisterDeviceNotificationW` and
`RegisterPowerSettingNotification` from `USER32.dll`,
`StartServiceCtrlDispatcherW` from `ADVAPI32.dll`. That is a syntax, type and
symbol check, and **not** evidence that any of it behaves correctly.

That distinction was worth every word it took. The clean cross-compile and the
resolving imports were both true on 2026-08-26, and the first install that day
still found [four defects](#the-first-install--four-defects) in forty minutes —
two of them in files no compiler reads. The runtime assumptions have since been
proven on Modern Standby hardware, and by running the thing, which was the only
way available.

### Three machines, not one

The 2026-08-24 plan was written around a single test machine and treated S3 as
permanently unverifiable, because that machine reports S1/S2/S3 unavailable *in
firmware*. That constraint was a property of the machine, not of the work, and
it no longer holds: there is now hardware covering all three configurations.

So the verification plan below is per-machine rather than per-feature, and the
thing that makes three days of testing on three machines add up to one record is
that each machine reports itself identically. `verify-windows.ps1` exists for
that — it reads the power model, the service configuration, the device state and
the log, and writes them in a fixed order.

**The daemon says which machine it is, in its own log.** Without that line the
rest of the log is ambiguous: an absent `PBT_APMSUSPEND` is a defect on an S3
desktop and the documented behaviour on a Modern Standby laptop, and no amount
of reading the events tells those apart.

```
[power] 14:22:31.104 machine power model: classic S3 suspend-to-RAM, hibernate available, Fast Startup on
[power] 14:22:31.104   S1=no  S2=no  S3=yes  S4=yes
[power] 14:22:31.104   hiberfil present=yes  Modern Standby (AoAc)=no
[power] 14:22:31.104   Fast Startup (Hiberboot)=yes
```

### Resume here

Ordered, because each step gates the next.

~~1. **Build on Windows** and run `--console`.~~ Done 2026-08-26 on the Modern
Standby laptop. Clean under MSVC `/W4`, and the ACK came back from the reflashed
firmware.

~~2. **Install the service.**~~ Done the same day, after
[four defects](#the-first-install--four-defects). It reports itself as Modern
Standby, puts itself under the correct display-off policy, and reads
`display state = on` at registration — which answers the design question the
boot `ON` rested on.

~~3. **The physical tests on this machine.**~~ Done 2026-08-26 and 27. **Every
row the Modern Standby laptop can answer, answers.** All eleven per-machine
tests pass, plus 13, 14, 15 and 16; test 17 was
[closed as out of scope](#what-actually-protects-playback). The largest open
risk in the brief — a full Modern Standby cycle, test 15 — passed three times,
one of them across 4h11m.

4. **The S3 machine is where the work goes next**, and only two rows genuinely
   need it. Both are rows no amount of testing here can ever reach:

   - **Test 11** — `PBT_APMSUSPEND` *driving* the OFF. The event itself has now
     fired on Modern Standby hardware, via a Fast Startup shutdown, but the
     display-off always got there first and did the work. A machine where the
     suspend arrives with no display change in front of it is the only place
     that half can be observed.
   - **Test 12** — a cold boot with Fast Startup **disabled**, the only true
     cold boot there is. Every boot measured on this laptop was `boot type
     0x1` or `0x2`: a resumed kernel session, never a fresh one.

   Both need Fast Startup turned **off** on that machine, which is a setting
   rather than a property — unlike the sleep model, which is firmware and cannot
   be changed. Elevated, before installing anything:

   ```powershell
   powercfg /h off        # disables hibernate, and Fast Startup with it
   ```

   Confirm it took by reading the daemon's own opening lines rather than the
   setting: `Fast Startup (Hiberboot)=no` in the capability report is the
   statement that matters, because that is what the daemon acted on.

   Note that `/h off` removes hibernate too, so tests 13 and 14 cannot be run in
   that state — but both already passed on the Modern Standby laptop, so the S3
   machine does not need to re-answer them. Turn it back on afterwards with
   `powercfg /h on` if that machine is somebody's daily driver.

   Everything else is the same as it was here: build, `--console`, install
   elevated, work the table, then `verify-windows.ps1`. The
   [Windows build section](#windows) has the commands.

5. **Run `verify-windows.ps1` on each machine** and keep the report. That is
   what makes three sessions on three machines into one record.

5. **The standby cycle** remains the largest single risk, and the one thing the
   firmware fix still has not been shown to survive. Three outcomes to tell
   apart, in [the risks below](#risks-to-check-on-the-real-machine).

Not on this list and still open from before any of it: the release guard on
`v*` tags, and the registered VID/PID.

An earlier version of this brief claimed Windows was "confirmed working". That
was a typo and is not true — recorded here so the claim does not resurface.

### The first install — four defects

The service had been written, reviewed, cross-compiled and sanity-checked
against its own design before anybody ran it. Installing it once found four
defects in about forty minutes. Recorded in the order they surfaced, because
each one was hiding the next: nothing below could be observed until the thing
above it was fixed.

| | Defect | Why nothing caught it |
|---|---|---|
| 1 | **`install-service.ps1` did not parse at all** under Windows PowerShell 5.1. The file was UTF-8 with no BOM; 5.1 falls back to the ANSI code page, and an em-dash's third byte (`0x94`) decodes as `"` — U+201D, which PowerShell accepts as a *string delimiter*. The string on line 171 ended early and swallowed the closing brace three lines later. Fixed by adding a UTF-8 BOM to both Windows scripts. | Nothing in this project had ever executed a `.ps1`. The brief already knew 5.1 mis-decodes UTF-8 — it says so about `Get-Content` and the log — and the same fact applied to the *scripts themselves* was not noticed. `verify-windows.ps1` had ten of the same characters and was equally unrunnable. |
| 2 | **`param([string]$Vid, [string]$Pid)`** in both selective-suspend helpers. `$PID` is a read-only automatic variable, so binding threw — *after* the service had been created and before it was started, leaving a machine with an installed service that had never run. | The script's own parameter block carries a comment explaining exactly this trap and choosing `-ProductId` to avoid it. The two helper functions below it then did the thing the comment warns against. A rule that holds at the top of a file holds inside it too. |
| 3 | **The daemon's log could not be read while the daemon ran.** `_wfreopen_s` — like every secure-CRT `_s` open — takes the file *exclusively*. Nothing else could open it at all: not `Get-Content`, not a shared-read handle, not `verify-windows.ps1`'s log tail, not the installer's own closing display. Fixed by using `_wfreopen` with the deprecation suppressed deliberately. | It is invisible to the process doing it — the daemon's own writes work perfectly. And the same call for `stderr` had been *failing since it was written*, because it could not open a file `stdout` already held exclusively. Neither return value was checked, so the daemon had been running with stderr unredirected and no way to say so. Both are checked now. |
| 4 | **A service restart was a coin toss.** The SCM reports a service `Stopped` when it reports `SERVICE_STOPPED`, while the process lives on briefly — still holding the single-instance mutex, which is released only by exiting. The replacement instance refused to start over a copy that was already leaving. Fixed with a bounded 5s wait in `claimSingleInstance`. | Genuinely intermittent: the same `Restart-Service` failed once and succeeded once within two minutes. After the fix, five consecutive restarts all passed **and all five logged the wait** — so the race was firing every time and had simply been winning more often than losing. |

**What the four have in common.** None is a logic error in the thing that was
designed so carefully; all four are in the seam between the program and the
platform, and every one of them needed the platform to find it. The compiler saw
nothing — MSVC `/W4` reported not one warning across the whole tree, so gcc's
`-Wall -Wextra` had not been hiding anything either. The cross-compile that
"links a complete PE with every import resolving" was true and proved nothing
about any of this. Two of the four were in PowerShell, which nothing compiles at
all.

Three of the four would have stopped *anybody* installing this, on any of the
three machines, before a single power event was tested. The brief's own summary
of the position — "that is a syntax, type and symbol check, and **not** evidence
that any of it behaves correctly" — turned out to be exactly right, and the
distance between the two was four defects wide.

### What the Modern Standby cycles actually showed

Three real cycles happened on 2026-08-26 in the course of ordinary use, and
Windows' own `Kernel-Power` events date them exactly: standby at 19:20:22 for two
minutes, again at 19:22:54 for **4h11m**, with a self-initiated maintenance wake
at 21:15:54 in the middle of it. This is test 15 — the brief's largest open risk
— run three times in its native habitat, and it passed. It also answered several
things that were not being asked.

The entries were user-initiated rather than idle: this laptop has `VIDEOIDLE`
and `STANDBYIDLE` both set to 0, so it never blanks or sleeps on its own. That
makes these test 4 rather than test 3b, and it leaves 3b and 17 unanswerable on
this machine until a screen timeout is configured — worth noticing, because a
machine that never blanks cannot exercise the one policy this platform's
display-off decision was written for.

**The ESP32 disappears across Modern Standby, and disabling selective suspend
does not prevent it.** The device was removed 3 seconds after the display went
off and did not return until the machine did:

```
[power] 19:22:54.721 display state = off
[cmd] OFF sent and ACK received (display off)
[power] 19:22:57.710 ESP32 removed
[power] 23:34:08.044 display state = on
[transport] ESP32 not found — skipping IR command: ON
[cmd] ON FAILED — no ACK, TV state not changed (display on)
[power] 23:34:20.673 ESP32 arrived
[event] ESP32 reconnected — re-asserting on
[cmd] ON sent and ACK received (ESP32 reconnected)
```

The per-device selective-suspend values were already in effect for that cycle —
written at install and picked up by the re-enumeration at 19:22:31. They changed
nothing, and could not have: this is the platform powering the controller down
on the way into DRIPS, not a device idling into selective suspend. The installer
is doing something worth doing for the *idle* case and nothing at all for this
one, which is a sharper statement of "defence in depth rather than a fix" than
the brief had.

**The device-arrival path is not a nicety; it is the only thing that worked.**
The wake `ON` failed — correctly, honestly, and with the TV left alone rather
than recorded as on — because the device was still 12.6 seconds from coming
back. Every later correction depended on the arrival notification added on
2026-08-26 for exactly this reason. Without it the TV would have stayed off
until the next display transition, which is what the brief predicted and is now
the observed behaviour of the machine rather than a prediction about it.

**A 12.6-second gap appeared once, and it is worth much less than it first
looked.** On the resume from the 4h11m standby, `display state = on` and
`ESP32 arrived` are 12.6s apart, and the `ON` in between failed. That was
written up here as the platform's largest user-visible defect. It is not, and
the rest of the log says why — every other transition is fast:

| Event | Gap |
|---|---|
| Bus cycle on the short standby resume | 0.4s |
| Physical replug, three separate times | 2.7s, 3.4s, 3.5s |
| Hibernate resume | no removal reported at all |
| **The 4h11m standby resume** | **12.6s, once, never reproduced** |

**And its cause was never established.** 12.6s matches neither the 0.4s bus
cycle nor the ~3s a physical replug takes in this same log, and nothing in the
daemon's record distinguishes "the USB stack took that long to restore after a
long DRIPS" from "somebody plugged the device in at that moment". Nobody was
watching the TV for it — it happened as the operator returned to a machine that
had been asleep for four hours, three minutes before a replug they *did* watch,
and their consistent report across every other transition all evening was that
it acts near-instantly.

Recorded as one unexplained sample rather than a finding. Worth resolving only
if it recurs, and the way to resolve it is to resume from a long standby without
touching the device.

The first explanation offered for it — that the gap scales with how deeply the
machine slept — was falsified the same night regardless, by the hibernate test
cutting *more* power and reconnecting with no removal at all. See
[the hibernate](#the-hibernate--and-the-two-predictions-it-falsified).

**No suspend event fires on the standby path.** Across four hours of standby the
log contains no `PBT_APMSUSPEND`, no away-time report and no grace-period
measurement — the display-state signal carried all of it alone, which is what it
was made the primary trigger for.

That looked at first like three finished features being dead on this machine
class. **It is not, and the shutdown test the same night proved it**: Fast
Startup turns a shutdown into a hibernate, that *does* raise `PBT_APMSUSPEND`,
and away-time reported `away for 23s` across it. So the suspend branch is live
here — just on the shutdown and boot path rather than the standby one. See
[the Fast Startup shutdown](#the-fast-startup-shutdown--the-suspend-path-does-run-here).

The point that survives is narrower and still worth keeping: on a Modern Standby
machine, the *sleep* a user actually performs several times a day produces no
suspend event at all, so anything hung off that event is invisible for exactly
the transitions that happen most.

**A staleness guard fired, unprompted and correctly.** On the short cycle the
arrival re-assert and the display coming on overlapped:

```
[power] 19:22:31.980 ESP32 arrived
[event] ESP32 reconnected — re-asserting off
[power] 19:22:32.007 display state = on
[cmd] OFF sent and ACK received (ESP32 reconnected)
[power] 19:22:32.133 power state changed mid-command — result discarded as stale
[cmd] ON sent and ACK received (display on)
```

27ms apart. The re-asserted `OFF` was in flight when the display came back; its
ACK arrived after the world had changed, was discarded rather than recorded as
the TV's state, and the `ON` went out behind it. That is the three-way tracking
— what the screen said, what the daemon concluded, what the TV confirmed —
doing the job it was separated into three for, on a race nobody had constructed.

### The Fast Startup shutdown — the suspend path does run here

Tests 5 and 13, run 2026-08-26 at 23:46. Windows logs `boot type 0x1`, which is
Fast Startup, so this was a shutdown that hibernated the kernel session and a
boot that resumed it — not a cold boot. Test 12 remains the only true one, and
it still belongs to a machine with Fast Startup turned off.

The whole cycle, twenty-four seconds end to end:

```
[power] 23:46:24.568 display state = off
[cmd] OFF sent and ACK received (display off)
[power] 23:46:25.181 PBT_APMSUSPEND (sleep or hibernate)
[power] 23:46:25.182 TV already off — no command sent (inside the suspend grace period)
[power] 23:46:49.072 away for 23s
[power] 23:46:49.072 PBT_APMRESUMESUSPEND while display is known off — not asserting on
[power] 23:46:49.133 display state = on
[cmd] ON sent and ACK received (display on)
[power] 23:46:49.588 PBT_APMRESUMEAUTOMATIC (resume, presence unknown) — not acted on
```

Seven things are being proven there, and only two of them were the test:

| | What it shows |
|---|---|
| **The TV was off before the machine went** | The `OFF` was sent *and ACKed* 613ms before `PBT_APMSUSPEND` arrived. Nothing was racing a two-second grace period. |
| **`PBT_APMSUSPEND` fires on this machine after all** | It is a Modern Standby laptop that never raises it for standby, and raises it for a Fast Startup shutdown, because that shutdown *is* a hibernate. Away-time and the suspend branch are live here, on this path only. |
| **The grace period was never used** | The display went off first, so by the time the suspend arrived the TV was already off and the send was suppressed — correctly, and *only* because that suppression is scoped to the grace period. On this machine the 1500ms budget may never actually be spent, which means it also cannot be measured here. |
| **The boot `ON` came from display state, not from starting** | There is no service start in the log at all. Fast Startup resumed the same process, so nothing "began" — and the `ON` still happened, because it is driven by the display coming on rather than by the daemon booting. This is exactly the case [the uptime gate could never have handled](#what-each-machine-can-and-cannot-prove), and it is now observed rather than argued. |
| **`PBT_APMRESUMESUSPEND` correctly declined to assert `ON`** | It fired 61ms before the display came on, and refused, because the display was still known off. Had it asserted, it would have been right by luck here and wrong on every unattended resume. |
| **`PBT_APMRESUMEAUTOMATIC` was named and ignored** | The invariant that a machine waking itself is not somebody walking into the room, holding on the one path where the two arrive half a second apart. |
| **Two resume events and a display change produced exactly one command** | Three plausible triggers, one `ON`. |

The order matters more than any single line: display-off, then suspend, then
resume, then display-on. On this platform the display signal brackets the power
events on both sides, which is why it can be the authority and the power events
can be advisory. On an S3 machine with Fast Startup off, that order will not
hold — and that is the reason the other two machines are still worth testing.

### The hibernate — and the two predictions it falsified

Test 14, run 2026-08-26 at 23:54 with `shutdown /h`. Windows logs `boot type
0x2`, which is a genuine resume from hibernate rather than the Fast Startup
`0x1` of the test before it. It passed — and it disproved two written
expectations, one from the brief and one written a few hours earlier in this
same section.

```
23:54:31.153 display state = off
[event] Display off
23:54:31.194 PBT_APMSUSPEND (sleep or hibernate)
[cmd] OFF sent and ACK received (display off)
23:54:31.265 power state changed mid-command — result discarded as stale
[event] Going to sleep
[cmd] OFF sent and ACK received (sleep)
23:54:31.390 suspend send took 206ms of the 1500ms budget
23:55:34.111 away for 62s
23:55:34.111 PBT_APMRESUMESUSPEND while display is known off — not asserting on
23:55:34.140 PBT_APMRESUMEAUTOMATIC (resume, presence unknown) — not acted on
23:55:34.185 display state = on
[cmd] ON sent and ACK received (display on)
```

**The suspend grace period finally has a number: 206ms of the 1500ms budget.**
This is one of the two things the brief listed as measurements rather than
pass/fail, and the first time any machine has produced one. It was measurable
here for a reason worth keeping: the display-off and the suspend arrived **41ms
apart**, so the display-off `OFF` was still in flight when the suspend fired and
the suspend send was *not* suppressed. On the Fast Startup shutdown they were
613ms apart, the TV was already confirmed off, and the send was skipped. The
grace period is therefore measurable only when the two events collide — which is
also the only time it matters.

**Prediction one, falsified: the ESP32 did not re-enumerate.** The brief said a
hibernate resume "is the cheapest real test of the device-arrival path", on the
reasoning that the device *will* have re-enumerated after full power loss. There
is no `ESP32 removed` and no `ESP32 arrived` anywhere in the cycle. The handle
survived, and the `ON` went out and was ACKed on the first attempt. The
device-arrival path was not exercised at all, so test 14 passed *without*
testing the thing it was chosen to test.

**Prediction two, falsified: recovery was faster, not slower.** Written a few
hours earlier here was the expectation that hibernate would be slower than the
12.6s Modern Standby recovery, because it cuts power more completely. It was
instantaneous. Deeper sleep, less disruption — the opposite of the stated
reasoning, so the reasoning was wrong rather than imprecise.

The likely mechanism, offered as inference and not as evidence: S4 leaves the
USB ports on standby power, so the ESP32 never lost power, never restarted and
never re-enumerated — the machine came back and found the device exactly where
it left it. Modern Standby, despite being the lighter state, *deliberately*
powers the controller down as part of entering DRIPS. That would make the
device's disappearance a thing the OS does on purpose in one state and does not
do in the other, which fits every observation but has not been confirmed against
anything but the daemon's own log.

**Both resume events fired before the display change this time**, in the
opposite order to the Fast Startup boot, and both declined to assert `ON` on the
same grounds. The ordering of the three signals has now been seen both ways
round, and the outcome did not depend on which arrived first — which is the
property that makes display state the authority rather than a tiebreak.

### The screen blank is the standby entry

Test 3b, 2026-08-27, with `VIDEOIDLE` set to 5 minutes and `STANDBYIDLE`
deliberately left at 0 — never — so that a screen blank could be observed
without a sleep confusing it. That was the plan. It is not what the machine
does:

```
[power] 00:10:21.954 display state = dimmed (treated as on)
[cmd] ON sent and ACK received (display on)
[power] 00:10:36.135 display state = off
[cmd] OFF sent and ACK received (display off)
[power] 00:11:02.488 display state = on
[cmd] ON sent and ACK received (display on)
```

Windows' own events put `The system is entering Modern Standby` at **00:10:36**,
the same second as the blank, with "sleep after" set to never. **On a Modern
Standby machine the screen blanking and the standby entry are one event, and no
setting separates them.** "Never sleep" governs nothing here; the display
timeout is the sleep timeout under another name.

Two consequences, and the first is a decision getting stronger rather than
weaker:

- **The display-off policy is better justified than the argument that produced
  it.** It was reversed on 2026-08-26 out of a worry that an idle blank would
  turn the TV off "under somebody still watching", and confined to machines with
  no usable suspend event as the lesser evil. On this machine the blank *is* the
  machine going to sleep. Asserting `OFF` is not a compromise standing in for a
  suspend signal — it is the correct response to a suspend that Windows declines
  to announce any other way.
- **Test 3b and test 4 cannot be told apart here**, which is why the row now
  carries the qualification rather than a bare yes.

**A dim raises a redundant `ON`.** Fifteen seconds before the blank, Windows
dims the display, the daemon correctly treats dimmed as on — and sends `ON` to a
TV that was already on, fourteen seconds before telling it to switch off. This
is the [repeat rule](#invariants) working exactly as written: outside the
suspend grace period a repeat is always sent, because discrete codes exist to
repair drift and skipping the send discards that repair.

It is still the first observed case where the rule fires with no drift possible
and is contradicted moments later, so it is recorded rather than fixed. The cost
is one redundant IR burst per idle cycle and nothing else, the benefit is a
genuinely idempotent design, and narrowing the rule to "changes only" would
reintroduce the failure the reversal removed. Worth revisiting only if the same
pattern shows up somewhere it costs more than a wasted frame.

### What actually protects playback

Test 17's first half, 2026-08-27: a fullscreen browser video left running for
**35 minutes** against a 5-minute display timeout. The screen never dimmed, the
TV stayed on, and the daemon logged **nothing at all** — its last line is from
before the video started. That is the pass, and the mechanism behind it was
captured while it was still running rather than inferred afterwards:

```
DISPLAY:
[PROCESS] \Device\HarddiskVolume3\Program Files\Mozilla Firefox\firefox.exe
display request
```

**The daemon has no part in this and cannot.** It does not know a video is
playing, has no concept of an application, and would switch the TV off if the
screen blanked. What prevented that was Firefox asserting a `DISPLAY` power
request and Windows honouring it, which is the entire protection the display-off
policy leans on.

That is worth stating plainly because it relocates the risk. The policy is
sound *conditional on applications declaring themselves*, and that condition is
owned by third-party software rather than by anything in this repository. A
media application that declares its request correctly is protected completely; a
game or a player that does not is not protected at all, and the daemon cannot
tell the two apart — from where it sits, an undeclared video and an idle desktop
are the same screen going dark.

**And then the game half disproved the framing above.** Hollow Knight, played on
a controller for 14 minutes against the same 5-minute timeout, held **no
`DISPLAY` request at any point** — 45 samples, the game focused for 43 of them,
`None.` in every one. The screen never blanked, and the daemon logged nothing.

```
01:02:39  hollow_knight     None.
   ... 43 samples, all None, game focused throughout ...
01:16:41  hollow_knight     None.
=== daemon log lines written during the sample ===
(none - the daemon logged nothing at all)
```

So the neat rule — declared apps are protected, undeclared apps are not — is
wrong. Something kept the display alive for twelve minutes past the last
keyboard or mouse input **without asserting anything Windows will report**.
Which of these it is has not been established:

- gamepad input feeding the idle timer after all, in which case a player who
  puts the controller down for five minutes during a cutscene still loses the
  TV; or
- fullscreen-exclusive presentation suppressing the timeout, in which case the
  game is protected the whole time it is running, controller or not.

The game was in **borderless windowed** rather than exclusive fullscreen, which
rules the second explanation out — a borderless window is a window, and Windows
does not suspend the display timeout for one. That leaves gamepad input feeding
the idle timer as the likely mechanism, and it was never confirmed, because the
question stopped mattering.

**Test 17 is closed as out of scope, and the row was wrong to exist.** The
daemon mirrors whatever Windows does with the display; it does not influence it
and must not. So if a game lets the display time out, the TV switches off — and
a monitor sitting in the same place goes dark at the same instant. That is not
the device misbehaving, it is the device doing the only thing it does. The row
was written on the premise that the display-off policy might black out a TV
somebody was watching, and that premise does not survive the observation that
**when the display is off, the TV is showing nothing anyway**. There is nobody
to protect.

What survives is not a defect but an asymmetry in the cost of recovery: a
blanked monitor returns in about a second of moving a mouse, while the TV needs
an IR `ON`, a device that took [12.6 seconds](#what-the-modern-standby-cycles-actually-showed)
to return after a long standby, and then the TV's own power-on. Same event,
much larger bill. That belongs to recovery latency and not to this policy.

One observation is worth keeping from the two runs: `powercfg /requests` is
**not** a reliable predictor of whether an application will keep the display
alive. Firefox declared a `DISPLAY` request and was protected; Hollow Knight
declared nothing and was equally protected. Anyone tempted to build a
warning — or a policy — on the presence of that request should know it reports
one of several mechanisms and not the outcome.

### Where the implementation departs from the plan

Seven things came out differently once the code was written. Recorded because a
plan that silently stopped matching its implementation is worse than no plan.

| | What changed, and why |
|---|---|
| **Service detection** | The plan chose `StartServiceCtrlDispatcherW` failing with `ERROR_FAILED_SERVICE_CONTROLLER_CONNECT` as the test for "running as a service". It is the standard test and it cannot be used here: the redirect has to be decided before the first log line, and that call blocks until the service stops. A `--console` flag decides it instead; the dispatcher failure is still handled, to print guidance rather than exit silently. |
| **What `--console` does** | Not a foreground daemon — power events reach services only, so there would be nothing to listen for. It sends one `ON` and reports the ACK, which makes it a device-reachability check that needs nothing installed. The first useful thing to run after a build. |
| **Callbacks return `bool`** | `IPowerMonitor`'s callbacks were `void`. Suppressing repeat commands needs to know whether the device confirmed: recording an attempt that was never ACKed as the TV's new state would suppress the retry and leave it wrong indefinitely — the "reports success regardless" failure in a new place. Linux ignores the answer, having no such state. |
| **One callback, carrying `TvCommand`** | The three callbacks became one. Windows raises commands for six distinct triggers, only two of which are a sleep or a shutdown, and routing the other four through `OnSleep`/`OnWake` gave them the wrong deadline and the wrong name in the log. The interface lost two setters and two virtuals in the process, and `LinuxPowerMonitor` got shorter. |
| **Preshutdown timeout** | Set by the installer as the `PreshutdownTimeout` registry value, which is what `ChangeServiceConfig2` writes anyway, rather than by the service to itself at startup. It is install-time configuration and belongs with the rest of it. |
| **`GUID_CONSOLE_DISPLAY_STATE`** | Spelled out in the source rather than taken from the SDK, which declares the power-setting GUIDs but leaves their definitions in a library whose name varies by SDK version. One line, and one fewer link-time question. |
| **A third conditional in `main.cpp`** | The startup `ON` block is now inside `#ifdef __linux__`, against the stated end-state of exactly one. The honest alternative was a `justBooted()` on `IPowerMonitor` that Windows answers "never" — machinery to avoid a two-line guard. Noted rather than hidden. |

### Completing the three power models — 2026-08-26

The 2026-08-24 code already covered all three configurations behaviourally: one
display-state mechanism, with the suspend and resume events as secondary
triggers. What it could not do was tell anyone *which* configuration had
produced a given log, or notice the device it talks to coming and going. Both
matter now that the testing spans three machines.

The capability report added here for the first reason has since acquired a
second: it decides whether an idle screen blank turns the TV off on this
machine. See [the two halves of the display
signal](#the-two-halves-of-the-display-signal-are-not-equally-safe).

| Added | Why |
|---|---|
| **Power capability report at startup** | `CallNtPowerInformation(SystemPowerCapabilities)` read once and logged. Without it a log is ambiguous: an absent `PBT_APMSUSPEND` is a defect on an S3 desktop and correct on a Modern Standby laptop. |
| **Device arrival and removal** | `RegisterDeviceNotification` on the HID interface class, filtered to this VID/PID. A removal invalidates the transport's handle deliberately; an arrival re-asserts whatever state the daemon last decided the TV should be in. Deliberately *not* the last display state: on a machine where an idle blank is ignored the two disagree, and re-asserting the display would switch the TV off on a replug for a reason the policy had already rejected. Three things are tracked separately for that reason — what the screen said, what the daemon concluded, and what the TV confirmed. |
| **`PBT_APMRESUMECRITICAL`** | Resume after an unannounced power loss — an emergency hibernate on a dying battery. Deprecated and still delivered, so it is named rather than logged as an unknown event number. Treated like an automatic resume: not evidence anybody is present. |
| **Grace-period measurement** | Every suspend send reports how many of its budgeted milliseconds it used. The ~2s is guidance, not a contract, and this is the only way to learn what a given machine actually allows. |
| **Away-time reporting** | Wall-clock elapsed across a suspend, so a brief Modern Standby dip and an overnight hibernate stop looking identical in the log. |
| **Idempotent installer** | Re-running it is an upgrade: the running service is stopped and deleted, with a poll for the deletion to land, before the new one is created. An installer that only works on a clean machine stops working on the second attempt, which is exactly when it is needed. |
| **`verify-windows.ps1`** | Captures power model, service configuration, device state, selective-suspend values and the log tail into one report. What makes three sessions on three machines into one record. |

**Why the device notifications are worth the code.** They landed on what was
then the brief's largest open risk, and they are the reason it closed. The
firmware re-enumerates itself after a USB suspend, so a wake `ON` issued while
the device is still coming back fails — and the arrival is what retries it.
Observed doing exactly that on 2026-08-26: an `ON` failed with no device
present, and the arrival re-assert put the TV right. Without it the TV stays
wrong until the next power event, which could be hours. The removal half is
smaller but not nothing: it makes the log say *why* the handle was dropped, on
the one path where a device vanishing and returning is routine.

Both are routed through the worker rather than acted on in the handler.
`HIDTransport` is single-threaded by design and holds no lock; the handler runs
on an SCM thread. Touching the transport from there would break the invariant
the worker exists to maintain — so the handler records and returns, exactly as
it does for power events.

**`SYSTEM_POWER_CAPABILITIES` is declared in full rather than included.** The
two fields that matter most — `AoAc`, which is how a machine says it is Modern
Standby, and `Hiberboot`, which is Fast Startup — were appended by Microsoft
into bytes the older struct reserved as spare. A current SDK declares them;
mingw-w64's `winnt.h` does not. Depending on the header would mean the capability
report existing or not according to which toolchain compiled it, and going
missing precisely on the cross-build nobody watches. The layout is documented
and stable, so it is written out once with a `static_assert` guarding against a
future SDK growing past it.

### Five decisions the sanity check reversed

Reviewing the settled decisions against each other — rather than against the
code — found places where a sound principle had been applied past where it held.
All were changed on 2026-08-26, and the reasoning is recorded where each
decision lives rather than only here.

The first three came from reading the decisions against one another. The last
two came from a second pass reading them against the
[Overview](#overview) — asking not "is this decision consistent?" but "does this
still deliver the four rows in the table this device exists for?" That is a
different question and it found different things.

| Was | Now | Why it was wrong |
|---|---|---|
| No display state at registration → **assert ON** | Assert nothing; drive from the first display-state change | "Wrong only for a restart while the display is off" *is* the 3am case the design exists to prevent, and restart-on-failure makes that a configured behaviour. See [driving from display state](#driving-from-display-state). |
| Skip any send that repeats the last asserted state | Skip only inside the suspend grace period | The optimisation was justified by the two-second window and then applied everywhere, including paths under no time pressure — discarding the drift repair that discrete IR codes exist to provide. |
| `send()` budget may only tighten the default | Honoured in both directions | It made the Linux-sized default a ceiling for every platform, capping the Windows shutdown at 4s of an available 60s — on the one send with nothing after it to correct a TV left on. |
| Display-off turns the TV off on **every** Windows machine | Only where no usable suspend event exists — see [the two halves of the display signal](#the-two-halves-of-the-display-signal-are-not-equally-safe) | One mechanism covering all three configurations was treated as the design's virtue. It is a virtue for the ON and a defect for the OFF: it took a behaviour a Modern Standby laptop forces — the TV going off under somebody still watching — and applied it to an S3 desktop that had a better signal available and needed nothing. |
| Sleep and shutdown budgets supplied by `IPowerMonitor::sleepBudget()` / `shutdownBudget()` | The budget and the reason travel with the command, as `TvCommand` | The budget was attached to an event *class* rather than to the event. Once Windows drove the TV from display state, a screen blanking was routed through the sleep callback and inherited a suspend's 1500ms — on a path where nothing was waiting for the daemon at all, and which is among the likeliest to find the ESP32 mid-re-enumeration. It logged itself as `(sleep)` too. |

The pattern in all five is the same, and it is the one the
[History](#history) section already names: a fix that closed the case in front
of it without closing the class it belonged to. Each was a correct decision
about one path, generalised one step too far.

The fifth is worth separating from the other four, because it is not only a
wrong decision but a *shape* that produces them. Three callbacks that each
implied their own deadline meant the deadline could not be stated per event;
anything routed through the sleep callback silently became a sleep. Collapsing
them to one callback carrying `{on, reason, budget}` removes the category, which
is the difference between fixing the instance and closing the class.

**Event Log is still deliberately not done.** It is the idiomatic Windows
answer and it stays on the list rather than in the code, for the reason already
recorded under [Logging](#logging--decided): it needs a registered event source,
a message resource DLL, and a logging abstraction spanning both platforms, and
its benefits accrue to administrators managing machines they did not build.
That is still not the situation. Get the three power models verified first.

### What needs no work at all

The transport layer is genuinely shared. `ITransport`, `IPowerMonitor` and
`HIDTransport` contain **zero** platform conditionals, hidapi covers the Win32
HID backend, and `CMakeLists.txt` already resolves it through vcpkg. That was
the point of the abstraction and it held.

The firmware needs nothing either. Its report descriptor declares a
vendor-defined usage page (`0xFF00`), so Windows binds `hidclass` automatically:
no driver to write, no INF to sign, and none of the access restrictions Windows
places on keyboard and mouse top-level collections.

### The architectural difference — accepted, not solved

Windows has no equivalent of a logind delay inhibitor for **sleep**. The ability
to veto a suspend (`PBT_APMQUERYSUSPEND`) was removed after XP.
`PBT_APMSUSPEND` is a *notification*: the machine suspends roughly two seconds
later whether anything has finished or not. That figure is Microsoft's guidance,
not a contract — measure it on real hardware before trusting it.

| Event | Linux | Windows |
|---|---|---|
| Sleep | delay inhibitor, 5s guaranteed | ~2s grace, **best-effort** |
| Wake | no guarantee needed — machine is up | same |
| Shutdown | delay inhibitor, 5s | `SERVICE_ACCEPT_PRESHUTDOWN`, minutes — **better than Linux** |
| Boot | no guarantee needed | same |

Only sleep degrades, and only from "guaranteed" to "almost always". The measured
IR round trip is well under a second, so the grace period is ample in the healthy
case. This is a documented concession, not a bug to fix.

The ACK still matters on Windows sleep even though nothing can be done with it:
it is the only evidence the IR actually fired, on the platform with no console.
Dropping to fire-and-forget would reintroduce exactly the "reports success
regardless" failure that fixes 1, 12, 13 and 21 all addressed.

### Windows is three configurations, not one

Windows varies along two independent axes, and the combinations are not edge
cases. This plan originally targeted the rarest of them.

**Boot.** Fast Startup — on by default wherever hibernation is enabled — makes
"shut down" a hibernation of the kernel session rather than a true shutdown. The
tick count does not reset across it. That is why Task Manager reports multi-day
uptime on a machine that gets shut down every night.

**Sleep.** A machine supports either classic S3 suspend or Modern Standby (S0
low power idle). It is a property of the platform firmware: never both, and not
switchable.

| Configuration | Boot ON | Sleep OFF |
|---|---|---|
| S3, Fast Startup off | uptime gate works | `PBT_APMSUSPEND`, ~2s grace |
| S3, Fast Startup on | **uptime gate never fires** | `PBT_APMSUSPEND`, ~2s grace |
| Modern Standby | **uptime gate never fires** | no classic suspend — `PBT_APMSUSPEND` may not arrive usefully at all |

Porting `systemJustBooted()` with `GetTickCount64()` is the obvious translation
and it is correct only in the first row. In the other two the machine powers on
carrying a tick count of hours, the gate skips, and the daemon logs `ON skipped
— service restarted on an already-running system, not a boot`. That is worse
than a missed command: it is a wrong reason recorded in the log, for the one
feature the device exists to provide.

It compounds. On a Fast Startup power-on the kernel resume completes before the
SCM starts services, so the daemon registers its control handler too late to
catch a resume broadcast either. Both routes to "TV on when the PC turns on"
close at the same time.

Modern Standby is the other half. Machines using S0 low-power idle never enter
S3, the power-event model differs, and the plan had a *check* for it
(`powercfg /a`) but no design for the branch where the check comes back S0.
A product cannot ship a sleep path that only works on one of two power models.

### Driving from display state

`RegisterPowerSettingNotification` on `GUID_CONSOLE_DISPLAY_STATE`, with
`DEVICE_NOTIFY_SERVICE_HANDLE`. Notifications arrive through the service control
handler that already exists, as `SERVICE_CONTROL_POWEREVENT` carrying
`dwEventType = PBT_POWERSETTINGCHANGE`.

What each source can answer, before any decision about which to use:

| Question | Suspend/resume events | Display state |
|---|---|---|
| Boot ON under Fast Startup | never fires | registration delivers the current value — display on at boot → ON |
| Unattended 3am wake | `PBT_APMRESUMEAUTOMATIC` turns the TV on | display stays off on a maintenance wake → nothing fires |
| Modern Standby sleep | no design | this *is* the documented S0ix signal |
| S3 sleep | `PBT_APMSUSPEND`, works | *could* work — the display goes off ahead of the suspend — but it also fires on an idle blank that is not a suspend at all, which is why it is not used here |

Only the last row is a choice rather than a constraint, and it is the one the
subsection below is about.

**Why being liberal about triggers is safe.** The IR codes are discrete, not
toggle — see [Manufacturer profiles](#manufacturer-profiles), where that choice
was made so the TV reaches the right state regardless of prior drift. The
consequence here is that re-asserting a state the TV is already in is a no-op at
the TV. So display state and `PBT_APMSUSPEND` can both be handled without either
having to be exactly right; whichever arrives first wins and the other is
absorbed.

#### The two halves of the display signal are not equally safe

This section originally read "one mechanism covers all three configurations",
and treated that as the design's chief virtue. It is a virtue for the ON and a
defect for the OFF, and collapsing them cost something real.

**Display on → ON, on every machine.** The screen lighting up answers "is
anyone there?" as directly as any Windows API does, and the failure mode if it
is wrong is a TV switched on that the user turns off with their own remote. It
is also the only route to an ON at boot, because the uptime gate does not
survive Fast Startup. Nothing about this is machine-specific.

**Display off → OFF, only where nothing else can.** Here the failure mode is the
opposite shape and much worse: the TV switches off under somebody who is still
watching. The mitigation is an assumption about other people's software —
players are supposed to assert `ES_DISPLAY_REQUIRED` and the good ones do, but
browsers and games are less consistent, and a game played on a controller
generates no keyboard or mouse input at all. A PC connected to a television is
this device's whole use case, so that failure lands exactly where it hurts most.

On a **Modern Standby** machine that cost has to be paid: there is no classic
suspend, `PBT_APMSUSPEND` may never arrive usefully, and a TV that never
switches off fails one of the four rows in the [Overview](#overview) table. On a
machine that reports **classic S3**, it does not: `PBT_APMSUSPEND` arrives, says
what it means, and is the honest cause. Taking the behaviour a Modern Standby
laptop forces and applying it to an S3 desktop buys nothing and pays the full
price.

So the policy is per machine, decided from the capability report the daemon
already reads at startup:

| Machine reports | Display off | What drives the OFF |
|---|---|---|
| S3, not Modern Standby | ignored | `PBT_APMSUSPEND` |
| Modern Standby (`AoAc`) | **asserts OFF** | display state — the only signal there is |
| Neither, or capabilities unreadable | **asserts OFF** | display state, because no suspend event can be relied on |

The third row is the permissive one and is deliberate. "Unknown" means nothing
can be assumed about whether a suspend event will ever arrive, and between a TV
that occasionally switches off early and a TV that never switches off at all,
the first is the annoyance and the second is a broken product.

`displayOffDrivesOff()` in `WindowsPowerMonitor.cpp` is that table, and the
daemon logs which branch it took on the line after the capability report — so a
missing OFF on a screen blank reads as the documented behaviour rather than as a
defect, without anybody having to remember which machine the log came from.

**What this changes about the capability report.** It was added on 2026-08-26 as
pure diagnostics — a line so a log could not be filed against the wrong
configuration. It is now load-bearing: `AoAc` decides a behaviour. That is worth
stating because it changes what a wrong reading costs. A machine that
misreports, or one where `CallNtPowerInformation` fails, no longer merely
produces a confusing log; it runs the wrong policy. The failure is bounded in
the direction that matters — the fallback is the permissive branch, which errs
towards the device doing its job — but it is no longer free.

**This is the same mistake the [reversals](#five-decisions-the-sanity-check-reversed)
section is about**, committed one more time in the section that records them: a
decision that was correct for one machine, generalised to every machine because
the mechanism happened to be available everywhere. It is listed as the fourth
reversal there.

**Suppressing the duplicate is scoped to the suspend path, and only there.**
The last asserted state is tracked, but a send that would repeat it is skipped
*only* inside a suspend — the one place under time pressure, where two IR
transmissions would not fit comfortably in the two-second grace period.

Everywhere else the repeat is sent. This was originally applied everywhere, and
that was wrong for a reason worth stating: it argues directly against the
sentence above it. Discrete codes were chosen so the TV reaches the right state
*regardless of prior drift*, and drift is exactly what happens when somebody
picks up the TV's own remote. Re-asserting a state the TV is already in is a
no-op; re-asserting one it has drifted out of is the repair. Skipping the send
outside a suspend throws the repair away to save time nothing is asking for.

The same reasoning governs what the daemon is entitled to remember. A device
removal resets the last asserted state to unknown, because while the ESP32 is
unplugged there is no channel to the TV at all — and somebody unplugging it is
a fairly good indicator that they are about to use the TV another way.

**What disappears.** `systemJustBooted()` never gains a Windows branch. The
question "was this a boot or a `sc restart`?" exists on Linux only because a
restart would assert ON with nobody at the machine — see the comment at the top
of `systemJustBooted()`. Display state answers presence directly, so the
question dissolves rather than needing an answer harder than `/proc/uptime`.

**Why not resume events alone.** `PBT_APMRESUMEAUTOMATIC` is documented as
firing on every resume *and* as explicitly not implying the user is present:
wake timers, maintenance windows and update-driven wakes all raise it. Handling
it alone therefore reintroduces, by a different route, the exact defect the
uptime gate was written to remove — a daemon turning the TV on at 3am with
nobody there. Handling only `PBT_APMRESUMESUSPEND`, as the code does today,
fails the opposite way and leaves the TV off for Wake-on-LAN. The two events are
a *presence discriminator*; choosing one throws away the information. They also
cannot be collapsed at the point of arrival, because AUTOMATIC comes first and
whether RESUMESUSPEND follows is not yet known.

**The behaviour this changes, and how much of a choice it really was.**
On a Modern Standby machine, display-off on an idle timeout turns the TV off
while the user is still sitting there. That should be recorded honestly as a
decision the API made rather than one the product did: Modern Standby exposes no
usable suspend event, so display state is not the best available signal on that
machine, it is the only one. The product behaviour was chosen to fit the
mechanism, and scoping it to that machine is as far as the constraint can be
pushed back.

Two things remain worth being straight about, both now narrower than they were:

- **Windows and Linux are still different products, on one class of machine.**
  Linux drives from sleep state only and never watches the display. An S3
  Windows machine now matches it. A Modern Standby laptop does not, and cannot
  without an idle signal Windows does not offer to a session-0 service in any
  other form. That residue is forced rather than chosen, which makes it
  something to document for users rather than a decision still owed. The
  alternative — teaching Linux the same trick via a session idle or DPMS signal
  — would spread a behaviour nobody wants to a platform that has no need of it,
  and is not planned.
- **The mitigation is an assumption about other people's software.** Media
  players are supposed to assert `ES_DISPLAY_REQUIRED` so the screen does not
  blank during playback, and the good ones do. Browsers and games are less
  consistent, and a game played on a controller produces no keyboard or mouse
  input at all. The failure is loud: the TV switches off part-way through
  whatever is being watched. Worth testing deliberately with a browser-based
  player and with a controller game on the Modern Standby machine — it is the
  most likely source of a "this thing is broken" report from an actual user, and
  it is now the *only* configuration where it can happen.

**Two things to confirm before committing to this**, since the boot ON depends
entirely on the first:

- that registration delivers the current display state immediately, rather than
  only on the next change;
- that `GUID_CONSOLE_DISPLAY_STATE` reaches a session-0 service as documented —
  `GUID_SESSION_DISPLAY_STATUS` is the per-session equivalent for user
  applications, and the two are easy to confuse.

**If the first turns out to be false, the fallback asserts nothing.** The
opening state stays unknown and the TV is driven from the first display-state
*change* instead.

This was originally "assert ON at service start unconditionally, which is wrong
only for a restart that happens while the display is off". That dismissal does
not survive being read next to the rest of this section. A restart while the
display is off *is* the 3am case — the one the uptime gate exists to prevent on
Linux and the one `PBT_APMRESUMEAUTOMATIC` is deliberately ignored to avoid here
— and the installer configures `restart/5000` on failure, which makes an
unattended restart a configured behaviour rather than a hypothetical. The
fallback would also be most active on precisely the machine where registration
does not deliver, so it was riskiest where it was most likely to run.

The asymmetry stated further down settles it: a spurious OFF is corrected by the
next display-on notification, while a spurious ON leaves a lit TV in a dark room
until somebody notices. A missed ON the user can undo with their own remote is
the smaller failure.

The cost is real and is not hidden: on a machine where the initial value never
arrives, nothing drives the TV until the first display transition. Reaching that
line at all means the assumption this whole design rests on is false on that
machine, so it is logged as a diagnostic — three lines saying the opening state
is unknown and that the boot `ON` needs rethinking — rather than quietly papered
over with a guess.

### Required changes — all landed

**Every item below is done.** Kept as the record of what was decided and why,
because the reasoning is what stops a later change quietly undoing one of them —
not as a list of outstanding work. What came out differently once the code was
written is in
[where the implementation departs](#where-the-implementation-departs-from-the-plan);
what was added afterwards is in
[completing the three power models](#completing-the-three-power-models--2026-08-26).

**`WindowsPowerMonitor.cpp` — rewrite rather than edit.** The Win32 boilerplate
(dispatch table, `RegisterServiceCtrlHandlerExW`, `SERVICE_STATUS` setup, the
global-instance routing) is correct and should be kept close to verbatim. The
logic and every comment get replaced, because the comments assert things that
are false and it is the mental model, not the syntax, that produced the bugs.

| | Change |
|---|---|
| Remove | The blocking-in-handler model. |
| Remove | Comments claiming the handler "achieves the same guarantee as the Linux inhibitor lock" and that "the OS waits for the handler to return before suspending". Both untrue. |
| Remove | The local `stopEvent` that nothing ever signals, so the process lingers holding the HID device open and an immediate restart cannot find the ESP32. |
| Add | `RegisterPowerSettingNotification` on `GUID_CONSOLE_DISPLAY_STATE`. The `POWERBROADCAST_SETTING` it delivers is valid only for the duration of the handler call, so the value must be copied out before returning. The handler currently discards `eventData` entirely. |
| Add | `PBT_APMSUSPEND` and `PBT_APMRESUMEAUTOMATIC` as secondary triggers, absorbed by the last-asserted-state check rather than deduplicated by hand. |
| Add | Last-asserted-state member; a send that would repeat the current state is skipped, but only inside a suspend — see [the reversals](#five-decisions-the-sanity-check-reversed). |
| Add | Generation counter, bumped on every power transition. The worker discards queued or in-flight work belonging to a previous generation — see [in-flight work across a suspend](#threading-model--worker-thread). |
| Add | Stop event as a member, signalled from `SERVICE_CONTROL_STOP`. |
| Add | `SERVICE_PRESHUTDOWN_INFO` to set the preshutdown timeout explicitly rather than inheriting the default. |
| Add | Serialisation around `SERVICE_STATUS` and `SetServiceStatus`, now called from two threads. |
| Add | `ERROR_CALL_NOT_IMPLEMENTED` for unhandled control codes, and a status report for `SERVICE_CONTROL_INTERROGATE`. The handler returns `NO_ERROR` for everything today, which claims support it does not have. |
| Change | Worker-thread model: the handler signals and returns promptly, reporting `SERVICE_*_PENDING` with advancing checkpoints while the work happens. Blocking inside the handler is against the documented contract. |

**`WindowsPowerMonitor.h`** — structurally fine, same class and overrides. Needs
the new members (stop event, power-setting notification handle, last asserted
state, generation counter, worker, status mutex) and its doc comment rewritten;
it carries the same false parity claim.

**`daemon/src/main.cpp`**:

| | Change |
|---|---|
| Unchanged | `systemJustBooted()` stays inside `#ifdef __linux__`, returning `true` elsewhere. Windows answers the presence question through display state instead, so no `GetTickCount64()` branch is written. |
| Add | Single-instance guard via a named mutex. hidapi's Win32 backend opens the device with read and write sharing, so a hand-run copy and the service can both hold it and either can consume the other's ACK — a false ACK, which is the failure the sequence byte exists to eliminate. The console-logging decision below makes running by hand a supported case, so this is a door the plan itself opens. |
| Change | Logging destination. A Windows service has no console, so every `std::cout` vanishes and `std::unitbuf` does nothing. See [Logging](#logging--decided). |

**`daemon/src/HIDTransport.cpp` — the budget becomes a parameter, not a
platform constant.** The current values are sized against logind's 5s. A Windows
suspend needs roughly 1500ms total with a ~500ms ACK wait, but those numbers are
wrong for Windows *shutdown* and *boot*, where the SCM allows minutes and where
a reopen after a replug cannot fit in 500ms.

The budget is not platform-specific, it is **event**-specific: how long will
this OS wait for me, for *this* event?

| Event | Linux | Windows |
|---|---|---|
| Sleep | 4000ms default (unchanged, proven) | 1500ms — the suspend proceeds regardless |
| Shutdown | 4000ms default (unchanged, proven) | 20000ms, against a 60s preshutdown timeout |
| Wake, display change, device arrival | 4000ms default | 4000ms default — nothing is waiting |

Implemented as `send(cmd, budget = SEND_BUDGET)`, with the value carried to the
call site in the `TvCommand` the monitor raises. The archived CDC transport is
untouched and Linux states nothing, so it takes the default everywhere.

This was previously deferred to a later consolidation pass on the grounds that
it was an expensive interface change. With a defaulted parameter it is not, and
the argument for the worker thread below — that an invariant nothing enforces
will eventually be violated — applies here with equal force. A
`#if defined(_WIN32)` pair of constants would be that invariant.

**A first attempt got the shape wrong**, and it is worth recording because the
mistake looked exactly like the fix. The budget was supplied by
`IPowerMonitor::sleepBudget()` and `shutdownBudget()` — per-platform virtuals
answering per-event questions. That is one step better than a constant and one
step short of correct: it made the budget a property of *which of three
callbacks was invoked*, so a display change routed through `OnSleep` inherited a
suspend's 1500ms and announced itself in the log as `(sleep)`. The three
callbacks are now one, and both the deadline and the reason travel in the
`TvCommand`.

**`daemon/CMakeLists.txt`** — every `install()` rule and the whole CPack block
are inside `if(UNIX AND NOT APPLE)`. Windows has no install path at all.

**`.github/workflows/packages.yml`** — a `windows-latest` job that configures
and builds, on push rather than on tags. At the time this was written the
Windows sources had never been compiled even once, and nothing would have
noticed if they stopped compiling. The cheapest item on the list by a wide
margin, and the only one that turned "stale on top of untested" into a standing
guarantee. A matching `linux-build` job was added alongside it.

### Logging — decided

**Redirect stdout and stderr to a file when running as a service.** Roughly five
lines at service start; every existing `std::cout` in the daemon keeps working
untouched, and the output stays byte-identical to what Linux writes to the
journal — which matters while the two platforms are being compared against each
other.

The redirect is conditional. `StartServiceCtrlDispatcherW` fails with
`ERROR_FAILED_SERVICE_CONTROLLER_CONNECT` when the process was not launched by
the SCM, which is the standard way to tell "running as a service" from "run by
hand in a terminal". Console output stays untouched in the second case, so
development does not mean tailing a file.

**The log path and the service account are one decision, not two.** LocalSystem
starts with a working directory of `System32`, and a low-privilege account
cannot write beside the binary in `Program Files` — so "LocalService once
tested" and "a log file" constrain each other. The destination is
`%ProgramData%\ESP32IRRemote\`, resolved through
`SHGetKnownFolderPath(FOLDERID_ProgramData)` rather than a relative path, with
the ACL set by the installer.

The log gains a size check at startup — rename to `.old` past a megabyte. At the
daemon's rate, a handful of lines a day, that is under a megabyte a year; the
check is insurance, not a rotation scheme. It runs before the file is opened,
which is the only ordering that works: Windows has no rename-over-open
semantics, so a rename fails outright while any handle is held. A crashed
instance still holding the file during the 5s restart delay is the case to
tolerate rather than treat as an error.

Event Log was the alternative. It is the idiomatic choice and self-bounding, but
it needs a registered event source, a message resource DLL for clean formatting,
and a logging abstraction spanning both platforms — real work whose benefits
accrue to administrators managing machines they did not build, which is not the
situation yet.

### Planned: abstract logging behind an interface

Once both platforms are verified working, logging should move behind an
interface the way transport and power events already are. This is deliberate
sequencing — get it working, then consolidate — not an oversight.

| Platform-specific today | Behind an interface |
|---|---|
| logging: file vs journal | `ILogger` — `FileLogger`, `EventLogLogger`, journal-by-stdout |
| choosing which implementations to construct | stays; one conditional in `main.cpp` |

Two rows that used to be on this list have left it. The send budget is being
done now, carried in `TvCommand` rather than deferred, for the reasons in
[Required changes](#required-changes--all-landed). And `systemJustBooted()` no longer needs
consolidating: it stays Linux-only, because the Windows answer to the same
question comes from display state rather than from an uptime reading. The
abstraction that looked necessary turned out to be a translation that should not
have been attempted.

End state: exactly one platform conditional in `main.cpp` — which
implementations to build — and nothing conditional anywhere else.

### Decisions — settled

| Decision | Chosen | Reasoning |
|---|---|---|
| `WindowsPowerMonitor.cpp` / `.h` | **Rewrite both**, keeping the Win32 boilerplate near-verbatim | The existing comments assert things that are false — that the handler "achieves the same guarantee as the Linux inhibitor lock", that "the OS waits for the handler to return before suspending". Wrong comments outlive wrong code, because they get read and believed. The dispatch table, `RegisterServiceCtrlHandlerExW` and `SERVICE_STATUS` setup are correct and get retyped as-is. |
| Presence signal for **ON** | **`GUID_CONSOLE_DISPLAY_STATE`**, on every machine, with `PBT_APMRESUMESUSPEND` as a fallback | The display lighting up answers presence directly, is the documented signal on Modern Standby, and is the only route to an ON at boot under Fast Startup. Discrete IR codes make the overlap with the resume event free. |
| Signal for **OFF** | **`PBT_APMSUSPEND` where the machine has S3; display-off only where it does not** | The two halves of the display signal have opposite failure modes. A wrong ON is undone with the user's own remote; a wrong OFF blacks out a TV somebody is watching. Pay that only where there is no alternative — see [the two halves](#the-two-halves-of-the-display-signal-are-not-equally-safe). |
| Boot detection | **None on Windows** | The uptime gate does not survive Fast Startup, and display state removes the need for it. `systemJustBooted()` stays Linux-only. |
| Idle screen blank | **Turns the TV off on Modern Standby only** | Forced there, because no suspend event arrives. Ignored on an S3 machine, which then matches Linux. The divergence is confined to the machines whose API leaves no choice. |
| Send budget | **Carried in `TvCommand`**, not a platform constant and not a virtual on the monitor, and honoured in both directions | The budget belongs to the *event*. Windows states less than the default for a suspend (~1.5s), more for a shutdown (20s, because preshutdown allows minutes), and nothing at all for a display change or a device arrival. Attaching it to an event class instead let a screen blank inherit a suspend's deadline — see [Timing](#invariants). |
| Single instance | **Named mutex** | The service and a hand-run copy can both open the HID device and steal each other's ACKs. |
| Service account | **LocalSystem now, LocalService once tested** | Whether hidapi's `CreateFile` open works under a low-privilege account is answerable in minutes with a working binary and not at all without one. Mirrors Linux, which ran as root until it worked and was then moved to `esp32ir`. |
| Install | **`sc create` now, MSI at first release** | Packaging follows a working daemon, as it did on Linux. Use `start= auto`, not `delayed-auto`: nothing downstream now depends on a boot window, but a delayed start also postpones the first display-state reading by up to two minutes. |
| Recovery | **Restart on failure, 5s delay** | Mirrors `Restart=on-failure` / `RestartSec=5`. Set in the same `sc` sequence as the install. |

### Threading model — worker thread

Control handlers push the event and return; a single long-lived worker thread
does the send. The handler reports `SERVICE_*_PENDING` with advancing
checkpoints during preshutdown so the SCM knows work is still in progress.

Blocking inside the handler was considered and rejected. It would have been
simpler — and viable, since the send is bounded at ~1.5s against roughly 30s of
SCM tolerance — but the bound lives in `HIDTransport.cpp` while the code relying
on it lives in `WindowsPowerMonitor.cpp`, with nothing but a comment connecting
them. Raise `SEND_BUDGET` for a good reason on Linux and the Windows handler
silently starts blocking longer than the SCM tolerates. The budget is already
marked as needing measurement, so it *will* change.

That is the same defect shape as fix 11 (correctness resting on `-D` ordering)
and fix 16 (timeouts individually fine, collectively over budget): an invariant
nothing enforces. The worker removes the coupling entirely — the next person
needs to know nothing about a constant in another file.

**What the single worker does and does not make safe.** It serialises sends by
construction, so `HIDTransport`'s single-threaded assumption holds without shared
code gaining a lock it does not need on Linux. It does *not* cover
`SERVICE_STATUS`. Advancing checkpoints while the work happens means the worker
reports status too, so the struct and `SetServiceStatus` are now touched from
both the SCM handler thread and the worker. That needs a mutex; the transport
does not.

**The wait hint has to outlast the longest send.** The checkpoint advances
either side of a send, not during one, so `dwWaitHint` is the SCM's answer to
"how long may the gap between two checkpoints be before this service is hung?".
The longest a single send can take is `SHUTDOWN_BUDGET`, 20s — so a 15s hint,
which is what this originally carried, let the SCM call a working preshutdown
send a hang. It is 30s now, with a `static_assert` tying it to the budget rather
than a comment asking the next person to remember. Same defect shape as fix 11
and fix 16 again: two numbers that have to hold a relationship, and nothing
enforcing it.

That invariant is why the device-removal notification added in 2026-08-26 does
not call `ITransport::invalidate()` from the handler, even though it would be
one line. The handler records the removal and returns; the worker performs the
invalidation before its next send. A notification that reached across to the
transport directly would break the single-threaded guarantee the worker exists
to provide, and would do it from the one code path where the device really is
disappearing underneath an in-flight write.

**The queue holds a state, not a backlog.** `ON` and `OFF` are idempotent
assertions about what the TV should be, not commands that each need executing,
so the queue coalesces to the latest one. A bounded queue that replayed a
backlog would fire stale commands after a resume.

**In-flight work across a suspend.** The machine suspends ~2s after
`PBT_APMSUSPEND` regardless, so the worker will sometimes be frozen mid-read and
thaw on resume, holding a deadline computed before the machine went down and
facing a device that may have re-enumerated. `HIDTransport` uses
`std::chrono::steady_clock`, which is `QueryPerformanceCounter`-backed on MSVC,
and whether it advances across a suspend is platform-dependent. Both answers are
wrong for us: if it stops, a stale `OFF` completes long after the wake `ON`
should have; if it advances, the send returns instantly having done nothing and
reports a failure that did not happen.

The generation counter is the fix, and it is deliberately not a timing
assumption: every power transition bumps it, and the worker drops anything
carrying an older value. Correct whichever way the clock behaves.

**Only a power transition may bump it.** A device arriving or leaving is not
one, and bumping for either would discard an in-flight send that was correct and
log `power state changed mid-command` about a machine whose power state did
nothing of the sort — a false statement in the log, which is the failure class
this project cares about most. The removal path was already written that way and
said why; the arrival path was not, because it re-asserts through the same
function every power event uses. It is an exception in that function now.

### Risks to check on the real machine

- ~~**Which configuration this machine is.**~~ No longer a question anyone has
  to answer by hand: the daemon reads `SYSTEM_POWER_CAPABILITIES` at startup and
  logs it. The 2026-08-24 laptop is Modern Standby, Fast Startup on, S3 absent
  in firmware; other machines now cover the rest. See
  [what each machine can prove](#what-each-machine-can-and-cannot-prove).
- **Spurious resumes during DRIPS.** A Modern Standby machine cycles in and out
  of the low-power state. If `PBT_APMRESUMESUSPEND` is raised on one of those
  exits while the display is still off, an unguarded handler would light the TV
  in a dark room. The handler now defers to the last known display state for
  that reason — watch the log for the `not asserting on` line, which says the
  guard fired and therefore that the risk was real.
- ~~**Display-state registration.**~~ **Answered 2026-08-26, both halves.**
  Registration delivers the current value — the first service start logged
  `display state = on` and drove an `ON` from it within 2ms — and
  `GUID_CONSOLE_DISPLAY_STATE` does reach a session-0 service, which every
  display transition since has confirmed.
- **The capability report is now load-bearing.** `AoAc` and `SystemS3` used to
  be diagnostics; they now decide whether an idle screen blank turns the TV off.
  Read the `display-off policy:` line on each of the three machines and confirm
  it says what that machine actually is — a misread there is no longer a
  confusing log, it is the wrong policy running. The failure is bounded in the
  right direction (an unreadable capability set falls back to driving the OFF
  from the display, which errs towards the device doing its job) but it is no
  longer free.
- **The ~2s grace period** is guidance, not a contract. **First measurement
  2026-08-27: 206ms of the 1500ms budget**, on a hibernate where the display-off
  and the suspend arrived 41ms apart. It is only measurable when they collide
  closely enough that the suspend send is not already suppressed. The Modern
  Standby half of the question — how much runway a session-0 service gets before
  the Desktop Activity Moderator throttles it — is still unmeasured, and has not
  bitten yet.
- ~~**HID access as a service.**~~ **Confirmed 2026-08-26.** hidapi's
  `CreateFile` open works under LocalSystem; every command since has gone
  through it. Whether it also works under LocalService is a separate question,
  and it is the reason that account move is still on the list.
- ~~**USB across standby — the largest open risk.**~~ **Closed 2026-08-26.**
  The concern was that Modern Standby suspends the bus regardless of any
  registry setting, and that the firmware's recovery
  (see [above](#usb-suspend--the-defect-the-windows-build-found)) might not
  carry across it — which would fail the wake `ON` after every standby, this
  device's main job on a laptop.

  Three cycles ran. The device is cycled across the resume — removed and back
  inside 0.4s on the short one, with nobody near the machine — re-enumerates,
  and answers. The first of the three outcomes this section listed is what
  happened: the fix working, at full speed, with no budget revision needed. The
  fallback that had no fix in hand — forcing re-enumeration from the host with
  the Windows configuration manager — is not required.

  One thing the cycles did *not* settle: whether the device stays torn down for
  the whole of a long standby. The only long absence in the log spans a physical
  unplug and cannot be read as the platform's doing.

- **Re-enumeration churn on Modern Standby — the risk the fix itself creates.**
  The firmware's recovery is written as costing nothing: *"a clean resume is
  re-enumerated for nothing, at about a second of unavailability that nothing
  observes."* That holds for one suspend a day. It may not hold on a machine
  that dips in and out of DRIPS continuously, where the bus can be suspended and
  resumed many times an hour.

  If every DRIPS exit triggers a `tud_disconnect()` / `tud_connect()`, the
  device spends a second unavailable each time, and any command landing in that
  window fails and has to be retried. The recovery would then be generating the
  failures it exists to prevent. Recovering blindly is only cheaper than
  detecting while the recovery is rare.

  **Measure before changing anything.** The device-arrival lines in the log are
  the instrument — count them across an idle hour on the Modern Standby machine.
  A handful is fine. Dozens means the recovery needs a rate limit, or needs to
  distinguish a bus suspend from a system one after all. Nothing about this is
  worth pre-emptively engineering against a number nobody has yet.

### USB suspend — the defect the Windows build found

**Found by running the daemon on Windows for the first time, 2026-08-24.** The
first real defect the Windows work uncovered, and it is in the firmware, not
in either daemon.

Windows powers down an idle USB device when nothing holds a handle open. This
ESP32 does not come back from it. The device NAKs its OUT endpoint
indefinitely, so `hid_open` still succeeds and every write then fails — hidapi
issues an overlapped `WriteFile`, gets `ERROR_IO_PENDING`, waits its internal
one second for completion, and gives up:

```
[transport] Write failed (hid_write/WaitForSingleObject:
            (0x000003E5) Overlapped I/O operation is in progress.)
```

It stays broken until the device is physically re-enumerated, which is what
made it look like "works for a while, then refuses until replugged". The
presentation is worth remembering, because three plausible explanations fit it
and all three were wrong:

| Ruled out | By |
|---|---|
| VID/PID collision, `hid_open` taking the wrong device | `Get-PnpDevice` showing one physical unit as a USB parent and its HID child — the normal tree |
| Firmware publishing a bad report descriptor (`USB.begin()` before `HID.begin()`) | The host's parsed descriptor dumping as the exact 34 bytes the firmware defines, Output item and all |
| A hung `loop()`, the known missing-watchdog issue | The button and OLED continuing to work throughout a failing streak |

What identified it was noticing the failure tracked **idle time** rather than
any command — and that `hid_error()` named a completion timeout rather than a
rejected write. Diagnostics were the whole difference; "Write failed" alone
supported all four stories equally.

**Two traps in confirming it.** The global power-scheme setting is overridden by
a per-device value, so changing the scheme proves nothing. And on a laptop the
AC and DC values are separate — `powercfg /setacvalueindex` alone is a no-op
while running on battery, which is how the first attempt at a fix appeared to
disprove a correct hypothesis.

#### The fix — the firmware rebuilds its USB state after a suspend

The device cannot tell a wedged OUT endpoint from an idle one; both are silence.
So rather than detecting the bad case, it stops trusting USB across a suspend
and rebuilds it unconditionally. `tud_disconnect()` drops the D+ pullup and
`tud_connect()` raises it — a replug nobody has to perform.

A clean resume gets re-enumerated for nothing, costing about a second of
unavailability that nothing observes. A broken one is repaired. Recovering
blindly is cheaper than detecting, and it does not depend on having correctly
guessed which resumes are the bad ones.

Registered through `ARDUINO_USB_SUSPEND_EVENT` / `ARDUINO_USB_RESUME_EVENT`,
and acted on in `loop()` rather than in the callback — tearing down USB from
inside a USB event is how a deadlock gets built. Only a resume that follows an
observed suspend counts, so resumes that never powered the bus down do not
cause churn.

The daemon side changed to match. A write now retries until the send budget
runs out instead of giving up after a single reopen: re-enumeration takes
around a second, and one retry lands inside that window and reports failure for
a device that was about to be fine. The budget was already the limit on
everything else in a send, so nothing new decides how long is too long.

#### What is still worked around, and what is still open

`install-service.ps1` continues to set the per-device registry values:

```
HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_1234&PID_5678\<serial>\Device Parameters
    SelectiveSuspendEnabled        = 0
    EnhancedPowerManagementEnabled = 0
```

This is now defence in depth rather than the fix. With the firmware recovering,
an idle suspend is survivable; not entering one in the first place simply avoids
re-enumeration churn on a device that is idle most of the time. It is also
per-instance — a different physical unit enumerates with a new serial and gets
Windows' defaults back — and the real answer for it remains an INF carrying the
values against the hardware ID, which arrives with the driver package and code
signing already listed as gaps.

**Still unproven:** that the same recovery works across a Modern Standby cycle,
where the bus suspends regardless of any registry setting. That is the case the
device exists for on a laptop, and it is the next thing to test.

#### What Linux actually does across a suspend

**Measured 2026-08-26, on the newly flashed firmware.** The previous version of
this brief predicted that Linux would now see the re-enumeration too, and told
the reader to expect it. It does not, and the prediction is left recorded here
rather than quietly deleted, because the reason it was wrong is the useful part.

A full `deep` (S3) suspend/resume cycle, 16 seconds, with the daemon running:

| Observation | Reading |
|---|---|
| No kernel disconnect, reset or re-probe for the device after the initial plug | It stayed enumerated across the whole suspend |
| `/dev/hidraw10` kept its original timestamp | No new node, so no `tud_disconnect()` / `tud_connect()` |
| The wake write succeeded on attempt 1, through an fd opened *before* the suspend | The OUT endpoint never wedged |
| `power/control` = `on`, `runtime_suspended_time` = 0 | The bus has never selective-suspended this device |

So the recovery correctly declined to fire: the firmware only re-enumerates
after a suspend it actually observed, and it observed none. What this cycle
demonstrates is that the fix causes **no regression** on Linux — not that it
works, because nothing asked it to.

The explanation this brief carried — that Linux escapes the defect because its
daemon holds the device open continuously — is *consistent* with this but is not
what was measured. What was measured is `power/control = on`: the kernel is not
runtime-suspending that port at all, which is a stronger and more specific
statement than "an open handle keeps it awake".

**The consequence for the Windows work.** This made a Modern Standby cycle the
largest open risk rather than merely the next item, because it was the first
place the firmware fix and the daemon's retry path would be tested for real.
**Settled 2026-08-26:** three cycles ran, the device was cycled across the
resume and re-enumerated, and the daemon's arrival path recovered the `ON` that
failed while it was away. The risk is closed; the Linux-side reproduction below
is kept because it is still the only way to exercise the path *on Linux*.

**A way to exercise it on Linux without waiting for Windows**, since nothing so
far has: force the selective suspend the firmware is waiting for. The daemon's
open handle is what keeps the port active, so it has to stop first.

```
sudo systemctl stop esp32-ir-remote
echo auto | sudo tee /sys/bus/usb/devices/<port>/power/control
sleep 4; cat /sys/bus/usb/devices/<port>/power/runtime_status   # expect: suspended
echo on   | sudo tee /sys/bus/usb/devices/<port>/power/control
```

If the fix works, the resume produces a new `hidraw` node and a kernel
disconnect/connect pair. If it does not, the suspend event is not reaching the
handler — which is a firmware finding, and far cheaper to chase here than on
Windows.

### What each machine can and cannot prove

**The sleep model is a property of the platform firmware.** A machine supports
either classic S3 or Modern Standby, never both, and no setting switches it. So
a single machine can only ever be evidence for one of them, and the 2026-08-24
test laptop — which reports S1/S2/S3 unavailable *in firmware*, with Fast
Startup on — could only ever be evidence for Modern Standby.

That was recorded here as a permanent limit. It was a limit of the hardware to
hand rather than of the work, and it has lifted: there is now hardware covering
S3, hibernate and Modern Standby.

What this means in practice:

| Configuration | What only that machine can prove |
|---|---|
| **S3, Fast Startup off** | `PBT_APMSUSPEND` as a real suspend trigger; a true cold boot, and therefore the only honest test of the boot `ON` |
| **S3 or S4, Fast Startup on** | That the boot `ON` survives a hibernated kernel session — the case the uptime gate could never have handled |
| **Hibernate (S4)** | ~~That a resume after full power loss to the device re-establishes the transport. The ESP32 *will* have re-enumerated, so this is the cheapest real test of the device-arrival path.~~ **Wrong, and shown to be on 2026-08-26.** The ESP32 did not re-enumerate: no removal, no arrival, the handle survived and the first `ON` was ACKed. What this machine class actually proves is the opposite — that a hibernate is *less* disruptive to USB than a Modern Standby cycle |
| **Modern Standby** | The DRIPS cycle, the display-state signal in its native habitat, and whether the firmware's USB recovery survives a bus suspend that no registry setting prevents |

Two things remain measurements rather than pass/fail, and both need the real
hardware:

- **The suspend grace period.** Microsoft's ~2s is guidance, not a contract. The
  daemon now measures and logs what each send actually used, so this is read off
  the log rather than assumed. **First number, 2026-08-26: 206ms of the 1500ms
  budget**, on the Modern Standby laptop's hibernate. Only measurable when the
  display-off and the suspend collide closely enough that the suspend send is
  not already suppressed — 41ms apart here, against 613ms on the Fast Startup
  shutdown, where nothing was sent and nothing could be timed.
- **The Modern Standby runway** between display-off and the Desktop Activity
  Moderator throttling a session-0 service. Same category, same method.

Whichever model a given log came from, the daemon's own opening line now says
so, so a report cannot silently be filed against the wrong configuration.

### Verification plan

Mirror what was done on Linux, since that is what "up to standard" means here.
**Started 2026-08-26 on the Modern Standby laptop**: the four rows that need no
power transition pass there. Everything still marked `no` needs somebody at the
keyboard watching a TV. Tick these off as they pass, the way the Linux second
pass was recorded, so the record is a record rather than a memory.

Run `verify-windows.ps1` on each machine first: it captures the power model,
service configuration, device state and log in one file, which is what makes
three separate sessions comparable afterwards.

**Per machine — every machine:**

| | Test | S3 box | S4 box | Modern Standby |
|---|---|---|---|---|
| 1 | Device reachable — `--console` sends `ON`, gets the ACK | no | no | **yes**, 2026-08-24, again 2026-08-26 |
| 2 | Service installs, starts, and logs its power model **and its display-off policy** correctly | no | no | **yes**, 2026-08-26 |
| 3 | Display returning turns the TV on | no | no | **yes**, 2026-08-26 — three times, on real resumes |
| 3b | Idle screen blank: TV **off** on a machine with no S3, TV **left on** where the log says the blank is ignored. Both outcomes are a pass — the log line says which to expect | no | no | **yes**, 2026-08-27 with a 5-minute timeout set. TV off, which is the right outcome against this machine's policy line. But the blank could not be separated from a standby entry — see [the blank is the standby](#the-screen-blank-is-the-standby-entry) |
| 4 | Sleep and wake, user-initiated | no | no | **yes**, 2026-08-26 — three cycles, `OFF sent and ACK received (display off)` going down and `ON` on the way back |
| 5 | Shutdown, then boot | no | no | **yes**, 2026-08-26 — `OFF` ACKed 613ms before the machine went; `ON` on the way back. See [the Fast Startup shutdown](#the-fast-startup-shutdown--the-suspend-path-does-run-here) |
| 6 | Service restart with the display on: `ON` re-asserted, no visible change | no | no | **yes**, 2026-08-26 — five consecutive restarts after the [race fix](#the-first-install--four-defects); the "no visible change" half still needs an eye on the TV |
| 7 | ESP32 unplugged and replugged while running — removal logged, arrival re-asserts | no | no | **yes**, 2026-08-26 — twice by hand, and four more times by the machine itself across standby |
| 8 | Graceful behaviour with the ESP32 absent entirely | no | no | **yes**, 2026-08-26 — a start with no device logs `ESP32 not found at startup — will retry when needed` and stays up; a send with no device logs `ON FAILED — no ACK, TV state not changed` rather than claiming success |
| 9 | A hand-run copy refusing to start while the service holds the mutex | no | no | **yes**, 2026-08-26 |
| 10 | Unconfigured profile answering `ERR` | no | no | **yes**, 2026-08-27 — both directions, and the OLED showed its no-IR-code message. `ESP32 returned ERR for command: OFF` → `OFF FAILED — no ACK, TV state not changed`, then the same for `ON`. The failure is reported at both layers and the TV's believed state is left alone |

**Configuration-specific — only the machine that has it can answer:**

| | Test | Where |
|---|---|---|
| 11 | `PBT_APMSUSPEND` fires and drives the OFF | S3 machine only — half of it landed elsewhere on 2026-08-26: the event *fires* on the Modern Standby laptop's Fast Startup shutdown, but it did not drive the OFF, because the display-off had already sent it. "Drives the OFF" still needs a machine where the suspend arrives without a display change in front of it |
| 12 | Cold boot with Fast Startup **disabled** — the only true cold boot | S3 machine, Fast Startup off |
| 13 | Boot with Fast Startup **enabled** | any machine with hibernate — **passed 2026-08-26** on the Modern Standby laptop, `boot type 0x1`. The strongest form of the pass: no service start appears in the log at all, because Fast Startup resumed the same process, and the `ON` happened anyway |
| 14 | Hibernate and resume; device re-enumerates and the arrival re-assert lands | S4 machine — **passed 2026-08-26** on the Modern Standby laptop (`boot type 0x2`), but *not as described*: the device never re-enumerated and the arrival re-assert never ran. See [the hibernate](#the-hibernate--and-the-two-predictions-it-falsified) |
| 15 | A full Modern Standby cycle with the firmware USB fix in place | Modern Standby only — **passed 2026-08-26**, three cycles including one of 4h11m. See [what the standby cycles showed](#what-the-modern-standby-cycles-actually-showed) |
| 16 | An unattended wake leaving the TV **off** — wake timer or Wake-on-LAN | any; easiest where a wake timer can be set — **passed 2026-08-26** by accident rather than design: Windows woke itself at 21:15:54 for a maintenance window, re-entered standby five seconds later, and the daemon logged nothing at all. The display never came on, so nothing asserted `ON` |
| 17 | Playback the display-blank policy is supposed to protect: a browser-based video and a controller-driven game left running past the screen timeout. TV must stay on | Modern Standby only — the only machine where the blank drives an OFF. **Closed as out of scope 2026-08-27.** Both halves passed on the way to that conclusion — Firefox 35 minutes with a `DISPLAY` request captured live, Hollow Knight 14 minutes on a controller with no request in any of 45 samples, neither blanking. But the row should not have existed: the daemon mirrors the display and does not influence it, so a display that times out mid-game takes a monitor dark exactly as it takes the TV off. See [what actually protects playback](#what-actually-protects-playback) |

Test 15 was not a formality: it was the one
[the largest open risk](#risks-to-check-on-the-real-machine) turned on, and
passing it three times is what closed that risk. Test 16 was the one most
easily got wrong, because getting it wrong looks like nothing happening — it
passed by accident, on a maintenance wake nobody scheduled. Test 17 was written
as the one an actual user would find first, and the only one testing a product
decision rather than a mechanism; it was
[closed as out of scope](#what-actually-protects-playback) instead, once it
became clear the daemon mirrors the display and a monitor goes dark at the same
instant the TV does.

### Gaps against a genuinely commercial Windows product

The decisions above are sound, but the plan they add up to is not yet what a
shipped Windows product looks like. Named here so the difference is deliberate
rather than discovered later.

| Gap | Notes |
|---|---|
| **Code signing** | Unsigned binaries and installers trigger SmartScreen warnings, and many corporate environments refuse them outright. A commercial Windows product signs both. Needs a certificate — a real cost, and a lead time. |
| **Windows Event Log** | The daemon logs to a file. The idiomatic Windows answer is the Event Log, so failures surface in Event Viewer where an administrator already looks. Deliberately deferred — it needs a registered event source, a message resource DLL and a logging abstraction spanning both platforms, and its benefits accrue to administrators managing machines they did not build. Revisit once the three power models are verified. |
| **Device notification on Linux** | ~~Both platforms~~ — Windows now reacts to `SERVICE_CONTROL_DEVICEEVENT`, added 2026-08-26. Linux still discovers a missing ESP32 only by failing a write and reopening; udev monitoring is the equivalent. The gap is now one-sided rather than symmetric. |
| **Installer scope** | Partly closed 2026-08-26: `install-service.ps1` now upgrades in place, stops and removes the running service first, cleans up its registry values on uninstall, and sets the log ACL. Still not an MSI — no per-user upgrade path, no rollback, no Add/Remove Programs entry, and it needs an execution-policy bypass to run at all. |
| **Recovery policy detail** | `sc failure` takes actions for first, second and subsequent failures plus a reset period. The installer sets restart/5s three times with a one-day reset, which specifies all of them — but the decision of whether to *stop* retrying eventually is still unmade, and restarting forever is a choice by default rather than by intent. |
| **The behaviour still differs by machine** | Scoping the display-off OFF removed the divergence on S3 machines, but a Modern Standby laptop still turns the TV off when its screen blanks and an S3 desktop does not. That is forced by the API rather than chosen, and it is not something a user can be expected to deduce. It needs a sentence in the user documentation that does not exist yet, and it is the one behaviour where "it works differently on my other PC" is a correct observation rather than a fault. |
| **No way to override that** | The daemon has no configuration of any kind — no file, no registry values it reads, no flags beyond `--console`. Every policy in it is a compile-time decision. **The case for an override got weaker on 2026-08-27, not stronger**: the two scenarios that motivated it — a game losing the TV mid-session, and music dying when the screen blanks — were both ruled [out of scope](#invariants), the first because a monitor blanks identically and the second because the TV is a display and not an audio device. What is left is a real but much smaller argument: somebody with an unusual setup and no way to say so. Worth revisiting when such a person appears rather than in anticipation of them. |
| **Automated tests** | There are none, and Windows makes it two platforms verified by hand — now across three sleep models and two boot models. Every future change needs exercising several times over. A fake `ITransport` and a fake `IPowerMonitor` would cover the protocol and the state logic without hardware, which is where most of the matrix actually lives. The single command callback makes this materially cheaper than it was: a fake monitor now implements one method rather than five. This is the largest single gap on the list. |

---

## Invariants

Rules the implementation has to keep. Each was learned the hard way; the commit
history has the incidents. Breaking one of these is how the project regresses.

**Honest reporting**

- A success report means the thing happened. ACK is sent only after the IR
  signal has fully transmitted. An unset (`0x0`) code answers `ERR`, never `ACK`.
- Replies are correlated to requests by sequence byte. Never infer a reply's
  identity from timing.
- `ITransport::send()` returns whether the device *confirmed* the command. A
  transport that cannot confirm must say so in its contract.

**Timing**

- The send budget belongs to the *event*, not the transport — and not to the
  event's *class* either. It is however long this OS will wait for us before
  proceeding without us, for this one event, and only whatever raised it knows
  that. It travels in the `TvCommand`; `send()` takes it as a defaulted
  parameter and honours it **in both directions**, bounded only by
  `MAX_SEND_BUDGET` as a backstop against a bug.
- Two earlier versions of that rule were wrong, in opposite ways, and both cost
  something real. A budget that could only ever *tighten* made the transport's
  Linux-sized default a ceiling for every platform, silently capping the Windows
  shutdown at four seconds when preshutdown allows sixty — on the one send with
  no event after it to correct a TV left on. A budget supplied by
  `sleepBudget()` / `shutdownBudget()` then attached it to which of three
  callbacks was invoked, so a Windows screen blank inherited a suspend's
  1500ms while nothing was waiting for it at all.
- A send under no deadline states zero and takes the transport's default. That
  is not a detail: the display, wake and device-arrival paths are the ones most
  likely to find the ESP32 mid-re-enumeration, so they are exactly where a
  borrowed suspend budget does the most damage.
- Linux: `send()` must complete within logind's `InhibitDelayMaxSec` (5s
  default). One shared budget, currently 4s, covering open + reopen + ACK wait.
- Windows: a suspend cannot be delayed at all. `PBT_APMSUSPEND` is a
  notification, the machine goes down about two seconds later regardless, and
  the sleep budget is 1500ms for that reason. Shutdown is the opposite case —
  preshutdown allows minutes, the installer sets 60s explicitly, and the
  shutdown budget is 20s to match.
- Suppressing a repeat command is allowed **only** where the budget is tight,
  which means the suspend path alone. Everywhere else the repeat is sent,
  because discrete codes exist so the TV reaches the right state regardless of
  drift, and skipping the send discards that repair. A device removal resets
  what the daemon believes the TV is doing: no channel, no knowledge.

**Threading**

- `HIDTransport` is single-threaded and holds no lock. Everything that touches
  it must run on one thread.
- On Windows that thread is the worker. A service control handler records what
  happened and returns; it never sends, and never invalidates the transport.
  This applies to device notifications exactly as it does to power events.
- `SERVICE_STATUS` is the exception that proves it: the worker advances
  checkpoints while the handler also reports state, so that struct *does* need a
  mutex. The transport does not, and must not be given one to paper over a call
  from the wrong thread.

**Persistence**

- Writes to LittleFS are atomic *and* length-checked. Atomic replacement of
  incomplete content is still corruption.
- Any index into the profile list is range-checked at the point of use, not
  only at the point of writing.

**Inhibitor lock** — Linux only; Windows has no equivalent and cannot delay a suspend

- Re-acquired on resume, ready for the next sleep.
- Acquisition never throws — it runs inside D-Bus signal handlers, where an
  exception unwinds into the sdbus event loop.
- A `false` from `send()` must never stop the system sleeping or shutting down.

**Process lifecycle**

- A command asserting the TV **on** must be caused by something that implies a
  person is there. Linux answers that with the uptime gate — the startup `ON`
  mirrors a *boot*, not a process start. Windows answers it with console display
  state, which asks the question directly. Neither may be replaced by "the
  service started", and `PBT_APMRESUMEAUTOMATIC` may never assert `ON`: a
  machine waking itself for a maintenance window is not somebody walking into
  the room.
- A command asserting the TV **off** must be caused by something that implies
  the PC is going away, wherever the platform can tell. The two directions are
  not symmetric and must not be given a symmetric rule: a wrong `ON` is undone
  with the user's own remote, while a wrong `OFF` blacks out a television
  somebody is watching. An idle screen blank is therefore an acceptable `OFF`
  trigger only on a machine that raises no usable suspend event — and the daemon
  decides that from the capability report rather than by assuming, because the
  answer differs per machine and is not observable from behaviour.
- Only one process may hold the device. Two openers of the same HID device
  consume each other's ACKs, which the sequence byte cannot catch — the reply is
  well-formed and correctly numbered, just for somebody else's request. Linux
  gets this from the service unit; Windows needs the named mutex.
- That mutex must tolerate a predecessor that is still exiting. A restart is not
  a handover: the SCM calls a service stopped the moment it reports
  `SERVICE_STOPPED`, while the old process is still alive and still holding the
  name. Refusing immediately makes every restart a race — one that was observed
  firing on all five of five restarts, and had simply been winning most of them.
  The wait is bounded, so a genuine second instance is still refused.
- `std::cout` must be unbuffered (`std::unitbuf`). Under systemd stdout is a
  pipe, so it is fully buffered by default and log lines never reach the journal;
  redirected to a file on Windows it is buffered for the same reason. Anything
  written just before the process is stopped is lost otherwise — including the
  `OFF` confirmation on shutdown, which is the one line most worth having.
- The log must stay readable *while* the daemon holds it. It is the only record
  the entire Windows verification plan is read from, and a log nobody can open
  until the service stops is not a log. This rules out the secure-CRT `_s` file
  opens, which take the file exclusively — and which fail silently at it, since
  the process doing the excluding is the one process that can still write.
- Every redirect of `stdout` or `stderr` is checked. An unredirected stream is a
  daemon talking to nobody, and it has no way to report that except the console
  it no longer has.

**Scope** — settled 2026-08-27, after test 17 spent an evening outside it

- **The daemon mirrors the display state. It never influences it.** No power
  request is asserted, no timeout is extended, no application is inspected. If
  Windows blanks the display the TV goes off, and the correct comparison is
  always a monitor in the same place, which goes dark at the same instant.
  Anything that would change *whether* the display blanks belongs in the user's
  power settings.
- Follows from that, and worth stating because it closes a whole class of
  question: **an application that lets the display time out is not this
  project's problem.** Games, players and anything else are judged only by what
  they do to the display state, never by what they are doing.
- **The TV is a display, not an audio device.** Audio is assumed to come from
  something connected to the PC directly, so "the display is legitimately off
  while the user is still listening" is not a case this device has to answer.
  Without this line the display-off policy needs an override, and with it the
  policy is simply correct.

**Windows scripts**

- The `.ps1` files keep their UTF-8 BOM. Windows PowerShell 5.1 reads a BOM-less
  file as ANSI, which turns an em-dash's trailing byte into U+201D — a character
  PowerShell honours as a *string delimiter*. One mis-decoded dash silently
  swallows the rest of a line, the closing brace after it, and the script.
- Function parameters may not be named `$Pid`, `$Host`, `$Error`, `$Input` or
  anything else PowerShell has already claimed. `$PID` is read-only and binding
  to it throws at call time, not at parse time — so it survives every check that
  does not actually run the function.

**Privilege**

- The daemon runs unprivileged. Device access comes from the udev rule, not from
  running as root.

**Build**

- Dependencies are pinned exactly, and the flash partition table is stated
  explicitly — a platform bump must not be able to relocate LittleFS out from
  under stored profiles.
- `ARDUINO_USB_MODE` must be `0` (TinyUSB/HID), removed via `build_unflags`
  before being redefined. At `1` the device enumerates as CDC serial and the
  daemon never finds it.
- Cross-task flags shared with the TinyUSB task are `volatile`.

---

## Building

### Firmware build

```
cd firmware && pio run          # build
pio run -t upload               # flash firmware
pio run -t uploadfs             # flash the web UI to LittleFS
```

Everything is pinned exactly — these are the versions the firmware was built and
hardware-verified against, not floors. A caret range only promises the build will
not break; it does not promise two people building the same commit get the same
binary.

| Dependency | Pinned |
|---|---|
| `platform = espressif32` | 6.13.0 |
| `adafruit/Adafruit SSD1306` | 2.5.17 |
| `adafruit/Adafruit GFX Library` | 1.12.6 |
| `crankyoldgit/IRremoteESP8266` | 2.9.0 |
| `bblanchon/ArduinoJson` | 7.4.3 |

Plus ESP32 built-ins: `WiFi`, `WebServer`, `LittleFS`.

ArduinoJson matters most — the firmware uses the **v7** API (`JsonDocument`,
`arr.add<JsonObject>()`), which does not compile against v6.

`board_build.partitions = default.csv` is stated rather than inherited. It is the
table the board definition already selects, so it changes nothing today — that is
the point.

Current size: 903,337 bytes flash (68.9%), 58,344 bytes RAM (17.8%).

### Daemon — Linux

C++17. `CMakeLists.txt` defaults to Release and enables `-Wall -Wextra` (`/W4` on
MSVC); the tree is clean under them.

```
cmake -B daemon/build -S daemon
cmake --build daemon/build
```

**Install from source.** The daemon runs as a dedicated unprivileged user; the
udev rule is what makes that possible.

```
# One-time: create the service account
sudo groupadd --system esp32ir
sudo useradd --system --no-create-home --shell /usr/sbin/nologin -g esp32ir esp32ir

# Install binary, udev rule and unit file (default prefix /usr/local)
sudo cmake --install daemon/build

sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=hidraw --action=add
sudo systemctl daemon-reload
sudo systemctl enable --now esp32-ir-remote
```

`--action=add` matters. udev evaluates rules on device **add** events only, so a
reload alone leaves an already-connected device with the ownership it was created
with. Replugging the ESP32 does the same thing.

Verify with the device plugged in — its node should be group `esp32ir`:
```
ls -l /dev/hidraw*
systemctl status esp32-ir-remote
journalctl -u esp32-ir-remote -f
```

### NixOS

The project builds with plain CMake and PlatformIO like anywhere else — the flake
only supplies the toolchain, it is not a second build system. Non-Nix users can
ignore `flake.nix` entirely.

```
nix develop                                  # cmake, gcc, hidapi, sdbus-cpp_2, platformio
cmake -B daemon/build -S daemon && cmake --build daemon/build
cd firmware && pio run
```

Three NixOS-specific points, all of which will otherwise waste an afternoon:

- **`sdbus-cpp_2`, not `sdbus-cpp`.** The unsuffixed attribute is still 1.x,
  which the daemon cannot build against.
- **Do not run `nix develop` from a directory whose path contains a space.** The
  shell sets `out=$PWD/outputs/out` and puts `-rpath $out/lib` into `NIX_LDFLAGS`
  unquoted, so `ld` splits on the space and the compiler probe fails at
  `project()` with `cannot find IR`. This repository's own directory name
  (`ESP32 IR remote`) triggers it. Invoke by flake reference from a clean path:
  `cd /tmp/work && nix develop "/home/niall/Projects/ESP32 IR remote"`.
  `nix build` is unaffected — it builds in the store, where paths are clean.
- **`platformio`, not `platformio-core`.** PlatformIO downloads a prebuilt
  `xtensa-esp32s3-elf` toolchain linked against `/lib64/ld-linux-x86-64.so.2`,
  which does not exist on NixOS. The `platformio` attribute is FHS-wrapped
  (bubblewrap) and handles this; the bare core package does not. The downloaded
  toolchain binaries also cannot run outside that wrapper, so `nm`, `gdb` and
  friends must be invoked from inside `nix develop` too.

The manual install steps do not apply here — `useradd`/`groupadd` do not survive
a `nixos-rebuild`. Use the module:

```nix
# flake.nix
inputs.esp32-ir-remote.url = "github:Niall-xox/ESP32-IR-CEC-Alternative";

# configuration.nix
imports = [ inputs.esp32-ir-remote.nixosModules.default ];
services.esp32-ir-remote.enable = true;
```

This declares the user and group, installs the udev rule, and defines the service
with the same hardening as the unit file. The package derivation calls the
project's own `CMakeLists.txt` rather than reimplementing the build, so it cannot
drift from what other distributions get. The `.service` file installed alongside
is unused here, because the module declares the unit natively.

### Windows

Requires Visual Studio Build Tools + vcpkg + hidapi. PowerShell throughout —
these use a backtick for line continuation, not a backslash.

```powershell
# --override matters: BuildTools on its own installs the installer and no
# compiler. VCTools is the C++ workload.
winget install Microsoft.VisualStudio.2022.BuildTools --override `
      "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install Kitware.CMake
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg install hidapi:x64-windows
```

```powershell
cmake -B daemon/build -S daemon -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build daemon/build --config Release
```

The binary lands at `daemon\build\Release\esp32-ir-daemon.exe` with `hidapi.dll`
beside it, copied there by vcpkg. **The two travel together.** The service runs
the binary from wherever it was registered, so moving the exe without the DLL
produces a service that installs and then fails to start.

Check the device before installing anything:

```powershell
.\daemon\build\Release\esp32-ir-daemon.exe --console
```

One `ON`, one ACK, then it exits — see [what `--console`
does](#where-the-implementation-departs-from-the-plan).

Then install, from an **elevated** shell. Windows client defaults to an
execution policy of `Restricted`, so the bypass is required rather than
optional; `-Scope Process` keeps it to that shell:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\daemon\packaging\windows\install-service.ps1 `
    -BinaryPath .\daemon\build\Release\esp32-ir-daemon.exe
```

Plug the ESP32 in **before** installing — the script disables USB selective
suspend on the device instance, which it can only do for a device that is
present. Replug once afterwards for that to take effect. Then follow the log:

```powershell
Get-Content C:\ProgramData\ESP32IRRemote\daemon.log -Wait -Tail 30 -Encoding UTF8
```

`-Encoding UTF8` is needed under Windows PowerShell 5.1, which otherwise
assumes the ANSI code page and mangles every em-dash. PowerShell 7 defaults to
UTF-8 and does not need it.

**Both `.ps1` files carry a UTF-8 BOM and have to keep it.** That is the same
mis-decode applied to the scripts rather than the log, and there it is fatal
rather than ugly: 5.1 reads a BOM-less UTF-8 file as ANSI, an em-dash's trailing
byte becomes `"` (U+201D), PowerShell treats that as a string delimiter, and the
file stops parsing — see [defect 1](#the-first-install--four-defects). Any editor
that helpfully re-saves these without a BOM breaks both scripts on every Windows
client machine, and the error it produces names a line that is not the problem.

The installer prints the opening log lines when it finishes, which is where the
capability report and the first display-state reading appear — the two things
every later step is read against.

**Re-running it is an upgrade.** It stops and removes the existing service
first, waiting for the deletion to actually land, so reinstalling after a
rebuild needs no manual cleanup. To remove it:

```powershell
.\daemon\packaging\windows\install-service.ps1 -Uninstall
.\daemon\packaging\windows\install-service.ps1 -Uninstall -RemoveLogs
```

Uninstalling also clears the selective-suspend registry values it wrote, so a
machine is not left carrying a setting nothing on it explains. The log directory
survives unless `-RemoveLogs` is given, because an uninstall during testing is
usually the prelude to a reinstall.

**Capture the machine's state before and after testing it:**

```powershell
.\daemon\packaging\windows\verify-windows.ps1
```

It changes nothing. It reads the power model, the service configuration, the
device state, the selective-suspend values and the log tail, and writes them in
a fixed order to `%ProgramData%\ESP32IRRemote\verify-<machine>-<stamp>.txt`.
Run it elevated — the service configuration and the device registry values are
not readable otherwise, and it says which parts it could not see rather than
reporting them as absent.

That fixed order is the point: the three configurations are being tested on
three machines on three different days, and that only adds up to one
verification record if each machine reports itself the same way.

hidapi comes from vcpkg via `find_package(hidapi CONFIG)`. PkgConfig is not used
here — it is guarded behind the Linux branch. The vcpkg target is
`hidapi::winapi` (not `hidapi::hidapi`), and the include path needs the *parent*
of the vcpkg include directory, derived at configure time via
`cmake_path(GET ... PARENT_PATH ...)`.

`PowrProf` is linked, for `CallNtPowerInformation` — the call that reports
whether this machine is S3 or Modern Standby and whether Fast Startup is on. It
ships with every Windows SDK under a name that has not changed, so it is linked
normally.

Nothing else is. `SHGetKnownFolderPath` was the one call that would have needed
another library, and it was dropped: `FOLDERID_ProgramData` is declared by the
SDK but defined in a library whose availability varies by SDK version, which is
a link error found by linking and never by compiling. The log path reads
`%ProgramData%` from the environment instead — a variable every service
inherits, and not user-redirectable the way per-user known folders are. The
power-setting and device-interface GUIDs are spelled out in the sources for the
same reason.

### Packaging & CI

`daemon/CMakeLists.txt` carries `install()` rules and a CPack configuration, so
one build feeds every distribution:

| Target | Artifact | Built by |
|---|---|---|
| Debian / Ubuntu | `.deb` | CPack, in a `debian:trixie` container |
| Fedora | `.rpm` | CPack, in a `fedora:rawhide` container |
| Arch | `PKGBUILD` | the user's own machine (`makepkg -si`) |
| NixOS | flake module | `nix build` |

`.github/workflows/packages.yml` does two jobs. It compile-builds **both**
daemons — `windows-build` under MSVC with hidapi from vcpkg, `linux-build` in a
Debian container — on every push to `main` and on every pull request, producing
no artifact: the point is to fail loudly when the half of the tree nobody builds
locally stops building. Separately, on a `v*` tag, it builds the deb and rpm and
attaches them to the release. Containers are required there because CPack shells
out to `dpkg-deb` and `rpmbuild`, and because the base image must actually ship
sdbus-c++ 2.x.

The firmware is built by neither. See
[Process](#process) in the known issues.

`packaging/postinst` and `prerm` are shared by both packages. They avoid reading
`$1` where possible, since dpkg passes `configure`/`remove` and rpm passes
`1`/`0`; `prerm` matches both spellings in one `case` so it can tell a real
removal from the removal half of an upgrade. `PKGBUILD` uses a separate
`.install` file and deliberately does *not* auto-enable the service, per Arch
policy.

Install destinations are **relative** (`lib/udev/rules.d`, not
`${CMAKE_INSTALL_PREFIX}/lib/...`) so CPack can rebase them onto its staging
directory; an absolute destination writes to the real `/usr` and fails the
package build. They are also literal `lib` rather than `${CMAKE_INSTALL_LIBDIR}`,
which resolves to `lib64` on Fedora — where neither udev nor systemd would ever
read the files.

Two things to know before shipping any of this:

- **Do not release packages until the VID/PID are real.** The udev rule matches
  `1234:5678`. On a machine you cannot inspect, that could chown an unrelated
  device to `esp32ir`, and `hid_open` takes the first match — so the daemon could
  open something else entirely. Packaging is precisely the mechanism that puts
  that rule on strangers' machines. **The workflow fires on any `v*` tag push**,
  and tags `v1.0` and `v1.1` already exist.
- CPack output suits a GitHub releases page, not the official Debian or Fedora
  archives, which want debhelper and `.spec` sources.

### The sdbus-c++ constraint

**sdbus-c++ 2.0 or newer is a hard requirement.** The v2 API is not
source-compatible with v1, and `CMakeLists.txt` enforces the minimum so a v1
system fails at configure time with a legible version message rather than a wall
of template errors.

This is the single biggest portability constraint, and it rules out two of the
most likely distributions outright — being "up to date" is not enough:

| Distribution | Ships | Builds |
|---|---|---|
| Arch | 2.3.1 | yes |
| Debian 13 (trixie) | 2.1.0 | yes |
| NixOS (`sdbus-cpp_2`) | 2.2.1 | yes |
| Fedora 44 / rawhide | 2.2.0 | yes |
| **Ubuntu 24.04 LTS** | **1.4.0** | no |
| **Fedora 42** | **1.5.0** | no |

Those users must build sdbus-c++ 2.x from source first (needs `libsystemd-dev`):

```
git clone --branch v2.1.0 https://github.com/Kistler-Group/sdbus-cpp.git
cmake -B sdbus-build -S sdbus-cpp -DCMAKE_BUILD_TYPE=Release -DSDBUSCPP_BUILD_CODEGEN=OFF
cmake --build sdbus-build && sudo cmake --install sdbus-build && sudo ldconfig
```

Two ways out, neither done:

- **Static-link sdbus-c++** (`-DBUILD_SHARED_LIBS=OFF`). Remaining dynamic
  dependencies would be `libsystemd`, `libudev` and libc — present on any systemd
  distribution by definition. Turns "supported on Arch and Debian testing" into
  "supported on Linux".
- **Switch to `sd-bus` from libsystemd** — a C API present on every systemd
  system, no third-party dependency at all. Contained to
  `LinuxPowerMonitor.h/.cpp` plus two lines of `CMakeLists.txt`, since `main.cpp`
  only touches `IPowerMonitor`.

---

## Known issues and deferred work

### Blocking release

| Issue | Notes |
|---|---|
| Placeholder VID/PID | `1234:5678` are common hobbyist defaults, so another device could collide — `hid_open` takes the first match. Must be changed in **five** files together; see [USB device identity](#usb-device-identity). Five hand-kept copies is itself the argument for generating them from one source. |
| Samsung / Sony / TCL / Hisense codes are `0x0` | No longer dangerous — the device reports these honestly — but the profiles are non-functional until real discrete codes are found. |
| No LICENSE or README | Both `PKGBUILD` and the RPM declare MIT while the repository ships no licence text. |

### Security / robustness

| Issue | Notes |
|---|---|
| WiFi AP password hardcoded (`irremote123`) | Bounded — config mode is user-initiated and transient. Better: a per-device password derived from the chip ID, shown on the OLED beside the IP. |
| `onNotFound` serves any file on LittleFS | `/profiles.json` and `/settings.json` are readable by anyone on the AP. Harmless today; a real leak the moment anything sensitive is stored. Fix with a whitelist or a `/www` prefix. |
| POST body size is unbounded | `server.arg("plain")` buffers the whole request before any handler runs, so the 32-profile cap bounds the *list*, not the allocation preceding it. Bounded in practice by the AP password and by ArduinoJson returning `NoMemory` cleanly. A real fix needs a custom body handler. |
| No firmware watchdog | Nothing feeds a task WDT on a device meant to sit powered continuously. A hung loop stays hung until it is unplugged. **The highest-priority item in this table**, and it is here rather than under [Blocking release](#blocking-release) only because it needs hardware time rather than a decision. The device's entire value is being more reliable than the network-based alternatives it exists to replace; a silent hang that persists until somebody notices the TV has stopped following the PC is precisely the failure those alternatives are criticised for. It is also the one defect in the project with no diagnostic at all — the daemon sees a write time out and reports honestly, but nothing anywhere says *why*. Sizing the timeout needs care: `loop()` performs blocking IR sends and serves the web UI, so the WDT has to tolerate the longest legitimate one. |
| Cancelled shutdown is not handled | `PrepareForShutdown(false)` does nothing, so after a *cancelled* scheduled shutdown the delay inhibitor is not re-taken and the next sleep or shutdown goes undelayed. Bounded to that one event — the wake path re-takes unconditionally, so a sleep/wake cycle restores the lock. Deliberate, and narrower than it sounds: reaching the state at all needs the cancel to land inside the sub-second window between the shutdown beginning and the daemon releasing its lock. Cancelling during the scheduled wait beforehand does nothing, because no signal has fired yet. Judged not worth the code. |
| No protocol version handshake | Deliberate while pre-release — see [Version lockstep is deliberate](#version-lockstep-is-deliberate). A mismatch is detected and logged distinctly, but not negotiated. Revisit at the first packaged release that reaches someone else. |

### Windows implementation

Still nothing to list here, and the reason has changed twice. The first install
on 2026-08-26 produced four *observed* failures rather than guesses, and all
four were fixed the same day — recorded as
[what the first install found](#the-first-install--four-defects) rather than as
a backlog. Two days of testing after that produced **no further defects at
all**: every remaining row on the Modern Standby machine passed on the first
attempt, and what the testing changed was the brief rather than the code.

What is left unverified is one machine class rather than a list of behaviours.
An S3 suspend driving the OFF, and a true cold boot, have never run — see
[Resume here](#resume-here). Both live in
[Windows — in progress](#windows--in-progress) with the
[verification plan](#verification-plan), which is now complete but for those two
rows.

Listing guesses here as though they were defects would put the two kinds of
claim in the same table, which is the habit the
[Invariants](#invariants) section exists to break.

### Process

| Issue | Notes |
|---|---|
| CI does not build the firmware | `packages.yml` builds both daemons on every push to `main` and on pull requests, so a daemon that stops compiling is caught immediately. The **firmware** is built by nothing — a change that breaks it is found by flashing, or not at all. |
| No release guard | Nothing stops a `v*` tag publishing packages while the VID/PID are still placeholders. |
| The VID/PID is documented rather than enforced | It appears in five files in three different spellings, and the brief carries a table asking people to keep them in sync. The count has gone three → four → five without anyone deciding it should, and the fifth was added by the same session that was auditing the document for exactly this kind of drift. A build step generating the C++ header, the udev rule and the two PowerShell defaults from one source would delete the table rather than maintain it. |
| The brief needs auditing to stay true | A 2026-08-26 review of the whole document found roughly eight stale claims — a section describing code as "never compiled" that had been building for two days, a CI note describing triggers that had changed, a plan written in the future tense for work already done. None were careless; they are the running cost of prose being the source of truth. Worth watching rather than fixing: when a fact is cheap to enforce (the VID/PID above) it should be enforced, and when it is a decision and its reasoning it belongs here. The failure mode is documenting things that could have been checked. |

### Verified on hardware — second pass

Flashed 2026-08-24 (firmware and filesystem both written and hash-verified), and
exercised against the real daemon on NixOS:

| Path | Evidence |
|---|---|
| udev rule on hotplug | node came up `root:esp32ir` `0660` automatically after a power cycle |
| Device retry at boot | `ESP32 not found at startup — will retry when needed` → `HID device opened` 2s later |
| Sequenced protocol | every `ON`/`OFF` ACKed with correlation active, both halves current |
| Sleep | `[event] Going to sleep` → `OFF sent and ACK received` → `Lock released` |
| Wake | `[event] Woke up` → `ON sent and ACK received` → `Lock acquired` |
| Shutdown | `[event] Shutting down` → `OFF sent and ACK received` → `Lock released`, *then* systemd stopped the unit — the inhibitor held the poweroff off until the ESP32 confirmed |
| Boot | `ON sent and ACK received (startup)` on a real boot |
| Uptime gate | `ON skipped — service restarted on an already-running system` on `systemctl restart`, and the ON above on a genuine boot — proven both ways |
| Unconfigured profile guard | Samsung selected (codes still `0x0`): OLED showed `Not Configured` and nothing was transmitted, rather than a meaningless frame answered with `ACK` |

#### Re-verified after the 2026-08-26 flash

The firmware changed underneath a platform that had already been verified, so
the sleep and wake rows above were re-run on the new build. Both behave exactly
as before:

```
13:41:41  [event] Going to sleep
13:41:41  [transport] HID device opened (VID=1234 PID=5678)
13:41:41  [cmd] OFF sent and ACK received (sleep)
13:41:41  [inhibitor] Lock released
13:41:58  [event] Woke up
13:41:58  [cmd] ON sent and ACK received (wake)
13:41:58  [inhibitor] Lock acquired
```

Worth noting in its own right: the lazy open at 13:41:41 succeeded on a device
that had been idle since 13:39, and the wake write went through that same
descriptor on its first attempt. No regression from either the firmware change
or the daemon's new retry budget.

What did **not** happen is covered under
[what Linux actually does across a suspend](#what-linux-actually-does-across-a-suspend).

### Still unverified on hardware

1. Boot with `display_always_on` enabled; status screen should be up from boot
   rather than after the first press.
2. Factory reset and profile deletion through the web UI.

### Accepted untested

**Corrupt `/profiles.json` recovery.** The path exists — `begin()` rewrites the
defaults when a load yields no profiles, and `getActive()` falls back to a
built-in profile regardless — but exercising it means building and flashing a
deliberately corrupt LittleFS image, since no API writes arbitrary files.

Judged not worth the effort: storage corruption is rare, atomic length-checked
writes are what made it rare, and the recovery from a device that did get stuck
is a reflash that takes under a minute. Revisit only if it is ever seen in the
field.

---

## History

The device was built in three stages: CDC serial with a fixed delay, then USB HID
with ACK-based lock release and Windows support, then profiles, the button, WiFi
config mode and the web UI.

Two hardening passes followed. The first found twelve issues, and the pattern was
that the worst were not crashes but **failures that reported themselves as
successes** — an unplugged ESP32 looked identical to a working one, and log
output never reached the journal at all.

The second pass found nine more, and the observation that came out of it is the
one worth keeping: **three of the first pass's own fixes had closed the reported
instance without closing the class it belonged to.** Fix 1 made a failed command
report honestly, but left the daemon unable to tell one command's reply from
another's. Fix 2 made a profile write atomic, but not complete. Fix 5 corrected
the ordering of a save, but not the equal case in a delete. Each was a correct
fix to the bug in front of it, and each left the door open one inch further down.

That is what the [Invariants](#invariants) section exists to prevent — it states
the rule rather than the incident, so the next change has something to violate
rather than a story to read.

The Windows bring-up is the third chapter, and it produced a different lesson.
The plan was written against a single machine and would have failed on the two
configurations that are more common — in both cases by reporting a *plausible
wrong reason* rather than an error, which is the same failure shape as the first
hardening pass in a new place. `GetTickCount64()` would have logged "service
restarted on an already-running system" on every Fast Startup boot, and it would
have been believed.

It also found a real firmware defect that neither Linux nor a code review would
have surfaced: the device does not come back from a USB suspend. What identified
it was noticing the failure tracked *idle time* rather than any command.
Diagnostics were the whole difference — "Write failed" alone supported four
different stories equally, and three of them were wrong.

The fourth chapter is this document. Prose has been the source of truth
throughout, and by the time the Windows plan was complete it was long enough
that keeping it internally consistent had quietly become the standard it was
held to. Two passes were run over it: one checking the settled decisions against
each other, which found three places a sound principle had been applied past
where it held, and one checking them against the [Overview](#overview) table —
the four rows the device exists to deliver — which found two more. The second
question turned out to be the harder one, and the reason is worth keeping: a
document can be entirely self-consistent and still have stopped describing the
product it started as. "Display off turns the TV off" followed impeccably from
every decision above it, and contradicted the first table in the file.

The fifth chapter was running it. After four chapters of increasingly careful
reasoning about a service nobody had ever started, starting it once found
[four defects in about forty minutes](#the-first-install--four-defects) — and
not one of them was reachable by any further amount of reading. Two were in
PowerShell, which nothing in this project compiles; one was a CRT open mode
whose failure is invisible to the process committing it; one was a race that had
been losing perhaps a third of the time and would have read as "the service
sometimes doesn't come back" for however long it took to become suspicious.

The lesson is not that the reasoning was wasted — the design it produced started
correctly on the first machine and reported itself accurately about a power
model it had never seen. It is that the four passes had been auditing the half
of the system written in C++ and reviewed against a plan, while the defects were
all in the seam where that half meets a platform: file encodings, reserved
variable names, sharing modes, and the gap between what the SCM says and what
the OS has actually finished doing. Prose can be checked against prose
indefinitely and will never report any of them.

The commit history has the detail: `7afd6bb` for the first pass, `23f3bd3` for
the second, `cc15d74` for the Windows rewrite, `a243f68` for the USB suspend
fix, `3d8f37b` for the three power models, and `f0fed26` for the first three
reversals.
