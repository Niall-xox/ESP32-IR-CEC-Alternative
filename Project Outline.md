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
- Startup detection: handled by the systemd service starting at boot.
- Wake detection: handled by `PrepareForSleep(false)` — emitted after resume.
  No lock is needed on wake as the system is already running.

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

### Phase 2 (current) — Two-way, ACK-based
Still the current wire format. Phase 3 added profiles, the button and WiFi
config on top of it without changing the protocol.
- Transport: USB HID (64-byte vendor-defined reports)
- Daemon → ESP32: 64-byte output report, command string in first bytes
- ESP32 → Daemon: 64-byte input report, response string in first bytes

| Command (Daemon → ESP32) | Response (ESP32 → Daemon) | Action |
|--------------------------|---------------------------|--------|
| `ON`  | `ACK` | Fire IR ON code for active profile |
| `OFF` | `ACK` | Fire IR OFF code for active profile |
| unknown | `ERR` | Unknown command received |

- IR dispatch is synchronous — ACK is sent only after the IR signal has fully
  transmitted, so the daemon releases its inhibitor lock the instant it receives ACK.
- No fixed delays anywhere in the pipeline.
- Communication is daemon-initiated only. The ESP32 responds to commands but
  does not send unsolicited reports.

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
- Delete profile
- Show/hide profile from button cycle

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

---

## Daemon — Cross-Platform Design

The daemon is structured to support multiple platforms without rewriting core
logic. Two platform-specific concerns are isolated behind abstract interfaces:

```
IPowerMonitor   — raises OnSleep, OnWake, OnShutdown events
ITransport      — send(const std::string& cmd)
```

`main.cpp` only interacts with these interfaces. Platform implementations are
compiled in or out by CMake based on the target OS.

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

#### Linux install
The daemon runs as a dedicated unprivileged user. The udev rule is what makes
that possible — without it `/dev/hidraw*` is root-only and the daemon cannot
open the device.

```
# One-time: create the service user and grant it access to the device
sudo groupadd --system esp32ir
sudo useradd --system --no-create-home --shell /usr/sbin/nologin -g esp32ir esp32ir
sudo cp daemon/99-esp32-ir-remote.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# Install the binary and unit file
sudo install -m 755 daemon/build/esp32-ir-daemon /usr/local/bin/
sudo cp daemon/esp32-ir-remote.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now esp32-ir-remote
```

Verify with the device plugged in — the ESP32's node should be group `esp32ir`:
```
ls -l /dev/hidraw*
systemctl status esp32-ir-remote
journalctl -u esp32-ir-remote -f
```

#### NixOS
The project builds with plain CMake and PlatformIO like anywhere else — the
flake only supplies the toolchain, it is not a second build system. Non-Nix
users can ignore `flake.nix` entirely.

```
nix develop                                  # cmake, gcc, hidapi, sdbus-cpp_2, platformio
cmake -B daemon/build -S daemon && cmake --build daemon/build
cd firmware && pio run
```

Two NixOS-specific points, both of which will otherwise waste an afternoon:

- **`sdbus-cpp_2`, not `sdbus-cpp`.** The unsuffixed attribute is still 1.x,
  which the daemon cannot build against.
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
so it cannot drift from what other distributions get.

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

daemon/
  CMakeLists.txt
  esp32-ir-remote.service
  99-esp32-ir-remote.rules       (udev rule — lets the daemon run unprivileged)
  src/
    main.cpp
    IPowerMonitor.h
    ITransport.h
    HIDTransport.h / .cpp
    LinuxPowerMonitor.h / .cpp
    WindowsPowerMonitor.h / .cpp
  archive/
    SerialTransport.h / .cpp     (Phase 1 — not compiled, retained for reuse on non-HID devices)

flake.nix                        (Nix dev shell, daemon package, NixOS module — optional, ignored elsewhere)

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
| `platformio.ini` | PlatformIO build config. ESP32-S3 target, TinyUSB (HID mode), LittleFS filesystem. |
| `main.cpp` | Entry point. USB HID device setup, IR dispatch, button callbacks, WiFi/web server lifecycle. |
| `Button.h/.cpp` | Non-blocking button input with press/hold detection and debouncing. Fires callbacks at 300ms/5s/8s/23s thresholds. |
| `Display.h/.cpp` | OLED state management. Handles status, IR confirm, hold/reset bars, WiFi lock message with timer-based expiry. |
| `Profiles.h/.cpp` | Manufacturer IR profile + settings storage on LittleFS. Loads/saves JSON, factory reset, profile cycle helper. Writes are atomic (temp file + rename) and `getActive()` falls back to a built-in profile if storage is unavailable. |
| `data/index.html` | Single-page web UI served in WiFi mode. Profile CRUD, settings, factory reset, save & exit. |

