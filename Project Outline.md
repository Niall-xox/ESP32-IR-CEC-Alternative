# ESP32 IR Remote — Project Outline

## Overview

A USB-connected device that synchronises a TV's power state with a connected PC,
using the TV's built-in IR remote compatibility. This replicates the behaviour of
HDMI-CEC without requiring CEC support on either device.

The device mirrors PC power events to the TV:

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

## Hardware

- **ESP32-S3 SuperMini** — microcontroller running the firmware
- **IR LED** — transmits IR signals to the TV (GPIO 4)
- **OLED display (128×32, SSD1306)** — provides status feedback (SDA: GPIO 2, SCL: GPIO 3)
- **Tactile button** — profile cycling, config mode, factory reset (GPIO 5, GND)

**On the board name.** PlatformIO has no board definition for the ESP32-S3
SuperMini, so `platformio.ini` builds for `lolin_s3_mini` as a close-enough
match — same ESP32-S3 target, and the pin assignments above are set explicitly
in the firmware rather than inherited from the board definition. This is
deliberate, not an oversight. The CAD directory likewise models the housing
around an `ESP32-S3_Zero.step`, another similarly-sized S3 board used as a
stand-in.

---

## Architecture

The system has two components:

### PC Daemon
A background process that monitors system power events and sends commands to the
ESP32 over USB. Written in C++17 for cross-platform portability.

**Power event detection (Linux):**
- Uses the **systemd-logind D-Bus API** — the standard mechanism used by GNOME,
  KDE, and other commercial Linux software.
- Subscribes to `PrepareForSleep` and `PrepareForShutdown` signals on
  `org.freedesktop.login1`.
- Takes a **delay inhibitor lock** before sleep/shutdown to guarantee the IR
  command is transmitted before the system suspends or powers off. The lock is
  released only after the ESP32 sends ACK, confirming the IR signal has fired.
- The lock is re-acquired whenever the machine stays up: after a resume
  (`PrepareForSleep(false)`) **and** after a cancelled shutdown
  (`PrepareForShutdown(false)`). Both paths matter — missing either leaves the
  daemon permanently unlocked, so every later event proceeds without waiting.
- Startup detection: handled by the systemd service starting at boot, gated on
  system uptime so a restart is not mistaken for a boot (see below).
- Wake detection: handled by `PrepareForSleep(false)` — emitted after resume.
  No lock is needed on wake as the system is already running.

**Startup ON is gated on uptime.** The service starting only implies a boot if
the machine actually just booted. `Restart=on-failure`, a package upgrade and a
manual `systemctl restart` all start the service too, and each of those used to
turn the TV on — including under a user who had deliberately switched it off
while leaving the PC running. The daemon reads `/proc/uptime` and skips the ON
if the system has been up longer than 180 seconds. The window is deliberately
generous: a slow machine that misses it merely fails to turn the TV on, which
the user can undo with their remote, whereas a tight window would make that the
common case.

### ESP32 Firmware
Receives commands from the PC over USB HID, transmits the appropriate IR signal
for the active manufacturer profile, updates the OLED display, and sends ACK or
ERR back to the daemon.

---

## Communication Protocol

### Phase 1 (complete) — One-way, fixed delay
- Transport: USB CDC Serial at 115200 baud
- Daemon → ESP32 commands: `ON\n`, `OFF\n`
- No response from ESP32
- Daemon waited a fixed 500ms delay before releasing the inhibitor lock.

### Phase 2 (current) — Two-way, ACK-based, sequence-correlated
Phase 3 added profiles, the button and WiFi config on top of this without
changing the protocol. The sequence byte was added afterwards, in the second bug
fix pass — see fix 13.

- Transport: USB HID (64-byte vendor-defined reports)
- Both directions share one 64-byte payload layout:

| Byte | Daemon → ESP32 | ESP32 → Daemon |
|------|----------------|----------------|
| `[0]` | sequence, 1–255, never 0 | the same sequence, echoed unchanged |
| `[1..]` | NUL-terminated command | NUL-terminated response |

| Command (Daemon → ESP32) | Response (ESP32 → Daemon) | Action |
|--------------------------|---------------------------|--------|
| `ON`  | `ACK` | Fire IR ON code for active profile |
| `OFF` | `ACK` | Fire IR OFF code for active profile |
| `ON` / `OFF`, code unset | `ERR` | Active profile's code for that direction is `0x0` — nothing transmitted |
| unknown | `ERR` | Unknown command received |

- IR dispatch is synchronous — ACK is sent only after the IR signal has fully
  transmitted, so the daemon releases its inhibitor lock the instant it receives ACK.
- The sequence byte is echoed, never interpreted. It exists so the daemon can
  tell a reply to *this* command from a late reply to an earlier one; the ESP32
  needs no state to support it.
- The daemon discards any reply whose sequence does not match what it sent, and
  keeps waiting for the right one until its budget expires.
- No fixed delays anywhere in the pipeline.
- Communication is daemon-initiated only. The ESP32 responds to commands but
  does not send unsolicited reports.

**Firmware and daemon must be updated together.** A daemon speaking the
sequenced protocol to pre-fix-13 firmware sees every reply as unmatched and logs
`stale reply, or firmware too old for the sequenced protocol` rather than
failing silently.

### Timing budget

`send()` runs inside a sleep or shutdown handler while a logind delay inhibitor
is held, and logind stops waiting after `InhibitDelayMaxSec` — **5 seconds by
default**. Every step of a send therefore draws on one shared 4-second budget:
device open, reopen-after-write-failure, and the ACK wait. Nothing in the
transport can outlive the lock it is delaying things for. `SEND_BUDGET` in
`HIDTransport.cpp` is the single knob; raising it above 5s reintroduces the
problem regardless of what the individual timeouts say.

### USB Device Identity
The ESP32 presents as a vendor-defined HID device identified by VID/PID.
The daemon finds the device by these IDs — no port numbers or paths involved.

| Field | Value | Notes |
|-------|-------|-------|
| VID   | `0x1234` | Placeholder — replace with registered ID before commercial release |
| PID   | `0x5678` | Placeholder — replace with registered ID before commercial release |
| Product name | `ESP32 IR Remote` | Shown in system device list |
| Manufacturer | `ESP32-IR-CEC` | Shown in system device list |

