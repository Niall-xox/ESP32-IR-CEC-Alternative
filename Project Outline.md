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
protocol, the uptime gate and the full sleep/wake/shutdown/boot cycle have all
been exercised against the real device and daemon. Five items remain untested,
none of them on the main power-sync path — see
[Still unverified on hardware](#still-unverified-on-hardware).

**Two things block any public release:**

1. Registered VID/PID — currently placeholder `1234:5678`.
2. Confirmed discrete IR codes for Samsung, Sony, TCL and Hisense.

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

These must match in three places: `firmware/src/main.cpp`, `daemon/src/main.cpp`
and `99-esp32-ir-remote.rules` (lowercase hex, no `0x`).

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
IPowerMonitor   — raises OnSleep, OnWake, OnShutdown events
ITransport      — bool send(const std::string& cmd)
```

`main.cpp` only ever touches these. Platform implementations are compiled in or
out by CMake.

| Concern       | Linux                      | Windows                          |
|---------------|----------------------------|----------------------------------|
| Power events  | sdbus-c++ / systemd-logind | Win32 Service API                |
| HID transport | hidapi (hidraw backend)    | hidapi (Win32 backend)           |
| Build system  | CMake                      | CMake                            |

hidapi is cross-platform, so `HIDTransport` is unchanged on both. Only
`IPowerMonitor` needs a platform-specific implementation.

The transport seam is also what makes the CDC path viable: swapping
`HIDTransport` for `archive/SerialTransport` in `main.cpp` is the entire change
needed to support a board that cannot present as USB HID (a classic ESP32, or an
ESP8266 behind a USB-UART bridge). That transport is one-way, so its `true`
means "bytes written", not "IR fired" — a weaker guarantee, and its header says
so.

### Windows status

Incomplete. The interface boundary is right and the transport is shared, so a
robust implementation is available later without touching anything else — but
three things are outstanding, all contained inside `WindowsPowerMonitor.cpp`.
See [Known issues](#known-issues-and-deferred-work).

### Daemon source map

```
daemon/
  CMakeLists.txt                 (build + install() rules + CPack config)
  VERSION                        (single source of the version)
  esp32-ir-remote.service.in     (unit template — CMake substitutes the bin path)
  99-esp32-ir-remote.rules       (udev rule — lets the daemon run unprivileged)
  src/
    main.cpp                     (wires transport + monitor, runs the event loop)
    ITransport.h                 (abstract: send(cmd) -> bool)
    IPowerMonitor.h              (abstract: sleep/wake/shutdown callbacks + run())
    HIDTransport.h / .cpp        (USB HID, sequence-correlated, budgeted)
    LinuxPowerMonitor.h / .cpp   (logind over D-Bus, inhibitor lock)
    WindowsPowerMonitor.h / .cpp (Win32 Service API — incomplete)
  packaging/
    postinst / prerm             (deb + rpm)
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

- `send()` must complete within logind's `InhibitDelayMaxSec` (5s default). One
  shared budget, currently 4s, covering open + reopen + ACK wait.

**Persistence**

- Writes to LittleFS are atomic *and* length-checked. Atomic replacement of
  incomplete content is still corruption.
- Any index into the profile list is range-checked at the point of use, not
  only at the point of writing.

**Inhibitor lock**

- Re-acquired on resume, ready for the next sleep.
- Acquisition never throws — it runs inside D-Bus signal handlers, where an
  exception unwinds into the sdbus event loop.
- A `false` from `send()` must never stop the system sleeping or shutting down.

**Process lifecycle**

- The startup `ON` mirrors a *boot*, not a process start.
- `std::cout` must be unbuffered (`std::unitbuf`). Under systemd stdout is a
  pipe, so it is fully buffered by default and log lines never reach the journal.

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

Requires Visual Studio Build Tools + vcpkg + hidapi.

```
winget install Microsoft.VisualStudio.2022.BuildTools
winget install Kitware.CMake
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg install hidapi:x64-windows
```

```
cmake -B daemon/build -S daemon -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build daemon/build --config Release
```

hidapi comes from vcpkg via `find_package(hidapi CONFIG)`. PkgConfig is not used
here — it is guarded behind the Linux branch. The vcpkg target is
`hidapi::winapi` (not `hidapi::hidapi`), and the include path needs the *parent*
of the vcpkg include directory, derived at configure time via
`cmake_path(GET ... PARENT_PATH ...)`.

### Packaging & CI

`daemon/CMakeLists.txt` carries `install()` rules and a CPack configuration, so
one build feeds every distribution:

| Target | Artifact | Built by |
|---|---|---|
| Debian / Ubuntu | `.deb` | CPack, in a `debian:trixie` container |
| Fedora | `.rpm` | CPack, in a `fedora:rawhide` container |
| Arch | `PKGBUILD` | the user's own machine (`makepkg -si`) |
| NixOS | flake module | `nix build` |

`.github/workflows/packages.yml` builds the deb and rpm on a `v*` tag and
attaches them to the release. Containers are required because CPack shells out to
`dpkg-deb` and `rpmbuild`, and because the base image must actually ship
sdbus-c++ 2.x.

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
| Placeholder VID/PID | `1234:5678` are common hobbyist defaults, so another device could collide — `hid_open` takes the first match. Must be changed in three files together. |
| Samsung / Sony / TCL / Hisense codes are `0x0` | No longer dangerous — the device reports these honestly — but the profiles are non-functional until real discrete codes are found. |
| No LICENSE or README | Both `PKGBUILD` and the RPM declare MIT while the repository ships no licence text. |

### Security / robustness

| Issue | Notes |
|---|---|
| WiFi AP password hardcoded (`irremote123`) | Bounded — config mode is user-initiated and transient. Better: a per-device password derived from the chip ID, shown on the OLED beside the IP. |
| `onNotFound` serves any file on LittleFS | `/profiles.json` and `/settings.json` are readable by anyone on the AP. Harmless today; a real leak the moment anything sensitive is stored. Fix with a whitelist or a `/www` prefix. |
| POST body size is unbounded | `server.arg("plain")` buffers the whole request before any handler runs, so the 32-profile cap bounds the *list*, not the allocation preceding it. Bounded in practice by the AP password and by ArduinoJson returning `NoMemory` cleanly. A real fix needs a custom body handler. |
| No firmware watchdog | Nothing feeds a task WDT on a device meant to sit powered continuously. A hung loop stays hung until it is unplugged. |
| Cancelled shutdown is not handled | `PrepareForShutdown(false)` does nothing, so after a *cancelled* scheduled shutdown the delay inhibitor is not re-taken and the next sleep or shutdown goes undelayed. Bounded to that one event — the wake path re-takes unconditionally, so a sleep/wake cycle restores the lock. Deliberate, and narrower than it sounds: reaching the state at all needs the cancel to land inside the sub-second window between the shutdown beginning and the daemon releasing its lock. Cancelling during the scheduled wait beforehand does nothing, because no signal has fired yet. Judged not worth the code. |
| No protocol version handshake | Deliberate while pre-release — see [Version lockstep is deliberate](#version-lockstep-is-deliberate). A mismatch is detected and logged distinctly, but not negotiated. Revisit at the first packaged release that reaches someone else. |

### Windows implementation

All three are contained inside `WindowsPowerMonitor.cpp` and need a Windows
machine to verify.

| Issue | Notes |
|---|---|
| Automatic wake leaves the TV off | Only `PBT_APMRESUMESUSPEND` is handled. Windows sends `PBT_APMRESUMEAUTOMATIC` on *every* resume and adds `RESUMESUSPEND` only for user-initiated ones, so Wake-on-LAN and scheduled wakes are missed. Needs both events plus a dedup flag. |
| Stop event never signalled | `serviceMain` waits on an event nothing sets, so the process lingers holding the HID device open — an immediate service restart then fails to find the ESP32. |
| Control handler blocks | Works, but against the documented contract, which wants a prompt return and a worker thread. `SERVICE_ACCEPT_PRESHUTDOWN` also needs its timeout set explicitly. |

The daemon needs rebuilding on Windows — `ITransport` and `HIDTransport` have
both changed — and the firmware it talks to must speak the sequenced protocol.

### Process

| Issue | Notes |
|---|---|
| CI does not build on ordinary commits | `packages.yml` runs on `v*` tags and manual dispatch only, and never builds the firmware. A broken tree is discovered at release time. |
| No release guard | Nothing stops a `v*` tag publishing packages while the VID/PID are still placeholders. |

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

### Still unverified on hardware

1. Select Samsung (still `0x0`); OLED should show `Not Configured` and the
   daemon should log `ERR`, not an ACK. Needs a power event to trigger a command.
2. Boot with `display_always_on` enabled; status screen should be up from boot
   rather than after the first press.
3. Factory reset and profile deletion through the web UI.
4. Corrupt `/profiles.json` deliberately; confirm defaults are restored rather
   than a reboot loop. The awkward one — no API writes arbitrary files, so it
   means flashing a deliberately corrupt LittleFS image.

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

The commit history has the detail: `7afd6bb` for the first pass, `23f3bd3` for
the second.