### Daemon file summaries

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration. C++17, hidapi on all platforms, sdbus-c++ on Linux. Defaults to a Release build. |
| `esp32-ir-remote.service` | systemd unit file. Starts at boot after logind, restarts on failure. Runs unprivileged as the `esp32ir` user with sandboxing applied. |
| `99-esp32-ir-remote.rules` | udev rule granting the `esp32ir` group access to the device's `/dev/hidraw` node. Required for the daemon to run without root. |
| `ITransport.h` | Abstract interface: `send(cmd)` returning true only once the device has confirmed the command. Isolates transport so `main.cpp` is unaffected by Serial → HID swap. |
| `IPowerMonitor.h` | Abstract interface: callbacks for sleep/wake/shutdown + `run()`. Isolates OS-specific event handling. |
| `HIDTransport.h/.cpp` | `ITransport` implementation. Finds ESP32 by VID/PID via hidapi, sends 64-byte reports, blocks until ACK received. Returns false if the device is missing, the write fails, the ACK times out, or the device replies ERR. Reopens device automatically if write fails (e.g. after wake or replug). Single-threaded — no background polling. Unchanged on both platforms. |
| `archive/SerialTransport.h/.cpp` | `ITransport` implementation for USB CDC Serial (Phase 1). Not compiled, retained for reuse on non-HID-capable devices. |
| `LinuxPowerMonitor.h/.cpp` | `IPowerMonitor` implementation for Linux. D-Bus via sdbus-c++, manages inhibitor lock, releases after ACK. |
| `WindowsPowerMonitor.h/.cpp` | `IPowerMonitor` implementation for Windows. Win32 Service API, blocks in control handler until ACK received. |
| `main.cpp` | Entry point. Constructs transport and platform-appropriate power monitor, wires callbacks, sends ON at startup, runs event loop. |

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

---

## Dependencies

### Firmware (PlatformIO)
- `adafruit/Adafruit SSD1306`
- `adafruit/Adafruit GFX Library`
- `crankyoldgit/IRremoteESP8266`
- `bblanchon/ArduinoJson` — JSON read/write for LittleFS profiles and settings.
  The firmware uses the **v7** API (`JsonDocument`, `arr.add<JsonObject>()`),
  which does not compile against v6.
- ESP32 built-ins: `WiFi`, `WebServer`, `LittleFS`, `Preferences`

All dependencies carry a major-version constraint in `platformio.ini` so a later
build cannot silently pull in a breaking release. These are floors, not exact
pins — after the first successful build, run `pio pkg list` and replace each
range with the version it actually resolved to make the build reproducible.

### PC Daemon
- **Build:** CMake
- **All platforms:** `hidapi` — USB HID communication
- **Linux:** `sdbus-c++` **2.0 or newer** — D-Bus communication with systemd-logind
- **Windows:** Win32 API only, no extra dependencies beyond hidapi

The sdbus-c++ major version is a hard requirement: the v2 API is not
source-compatible with v1, and `CMakeLists.txt` enforces the minimum so a v1
system fails at configure time with a clear version message rather than a wall
of template errors. v2 is recent enough that distributions frozen before
mid-2024 — notably older LTS releases — may still package 1.x, which would need
building the library from source.

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

### Fixed

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

### Known and deferred

| Issue | Notes |
|---|---|
| Samsung / Sony / TCL / Hisense codes are `0x00000000` | Needs real discrete codes per manufacturer. These profiles are `visible: true`, so they can be selected, transmit a meaningless frame **and still return ACK** — everything the device reports says success. Make the firmware treat `0x0` as unconfigured (reply `ERR`, show "Not configured") when the codes are added. |
| WiFi AP password hardcoded (`irremote123`) | Bounded — config mode is user-initiated and transient. Better: a per-device password derived from the chip ID, shown on the OLED beside the IP. |
| `onNotFound` serves any file on LittleFS | `/profiles.json` and `/settings.json` are readable by anyone on the AP. Harmless today; a real leak the moment anything sensitive is stored. Fix with a whitelist or a `/www` prefix. |
| Windows: automatic wake leaves the TV off | Only `PBT_APMRESUMESUSPEND` is handled. Windows sends `PBT_APMRESUMEAUTOMATIC` on *every* resume and adds `RESUMESUSPEND` only for user-initiated ones, so Wake-on-LAN and scheduled wakes are missed. Needs both events plus a dedup flag. |
| Windows: stop event never signalled | `serviceMain` waits on an event nothing sets, so the process lingers holding the HID device open — an immediate service restart then fails to find the ESP32. |
| Placeholder VID/PID | See Open Questions. These are common hobbyist defaults, so another device could collide — `hid_open` takes the first match. |

Both Windows items need a Windows machine to verify. Fix 1 changes `ITransport`,
which the Windows build links against — no Windows code changed, but the daemon
still needs rebuilding there.

### Verification status

**Compiled — both halves, zero warnings.**

- Daemon: builds against sdbus-c++ 2.2.1 and hidapi 0.15.0, links
  `libsdbus-c++.so.2`. CMake 4.3.4 accepts the `3.16` minimum.
- Firmware: builds clean at 902,657 bytes flash (68.9%) and 58,344 bytes RAM
  (17.8%).

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

**Still unverified:**

1. Sleep / wake / shutdown firing IR (needs a real suspend cycle and a reboot —
   boot and shutdown behaviour only exists once systemd owns the process).
2. Fix 2's recovery path — corrupt `/profiles.json` deliberately and confirm the
   device restores defaults rather than rebooting.
3. Fix 4 / 5 — factory reset and profile deletion through the web UI.