For open-source release, a free registered VID/PID pair can be obtained from
[pid.codes](https://pid.codes). For commercial release, a USB-IF VID is required.

---

## Manufacturer Profiles

IR codes are stored per-profile in `/profiles.json` on LittleFS. Each profile contains:
- `name` — display name (e.g. `LG`, `Samsung`)
- `protocol` — IR protocol (e.g. `NEC`, `SAMSUNG`, `SONY`)
- `on_code` — discrete power on code
- `off_code` — discrete power off code
- `visible` — whether the profile appears in the button cycle

Discrete on/off codes are preferred over toggle codes, as they guarantee the
correct TV state regardless of any prior state drift.

### `0x0` means "not configured"

A code of `0x00000000` marks a manufacturer whose discrete codes have not been
confirmed yet. This is enforced, not just a convention:

- The firmware refuses to transmit and answers `ERR`; the OLED shows
  `Not Configured`.
- The daemon logs `FAILED — no ACK, TV state not changed`.
- The web UI shows a `No IR code` badge on any profile with an unset code.

Previously such a profile transmitted a meaningless frame and still answered
`ACK`, so every layer reported success for an IR command that could not possibly
have worked. Filling in real codes needs no code change — the guard simply stops
firing.

### Default profiles (hardcoded in firmware, written to LittleFS on first boot)

| Profile  | Protocol | ON Code      | OFF Code     | Notes |
|----------|----------|--------------|--------------|-------|
| LG       | NEC      | `0x20DF23DC` | `0x20DFA35C` | Confirmed working on LG C2 |
| Samsung  | SAMSUNG  | placeholder  | placeholder  | |
| Sony     | SONY     | placeholder  | placeholder  | |
| TCL      | NEC      | placeholder  | placeholder  | |
| Hisense  | NEC      | placeholder  | placeholder  | |

Note: `0x20DFB34C` was tested as an LG ON code but triggers a smart TV network
prompt on the C2 — it maps to a smart home power-on mode, not plain power-on.

Custom profiles can be added via the web UI and are also stored in `/profiles.json`.
Factory reset rewrites `/profiles.json` from the hardcoded defaults above.

---

## Button & Display Behaviour

### Button (GPIO 5)
- Press defined as release within 300ms of press
- Hold defined as button held beyond 300ms

The thresholds (300ms / 5s / 8s / 23s) live in `HoldTimings.h` and are shared by
`Button`, which decides what a hold means, and `Display`, which draws bars
spanning the same windows. They were previously declared separately in each.

### At Boot

The status screen is drawn as soon as the display and profiles are up:

- **Display always on enabled:** status screen, stays on.
- **Display always on disabled:** status screen for 2 seconds as a boot
  confirmation, then off.

The OLED previously stayed blank from boot until the first button press or IR
command, which contradicted the always-on setting outright.

### Normal Operation — Display Always On Disabled (default)

**Press 1 (display off):**
- Display turns on showing:
  ```
  Profile: LG
  ```
- 2 second timer starts, display turns off after

**Press 2+ (display on, within timer):**
- Cycle to next visible profile, reset 2 second timer

**IR command received:**
- Display turns on showing `TV On` or `TV Off`
- Turns off after 2 seconds

### Normal Operation — Display Always On Enabled

**Display:** OLED on continuously showing:
```
Profile: LG
```

**Press 1:**
- Show status screen

**Press 2+:**
- Cycle to next visible profile

**IR command received:**
- Display shows `TV On` or `TV Off`
- After 2 seconds returns to status screen

### Hold Behaviour (both normal operation modes)

- **300ms:** progress bar appears at bottom, text above:
  ```
  Enter Wireless Config Mode?
  [▓▓░░░]  ← 5 blocks, 1 per second
  ```
- **Released before 5s:** status screen shown, next press cycles
- **5s:** bar full → text changes to:
  ```
  Release To Enter
  Wireless Config!
  ```
- **Released at 5s–8s:** enter wireless config mode, status screen shown
- **Held past 5s, at 8s:** factory reset screen:
  ```
  Hold To Factory Reset
  [▓▓▓░░░░░░░░░░░░░░░░░]  ← 15 segments, 1 per second
  ```
- **Released during reset bar (8s–23s):** status screen shown, next press cycles
- **Held to 23s:** factory reset triggers automatically, status screen shown

In all cases where the hold is cancelled or completes, the display shows the status screen and the next press cycles profiles.

### Wireless Config Mode Active

**Display:** OLED on continuously (always, cannot be disabled):
```
Profile: LG
WiFi: Active
```

**Press 1:**
- Show status screen

**Press 2+:**
- Display shows for 2 seconds:
  ```
  Hold Button to Exit
  Wireless Mode to
  Switch Profiles
  ```
- Reverts to status screen

**Hold behaviour:**
- **300ms:** progress bar appears, text:
  ```
  Exit Wireless Config Mode?
  [▓▓░░░]  ← 5 blocks, 1 per second
  ```
- **Released before 5s:** status screen shown, next press cycles
- **5s:** bar full → text changes to:
  ```
  Release To Exit
  Wireless Config!
  ```
- **Released at 5s–8s:** exit wireless config mode, return to normal operation
- **Held past 5s, at 8s:** factory reset screen, same as above
- **Held to 23s:** factory reset triggers automatically

**Exit via web UI:**
- `Save and Exit` button → exits wireless config mode, returns to normal operation

---

## Web UI (accessed via `192.168.4.1` when wireless config mode is active)

Hosted on LittleFS alongside `/profiles.json` and `/settings.json`.

### Profile Management
- View all profiles
- Select active profile
- Add new profile (name, protocol, ON code, OFF code)
- Edit existing profile
- Delete profile — deleting the *active* profile falls back to the preceding
  entry and says which profile is now active, rather than letting whichever
  profile slid into the freed index become active unannounced
- Show/hide profile from button cycle
- `No IR code` badge on any profile whose ON or OFF code is `0x0`

### Validation
Saving is refused, with the offending profile named, if any profile has an empty
name or a code that is not hex (`0x20DF23DC` or a bare `20DF23DC`, up to 32
bits). The firmware parses an unusable code as `0x0` and treats that as
unconfigured, so a typo cannot corrupt anything — but it would produce a profile
that silently declines to transmit, and the browser is where the user can still
see what they typed.

The device caps the profile list at 32 (`Profiles::MAX_PROFILES`), enforced both
when loading `/profiles.json` and on `POST /api/profiles`, so neither a corrupt
file nor a request over the AP can grow the list until the heap runs out.

### Settings
- Display always on toggle (applies to normal operation only)

### Actions
- **Save** — saves current settings without exiting wireless mode
- **Restore** — reverts to last saved config (discards unsaved changes)
- **Factory Reset** — rewrites all profiles and settings from hardcoded defaults
- **Save and Exit** — saves and exits wireless config mode

---

## Storage (LittleFS)

### `/profiles.json`
Array of profile objects. Written from hardcoded defaults on first boot.
```json
[
  { "name": "LG", "protocol": "NEC", "on": "0x20DF23DC", "off": "0x20DFA35C", "visible": true },
  ...
]
```

### `/settings.json`
Device settings. Written from hardcoded defaults on first boot.
```json
{
  "active_profile": 0,
  "display_always_on": false
}
```

Factory reset rewrites both files from hardcoded defaults.

Both files are written atomically — serialised to `<name>.tmp`, then renamed over
the destination — so an interrupted write cannot leave a truncated file behind.
A stray `.tmp` file after a power loss is harmless and is overwritten by the next
save.

The temp file is only promoted if the bytes written match `measureJson()`. The
rename makes the *replacement* atomic; it does nothing about whether the new
contents are complete, and a full or failing flash returns a partial write with
a non-zero count. Checking the length is what stops a truncated document being
renamed over a good one.

---

## Daemon — Cross-Platform Design

The daemon is structured to support multiple platforms without rewriting core
logic. Two platform-specific concerns are isolated behind abstract interfaces:

```
IPowerMonitor   — raises OnSleep, OnWake, OnShutdown events
ITransport      — bool send(const std::string& cmd)
```

`main.cpp` only interacts with these interfaces. Platform implementations are
compiled in or out by CMake based on the target OS.

`send()` returns whether the device confirmed the command, and must never throw
— it is called from power-event handlers, where an exception escapes into the
caller's event loop, and a `false` must not stop the system sleeping or shutting
down. Both implementations honour that, and both bound themselves to the same
send budget. The transport seam is also what makes the CDC path viable: swapping
`HIDTransport` for `archive/SerialTransport` in `main.cpp` is the entire change
needed to support a board that cannot present as USB HID.

### Platform implementation map

| Concern       | Linux                      | Windows                          |
|---------------|----------------------------|----------------------------------|
| Power events  | sdbus-c++ / systemd-logind | Win32 Service API                |
| HID transport | hidapi (hidraw backend)    | hidapi (Win32 backend)           |
| Build system  | CMake                      | CMake                            |

Note: hidapi is cross-platform — the same `HIDTransport` implementation works on
both Linux and Windows. Only `IPowerMonitor` needs a platform-specific implementation.

### Build system
CMake is used as the build system. It generates the appropriate native build
files per platform (Makefiles on Linux, Visual Studio/Ninja on Windows) from a
single `CMakeLists.txt`. Platform-specific source files and dependencies are
included or excluded via CMake conditionals.

On Windows, hidapi is provided via **vcpkg** and found with `find_package(hidapi CONFIG)`.
PkgConfig is not used on Windows — it is guarded behind the Linux branch.
The vcpkg target is `hidapi::winapi` (not `hidapi::hidapi`).
The include path requires the parent of the vcpkg include directory, derived at
configure time via `cmake_path(GET ... PARENT_PATH ...)`.

#### Linux build
```
cmake -B daemon/build -S daemon
cmake --build daemon/build
```

#### Linux install (from source)
The daemon runs as a dedicated unprivileged user. The udev rule is what makes
that possible — without it `/dev/hidraw*` is root-only and the daemon cannot
open the device.

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
reload alone leaves an already-connected device with the ownership it was
created with. Replugging the ESP32 does the same thing.

Verify with the device plugged in — the ESP32's node should be group `esp32ir`:
```
ls -l /dev/hidraw*
systemctl status esp32-ir-remote
journalctl -u esp32-ir-remote -f
```

#### Packaging
`daemon/CMakeLists.txt` carries `install()` rules and a CPack configuration, so
the same build feeds every distribution:

| Target | Artifact | Built by |
|---|---|---|
| Debian / Ubuntu | `.deb` | CPack, in a `debian:trixie` container |
| Fedora | `.rpm` | CPack, in a `fedora:rawhide` container |
| Arch | `PKGBUILD` | the user's own machine (`makepkg -si`) |
| NixOS | flake module | `nix build` |

CI (`.github/workflows/packages.yml`) builds the deb and rpm on a `v*` tag and
attaches them to the release. Containers are required because CPack shells out
to `dpkg-deb` and `rpmbuild`, and because the base image must be one that
actually ships sdbus-c++ 2.x — see the dependency note below.

`packaging/postinst` and `packaging/prerm` are shared by both packages. They
avoid reading `$1` where possible, since dpkg passes `configure`/`remove` and
rpm passes `1`/`0`; `prerm` matches both spellings in one `case` so it can tell
a real removal from the removal half of an upgrade. `packaging/PKGBUILD` uses a
separate `.install` file and deliberately does *not* auto-enable the service,
per Arch policy.

Two things to know before shipping any of this:

- **Do not release packages until the VID/PID are real.** The udev rule matches
  `1234:5678`. On a machine you cannot inspect, that could chown an unrelated
  device to `esp32ir`, and `hid_open` takes the first match — so the daemon
  could open something else entirely. Packaging is precisely the mechanism that
  puts that rule on strangers' machines.
- CPack output suits a GitHub releases page, not the official Debian or Fedora
  archives, which want debhelper and `.spec` sources.

Destinations are **relative** (`lib/udev/rules.d`, not
`${CMAKE_INSTALL_PREFIX}/lib/udev/rules.d`) so CPack can rebase them onto its
staging directory; an absolute destination writes to the real `/usr` and fails
the package build. They are also literal `lib` rather than
`${CMAKE_INSTALL_LIBDIR}`, which resolves to `lib64` on Fedora — where neither
udev nor systemd would ever read the files.

#### NixOS
The project builds with plain CMake and PlatformIO like anywhere else — the
flake only supplies the toolchain, it is not a second build system. Non-Nix
users can ignore `flake.nix` entirely.

```
nix develop                                  # cmake, gcc, hidapi, sdbus-cpp_2, platformio
cmake -B daemon/build -S daemon && cmake --build daemon/build
cd firmware && pio run
```

Three NixOS-specific points, all of which will otherwise waste an afternoon:

- **`sdbus-cpp_2`, not `sdbus-cpp`.** The unsuffixed attribute is still 1.x,
  which the daemon cannot build against.
- **Do not run `nix develop` from a directory whose path contains a space.**
  The shell sets `out=$PWD/outputs/out` and puts `-rpath $out/lib` into
  `NIX_LDFLAGS` unquoted, so `ld` splits on the space and the compiler probe
  fails at `project()` with `cannot find IR`. This repository's own directory
  name (`ESP32 IR remote`) triggers it. Invoke the shell by flake reference from
  a path without spaces instead:
  `cd /tmp/work && nix develop "/home/niall/Projects/ESP32 IR remote"`.
  `nix build` is unaffected — it builds in the store, where paths are clean.
- **`platformio`, not `platformio-core`.** PlatformIO downloads a prebuilt
  `xtensa-esp32s3-elf` toolchain linked against `/lib64/ld-linux-x86-64.so.2`,
  which does not exist on NixOS. The `platformio` attribute is FHS-wrapped
  (bubblewrap) and handles this; the bare core package does not. The downloaded
  toolchain binaries also cannot be run outside that wrapper, so `nm`, `gdb` and
  friends must be invoked from inside `nix develop` too.

The manual install steps above do not apply on NixOS — `useradd`/`groupadd` do
not survive a `nixos-rebuild`, and `/usr/local/bin` is not how services are
deployed. Use the module instead:

```nix
# flake.nix
inputs.esp32-ir-remote.url = "github:Niall-xox/ESP32-IR-CEC-Alternative";

# configuration.nix
imports = [ inputs.esp32-ir-remote.nixosModules.default ];
services.esp32-ir-remote.enable = true;
```

This declares the `esp32ir` user and group, installs the udev rule, and defines
the service with the same hardening as the unit file. The package derivation
calls the project's own `CMakeLists.txt` rather than reimplementing the build,
so it cannot drift from what other distributions get — and since the CMakeLists
gained `install()` rules it has no `installPhase` either, so the file layout is
shared too rather than reimplemented in Nix. The `.service` file installed
alongside is unused on NixOS, because the module declares the unit natively.

#### Windows build (requires Visual Studio Build Tools + vcpkg + hidapi)
```
cmake -B daemon/build -S daemon -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build daemon/build --config Release
```

#### Windows prerequisites (one-time)
```
winget install Microsoft.VisualStudio.2022.BuildTools
winget install Kitware.CMake
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg install hidapi:x64-windows
```

### Language standard
The daemon targets **C++17**, which is fully supported by GCC, Clang, and MSVC.

---

## File Structure

```
firmware/
  platformio.ini
  data/
    index.html                   (Web UI served from LittleFS in WiFi mode)
  src/
    main.cpp
    Button.h / .cpp
    Display.h / .cpp
    Profiles.h / .cpp
    HoldTimings.h                (button thresholds, shared by Button and Display)

daemon/
  CMakeLists.txt                 (build + install() rules + CPack config)
  VERSION                        (single source of the version — read by CMake and flake.nix)
  esp32-ir-remote.service.in     (unit template — CMake substitutes the bin path)
  99-esp32-ir-remote.rules       (udev rule — lets the daemon run unprivileged)
  src/
    main.cpp
    IPowerMonitor.h
    ITransport.h
    HIDTransport.h / .cpp
    LinuxPowerMonitor.h / .cpp
    WindowsPowerMonitor.h / .cpp
  packaging/
    postinst                     (deb + rpm: create account, reload udev, enable unit)
    prerm                        (deb + rpm: stop unit on removal, not on upgrade)
    PKGBUILD                     (Arch — builds from source on the user's machine)
    esp32-ir-remote.install      (Arch pacman hooks)
  archive/
    SerialTransport.h / .cpp     (USB CDC — not compiled, the supported path for non-HID boards)

flake.nix                        (Nix dev shell, daemon package, NixOS module — optional, ignored elsewhere)
.github/workflows/packages.yml   (builds .deb and .rpm on a v* tag, attaches to the release)

3D modeling/
  ESP32-Remote-Housing.FCStd     (FreeCAD source — the editable master for the housing)
  *.STEP / *.step                (component models: board, IR transmitter, OLED, button)
                                 STL exports are not tracked — re-export from the FCStd

DEPRECATED Component test/       (pre-Phase 1 bring-up: IR LED, OLED and upload toolchain)
HID test/                        (pre-Phase 2 spike: firmware + PC halves of a HID echo test)
```

Both test directories predate the code they proved out and are kept for
reference only — neither is built or flashed as part of the project.

### Firmware file summaries

| File | Purpose |
|------|---------|
| `platformio.ini` | PlatformIO build config. ESP32-S3 target, TinyUSB (HID mode), LittleFS filesystem. Platform, libraries and partition table are pinned exactly. |
| `main.cpp` | Entry point. USB HID device setup, IR dispatch, button callbacks, WiFi/web server lifecycle. Refuses to transmit an unset (`0x0`) code and answers `ERR`. |
| `Button.h/.cpp` | Non-blocking button input with press/hold detection and debouncing. Fires callbacks at the `HoldTimings` thresholds. |
| `Display.h/.cpp` | OLED state management. Handles status, IR confirm, "Not Configured", hold/reset bars, WiFi lock message with timer-based expiry. |
| `Profiles.h/.cpp` | Manufacturer IR profile + settings storage on LittleFS. Loads/saves JSON, factory reset, profile cycle helper. Writes are atomic (temp file + rename) *and* length-checked; `getActive()` falls back to a built-in profile if storage is unavailable; `toJson`/`fromJson` are the only places profiles cross the JSON boundary. |
| `HoldTimings.h` | The button hold thresholds, shared by `Button` and `Display` so a bar cannot fill over a different window than the callback fires on. |
| `data/index.html` | Single-page web UI served in WiFi mode. Profile CRUD, settings, factory reset, save & exit. Validates names and hex codes before saving. |

### Daemon file summaries

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration. C++17, hidapi on all platforms, sdbus-c++ on Linux. Defaults to a Release build, enables `-Wall -Wextra` (`/W4` on MSVC), and reads the version from `VERSION`. |
| `VERSION` | The version number, in one place. `CMakeLists.txt` and `flake.nix` both read it; `packaging/PKGBUILD` is the one copy that must be updated by hand, because makepkg needs a literal before it has fetched anything. |
| `esp32-ir-remote.service.in` | systemd unit template. Starts at boot after `dbus.socket` and logind, restarts on failure. Runs unprivileged as the `esp32ir` user with sandboxing applied. CMake substitutes the binary path so the unit is correct under both `/usr/local` and `/usr`. |
| `99-esp32-ir-remote.rules` | udev rule granting the `esp32ir` group access to the device's `/dev/hidraw` node. Required for the daemon to run without root. |
| `ITransport.h` | Abstract interface: `send(cmd)` returning true only once the device has confirmed the command. Isolates transport so `main.cpp` is unaffected by Serial → HID swap. |
| `IPowerMonitor.h` | Abstract interface: callbacks for sleep/wake/shutdown + `run()`. Isolates OS-specific event handling. |
| `HIDTransport.h/.cpp` | `ITransport` implementation. Finds ESP32 by VID/PID via hidapi, sends 64-byte reports, blocks until the ACK *for that command* is received — replies carrying another sequence are discarded. Drains stale reports before each write. Returns false if the device is missing, the write fails, the ACK times out, or the device replies ERR. Reopens the device automatically if a write fails (e.g. after wake or replug). All of it inside one 4s budget. Single-threaded — no background polling. Unchanged on both platforms. |
| `archive/SerialTransport.h/.cpp` | `ITransport` implementation for USB CDC Serial. Not compiled; the supported path for boards that cannot present as HID. One-way, so `true` means "bytes written", not "IR fired" — a weaker guarantee than HID, and the header says so. Honours the same send budget and returns false rather than throwing. |
| `LinuxPowerMonitor.h/.cpp` | `IPowerMonitor` implementation for Linux. D-Bus via sdbus-c++, manages the inhibitor lock, releases after ACK, re-acquires on resume and on cancelled shutdown. Lock acquisition never throws — it runs inside signal handlers. |
| `WindowsPowerMonitor.h/.cpp` | `IPowerMonitor` implementation for Windows. Win32 Service API, blocks in control handler until ACK received. Incomplete — see "Known and deferred". |
| `main.cpp` | Entry point. Constructs transport and platform-appropriate power monitor, wires callbacks, sends ON at startup *if the system just booted*, runs event loop. |

---

## Roadmap

### Phase 1 — Minimum Viable Product ✓
- Firmware: USB CDC Serial, receives ON/OFF, fires IR, updates OLED ✓
- Daemon: D-Bus logind listener, fixed-delay inhibitor lock release ✓
- Confirmed working: sleep, wake, shutdown ✓

### Phase 2 — USB HID + Cross Platform ✓
- USB HID communication with ACK-based lock release ✓
- Device discovery by VID/PID — no hardcoded port numbers ✓
- Firmware: sends ACK after IR confirmed transmitted ✓
- Daemon installed as systemd service — starts at boot, restarts on failure ✓
- Graceful handling when ESP32 is unplugged ✓
- Confirmed working: sleep, wake, shutdown, boot on Linux and Windows ✓
- Windows daemon support confirmed working ✓

### Phase 3 — Multi-Profile, Button, WiFi Config ✓
- Physical button (GPIO 5): profile cycling, config mode, factory reset ✓
- Multi-manufacturer IR profile support stored on LittleFS as JSON ✓
- Display always-on setting ✓
- Factory reset via button hold ✓
- WiFi AP config mode (`ESP32-IR-Remote` / `irremote123`) at `192.168.4.1` ✓
- Web UI for profile management and settings ✓
- Factory reset via web UI (also exits WiFi mode) ✓
- OLED shows AP IP address in WiFi mode ✓
- Web UI save triggers immediate OLED refresh ✓

### Hardening passes ✓ (code) / partial (hardware)
- First bug fix pass — 12 fixes, verified on hardware ✓
- Second bug fix pass — 9 fixes plus redundancy removal, compile-verified;
  hardware verification outstanding (see Verification status)

---

## Dependencies

### Firmware (PlatformIO)

Pinned exactly in `platformio.ini` — these are the versions the firmware was
built and hardware-verified against, not floors. A caret range only promises the
build will not break; it does not promise two people building the same commit
get the same binary.

| Dependency | Pinned |
|---|---|
| `platform = espressif32` | 6.13.0 |
| `adafruit/Adafruit SSD1306` | 2.5.17 |
| `adafruit/Adafruit GFX Library` | 1.12.6 |
| `crankyoldgit/IRremoteESP8266` | 2.9.0 |
| `bblanchon/ArduinoJson` | 7.4.3 |

Also: ESP32 built-ins `WiFi`, `WebServer`, `LittleFS`.

ArduinoJson is the pin that matters most — the firmware uses the **v7** API
(`JsonDocument`, `arr.add<JsonObject>()`), which does not compile against v6.

`board_build.partitions = default.csv` is stated explicitly rather than
inherited. It is the table the board definition already selects, so it changes
nothing today — that is the point. Left implicit, the flash layout is whatever
the platform happens to default to, and a platform bump that moved the
filesystem offset would relocate LittleFS out from under a device that already
has profiles stored in it. Bump any of these deliberately, then re-verify on
hardware.

### PC Daemon
- **Build:** CMake
- **All platforms:** `hidapi` — USB HID communication
- **Linux:** `sdbus-c++` **2.0 or newer** — D-Bus communication with systemd-logind
- **Windows:** Win32 API only, no extra dependencies beyond hidapi

The sdbus-c++ major version is a hard requirement: the v2 API is not
source-compatible with v1, and `CMakeLists.txt` enforces the minimum so a v1
system fails at configure time with a clear version message rather than a wall
of template errors.

This is currently the single biggest portability constraint, and it rules out
two of the most likely distributions outright — being "up to date" is not
enough, because their current stable releases still carry 1.x:

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

The cleaner long-term answer is to **static-link sdbus-c++ into the daemon**
(`-DBUILD_SHARED_LIBS=OFF`). The remaining dynamic dependencies would then be
`libsystemd`, `libudev` and libc — present on any systemd distribution by
definition — which turns "supported on Arch and Debian testing" into "supported
on Linux" and makes a single portable binary viable. Not yet done.

If broad compatibility with older distributions ever outweighs the current
implementation's readability, the alternative is `sd-bus` from libsystemd: a C
API present on every systemd system by definition, with no third-party
dependency at all. That swap is contained to `LinuxPowerMonitor.h/.cpp` plus two
lines of `CMakeLists.txt`, since `main.cpp` only ever touches the
`IPowerMonitor` interface.

---

## Open Questions
- Confirm working discrete IR codes for Samsung, Sony, TCL, Hisense
- Assign registered VID/PID before any public or commercial release

---

## Bug Fixes

Reviewed after Phase 3. The architecture held up — no redesign was needed — but
robustness gaps were found at the edges: corrupt input, a missing device, unusual
OS events. The worst were not crashes but **failures that reported themselves as
successes**, which send debugging in entirely the wrong direction.

### First pass — fixes 1–12

| # | Issue | Cause → fix | Files |
|---|---|---|---|
| 1 | Daemon logged ACK for commands that never happened | `ITransport::send()` returned `void`, so an unplugged ESP32, a failed write and an expired 2s ACK timeout all logged as success. `send()` now returns `bool`; failures log `FAILED — no ACK, TV state not changed`. A false return still never blocks sleep or shutdown. | `ITransport.h`, `HIDTransport.*`, daemon `main.cpp`, `archive/SerialTransport.*` |
| 2 | Corrupt profile storage caused a reboot loop | Saves opened the destination with `"w"`, truncating it immediately, and every profile cycle saves — so a power loss mid-write corrupted the file. `getActive()` then indexed an empty vector and crashed on the next button press or IR command. Now `writeJsonAtomic()` writes to `.tmp` and renames; `getActive()` falls back to a built-in profile and range-checks the index; `begin()` restores defaults if a load yields nothing, and `setup()` checks its return value. | `Profiles.*`, firmware `main.cpp` |
| 3 | Missing JSON fields passed `nullptr` to `strtoul` (undefined behaviour) | `as<const char*>()` yields `nullptr` for an absent or non-string key. Parsing is now centralised in `Profiles::fromJson()`, which null-checks and defaults to `0x0`. This also removed a duplicated copy of the parse logic, so LittleFS loads and web POSTs can no longer drift apart. | `Profiles.*`, firmware `main.cpp` |
| 4 | Successful factory reset reported as failed | The reset also exits WiFi mode, so the page's follow-up `load()` could never complete and showed a red error with the AP already gone. Reload removed; the page now reports success and warns that WiFi is shutting down. | `index.html` |
| 5 | Saving could select the wrong profile | Profiles and settings were POSTed concurrently, so `/api/settings` could range-check `active_profile` against the pre-delete list. Now sequential, profiles first. `save()` returns a bool and `Save & Exit` no longer exits on a failed save, which previously discarded the changes. | `index.html` |
| 6 | HID receive flag not `volatile` | `received_` is written on the TinyUSB task and read by `loop()`; the compiler was free to cache it and never observe the change. `rxBuf_` itself is safe — the protocol is strictly request/response. | firmware `main.cpp` |
| 7 | Progress bars redrew the full screen every loop | `onHold` fires every iteration, pushing a whole 128×32 I²C frame for a bar with only 5 or 15 states. `Display` now caches bar kind and fill level and skips unchanged redraws — roughly 20 per 23s hold instead of thousands. | `Display.*` |
| 8 | Daemon ran as root | Only `/dev/hidraw*` access needed privilege. `99-esp32-ir-remote.rules` grants the `esp32ir` group access to that one device; the unit now runs unprivileged with sandboxing. Relax `ProtectSystem` first if D-Bus ever becomes unreachable. | `.service`, `.rules` |
| 9 | Build reproducibility | Nothing was version-pinned (the v7 ArduinoJson API will not compile against v6) — all dependencies now carry major-version constraints. CMake linked the bare `${HIDAPI_LIBRARIES}` name list, dropping the library search path; now uses `IMPORTED_TARGET` + `PkgConfig::HIDAPI`. Added a Release default. Replaced deprecated `containsKey()` with `is<T>()`. | `platformio.ini`, `CMakeLists.txt`, firmware `main.cpp` |
| 10 | Dead code | `Button::holdFired_`, `Button::configFired_` and the web UI's `settingsChanged` were write-only, and a comment describing `configFired_` was wrong. All removed. | `Button.*`, `index.html` |
| 12 | Daemon log output never reached the journal | `std::cout` is fully buffered whenever stdout is not a terminal — which is exactly the case under systemd, where it is a pipe to the journal. Log lines sat in a 4KB buffer (months of normal operation at this volume), and anything written just before systemd's SIGTERM was lost outright, including the shutdown `OFF` confirmation. Worse, `std::cerr` is unbuffered *and* tied to `std::cout`, so failures always appeared while successes silently vanished — the exact inverse of the problem fix 1 solved. `std::cout << std::unitbuf` in `main()`. Found by running the daemon: it produced no output at all until forced through `stdbuf -oL`. | daemon `main.cpp` |
| 11 | `ARDUINO_USB_MODE` defined twice | The `lolin_s3_mini` board definition hardcodes `=1` (CDC serial); `build_flags` appended `=0` without removing it, so correctness rested on `-D` ordering and the build emitted ~170 redefinition warnings that would hide any real one. Now removed via `build_unflags` first. Found during the first compile; the binary is byte-identical, so the previous ordering was already resolving correctly — but had it ever flipped, the device would have enumerated as CDC serial and the daemon would never have found it. | `platformio.ini` |

### Second pass — fixes 13–21

A review of the whole tree after the first pass. The theme this time was
narrower and more uncomfortable: **three of the first pass's own fixes had
closed the reported instance without closing the class it belonged to.** Fix 1
made a failed command report honestly but left the daemon unable to tell one
command's reply from another's; fix 2 made the write atomic but not complete;
fix 5 fixed the ordering of a save but not the equal case in a delete.

| # | Issue | Cause → fix | Files |
|---|---|---|---|
| 13 | A late ACK was accepted as the *next* command's reply | hidraw queues input reports. When a command timed out and its ACK arrived afterwards, the reply sat in the queue and satisfied the next command's read — so `send()` returned true, and the daemon logged "ACK received", for an IR signal that command never sent. Precisely the failure fix 1 existed to eliminate, one layer down. The protocol gained a sequence byte, echoed by the firmware; the daemon discards replies that do not match and drains the queue before every write. | `HIDTransport.*`, firmware `main.cpp` |
| 14 | A short write was renamed over good profile storage | `writeJsonAtomic()` only rejected a write of *zero* bytes. A full or failing flash returns a partial document with a non-zero count, which was then renamed into place — reintroducing fix 2's corruption through a narrower door. The rename made the replacement atomic; nothing made the contents complete. Now compared against `measureJson()`. | `Profiles.cpp` |
| 15 | A cancelled shutdown released the inhibitor lock forever | `PrepareForShutdown(false)` — emitted when a scheduled shutdown is cancelled — did nothing, while its `PrepareForSleep(false)` twin correctly re-acquired. The lock was released on the way into the shutdown and never taken again, so every later sleep and shutdown proceeded unblocked and the OFF raced the machine powering down. Silent, permanent, and indistinguishable from working until the TV stayed on. | `LinuxPowerMonitor.*` |
| 16 | `send()` could outlive the lock it was holding things up for | logind stops waiting after `InhibitDelayMaxSec` (5s default). The timeouts were independent rather than pooled: a failed write spent 5s reopening *before* the 2s ACK wait began — 7s of blocking to honour a 5s guarantee, with the system suspending out from under it. Every step now draws on one 4s budget. | `HIDTransport.*`, `archive/SerialTransport.cpp` |
| 17 | "Display always on" was blank from boot | `setup()` called `display.begin()`, which turns the OLED *off*, and never drew a status screen. The screen stayed blank until the first button press or IR command — contradicting the one setting whose entire purpose is that it never is. Now drawn at boot; with the setting off it doubles as a 2s boot confirmation. | firmware `main.cpp` |
| 18 | Inhibitor acquisition could throw out of a D-Bus signal handler | `onPrepareForSleep` guarded the user callback with `try/catch` but not the re-acquire beneath it. A transient `Inhibit()` failure unwound into the sdbus event loop. `takeInhibitorLock()` now returns `bool` and never throws; the constructor treats a false return as fatal, the handlers log and continue. | `LinuxPowerMonitor.*` |
| 19 | Deleting the active profile silently activated another | `removeProfile()` handled indices above and below the deleted one but not the equal case, so the index stayed put and whichever profile slid into the freed slot became active unannounced. Now falls back to the preceding entry and says which profile it selected. | `index.html` |
| 20 | Startup `ON` fired on every service restart | The `ON` mirrors a boot, but `Restart=on-failure`, package upgrades and `systemctl restart` all start the service too. A daemon that crashed at 3am turned the TV on; so did an upgrade, under a user who had deliberately switched it off. Now gated on `/proc/uptime`. | daemon `main.cpp` |
| 21 | Unconfigured profiles reported success | A profile with `0x0` codes transmitted a meaningless frame and answered `ACK`, so firmware, daemon and web UI all reported a TV state change that could not have happened. The firmware now declines to transmit and answers `ERR`, the OLED shows `Not Configured`, and the web UI badges the profile. Previously deferred until real codes existed; it is the placeholder state that needed to be honest, not the codes. | `Profiles.*`, firmware `main.cpp`, `Display.*`, `index.html` |

### Second pass — redundancy and hardening

| Area | Change |
|---|---|
| Duplicated hold thresholds | `5000` / `8000` / `23000` were declared separately in `Button.h` and `Display.h`. Changing one without the other would have left bars filling over a different window than the callbacks fire on. Now `HoldTimings.h`. |
| Triplicated profile → JSON | The same object was hand-built in `saveProfiles()`, `buildDefaultProfilesDoc()` and the `GET /api/profiles` handler — three chances for the on-disk format and the web API to drift. Now one `Profiles::toJson()`, mirroring what fix 3 did for the parse direction. |
| Dead code | `Button::isHeld()` and `Button::heldMs()` were defined and never called. Removed. |
| Version in four places | `CMakeLists.txt`, `flake.nix` and `PKGBUILD` each carried their own `0.3.0`. Now `daemon/VERSION`, read by CMake and the flake. `PKGBUILD` still needs a literal — makepkg cannot read a version out of a source it has not fetched — and says so. |
| Warnings not enabled | Neither `-Wall` nor `-Wextra` was ever passed; the tree was clean by discipline rather than by construction. Now enabled in `CMakeLists.txt` (`/W4` on MSVC). Not `-Werror`: a future compiler inventing a warning should not stop someone building a release they need. |
| Boot race with the system bus | The unit ordered itself after logind but not `dbus.socket`, so at boot the daemon could lose the race, exit 1, and only recover on the next `Restart=` — turning the TV on seconds late. Ordered explicitly, in both the unit file and the NixOS module. |
| Unpinned firmware build | Caret ranges promise the build will not break, not that two people building the same commit get the same binary. Platform and all four libraries pinned exactly; `board_build.partitions` stated rather than inherited, so a platform bump cannot relocate LittleFS out from under stored profiles. |
| Unbounded profile list | `/profiles.json` is also reachable over the config AP. Capped at 32 on load and on POST. |
| Archived CDC transport | Threw `std::runtime_error` on timeout, which `ITransport` explicitly forbids — it runs inside a power-event handler — and retried for 10s, twice the inhibitor budget. Now returns false within the same 4s budget, and rejects short writes. Its header states plainly that `true` means "bytes written", not "IR fired". |

### Known and deferred

| Issue | Notes |
|---|---|
| Samsung / Sony / TCL / Hisense codes are `0x00000000` | Needs real discrete codes per manufacturer. No longer dangerous — fix 21 makes the device report these honestly — but the profiles remain non-functional until the codes are filled in. |
| WiFi AP password hardcoded (`irremote123`) | Bounded — config mode is user-initiated and transient. Better: a per-device password derived from the chip ID, shown on the OLED beside the IP. |
| `onNotFound` serves any file on LittleFS | `/profiles.json` and `/settings.json` are readable by anyone on the AP. Harmless today; a real leak the moment anything sensitive is stored. Fix with a whitelist or a `/www` prefix. |
| POST body size is unbounded | `server.arg("plain")` buffers the whole request before any handler runs, so the 32-profile cap bounds the *list*, not the allocation that precedes it. Bounded in practice by the AP password and by ArduinoJson returning `NoMemory` cleanly. A real fix needs a custom body handler. |
| No protocol version handshake | Firmware and daemon share a VID/PID and a three-byte vocabulary. A mismatch is now *detected* (fix 13 logs it distinctly) but not negotiated. Cheap to add now, expensive once units ship. |
| No firmware watchdog | Nothing feeds a task WDT on a device meant to sit powered continuously. A hung loop stays hung until it is unplugged. |
| CI does not build on ordinary commits | `packages.yml` runs on `v*` tags and manual dispatch only, and never builds the firmware. A broken tree is discovered at release time. |
| Windows: automatic wake leaves the TV off | Only `PBT_APMRESUMESUSPEND` is handled. Windows sends `PBT_APMRESUMEAUTOMATIC` on *every* resume and adds `RESUMESUSPEND` only for user-initiated ones, so Wake-on-LAN and scheduled wakes are missed. Needs both events plus a dedup flag. |
| Windows: stop event never signalled | `serviceMain` waits on an event nothing sets, so the process lingers holding the HID device open — an immediate service restart then fails to find the ESP32. |
| Windows: control handler blocks | Blocking inside `serviceCtrlHandler` works but is against the documented contract, which wants a prompt return and a worker thread. `SERVICE_ACCEPT_PRESHUTDOWN` also needs its timeout set explicitly rather than left at the default. |
| Placeholder VID/PID | See Open Questions. These are common hobbyist defaults, so another device could collide — `hid_open` takes the first match. |
| No LICENSE or README | Both `PKGBUILD` and the RPM declare MIT while the repository ships no licence text. To be written before release; the project is incomplete. |

The Windows items need a Windows machine to verify. Fixes 1, 13 and 16 all
change `ITransport` or `HIDTransport`, which the Windows build links against —
no Windows code changed, but the daemon needs rebuilding there, and the firmware
it talks to must be post-fix-13.

### Verification status

**Compiled — both halves, zero warnings.** After the second pass, that claim is
enforced rather than asserted: `-Wall -Wextra` is on in `CMakeLists.txt`.

- Daemon: builds against sdbus-c++ 2.2.1 and hidapi 0.15.0, links
  `libsdbus-c++.so.2`. CMake 4.3.4 accepts the `3.16` minimum. Clean under
  `-Wall -Wextra -Wpedantic`.
- Firmware: builds clean at 903,337 bytes flash (68.9%) and 58,344 bytes RAM
  (17.8%). Zero warnings across the whole build, framework included.
- `nix build .#esp32-ir-daemon` produces `esp32-ir-daemon-0.3.0`, confirming the
  version is being read from `daemon/VERSION` rather than restated.
- `archive/SerialTransport.cpp` syntax-checks clean under `-Wall -Wextra`
  despite not being in the build, so the CDC path cannot rot unnoticed.

**Flashed and verified on hardware.** Firmware and filesystem both written and
hash-verified; the device enumerates as `1234:5678 ESP32 IR Remote`.

| Fix | How it was verified |
|---|---|
| 1 (failure path) | With no device attached, the daemon logs `ON FAILED — no ACK, TV state not changed` where it previously claimed success. |
| 1 (success path) | Against the real device: `[cmd] ON sent and ACK received (startup)` — the ESP32 received the command, fired the IR and replied. |
| 2 | Button press shows `Profile: LG` — profiles loaded from a freshly formatted LittleFS instead of indexing an empty vector. |
| 7 | Hold bars animate smoothly with the redraw cache in place. |
| 8 | `[inhibitor] Lock acquired` succeeded as an unprivileged user against real logind — the part most likely to fail quietly. |
| 11 | Found by compiling; rebuilt binary is byte-identical. |
| 12 | Found by running; output now appears without `stdbuf`, and reaches the journal under systemd. |

**Deployed and verified on NixOS via the flake module.** The daemon runs as an
enabled system service under the unprivileged `esp32ir` user, the udev rule
grants it `/dev/hidraw*` (`root:esp32ir`, mode `0660`) automatically on hotplug,
and the journal shows the full startup path:

```
[transport] HID device opened (VID=1234 PID=5678)
[inhibitor] Lock acquired
[monitor] Connected to systemd-logind
[cmd] ON sent and ACK received (startup)
```

Note on udev: rules are evaluated when a device is **added**, so installing the
rule while the device is already plugged in does nothing until it is replugged
(or `udevadm trigger -s hidraw` is run). This is expected behaviour, not a
broken rule.

**Still unverified.** The first-pass items, plus everything from the second pass
— which is compile-verified on both halves but has not yet been flashed:

1. Sleep / wake / shutdown firing IR (needs a real suspend cycle and a reboot —
   boot and shutdown behaviour only exists once systemd owns the process).
2. Fix 2's recovery path — corrupt `/profiles.json` deliberately and confirm the
   device restores defaults rather than rebooting.
3. Fix 4 / 5 — factory reset and profile deletion through the web UI.
4. Fix 13 end to end — the daemon and firmware now speak the sequenced protocol
   and must be flashed and rebuilt **together**. Confirm a normal `ON`/`OFF`
   still ACKs before trusting anything else.
5. Fix 17 — reboot with `display_always_on` enabled and confirm the status
   screen is up from boot rather than after the first press.
6. Fix 20 — `systemctl restart esp32-ir-remote` on a machine that has been up a
   while should log `ON skipped`, and a real reboot should still log
   `ON sent and ACK received (startup)`.
7. Fix 21 — select Samsung (still `0x0`) and confirm the OLED shows
   `Not Configured` and the daemon logs a failure rather than an ACK.
8. Fix 15 — `shutdown -h +1` then `shutdown -c`, and confirm
   `[event] Shutdown cancelled` followed by `[inhibitor] Lock acquired`.
