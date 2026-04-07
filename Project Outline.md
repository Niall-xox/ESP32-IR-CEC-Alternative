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
- Transport: USB HID (64-byte vendor-defined reports)
- Daemon → ESP32: 64-byte output report, command string in first bytes
- ESP32 → Daemon: 64-byte input report, response string in first bytes

| Command (Daemon → ESP32) | Response (ESP32 → Daemon) | Action |
|--------------------------|---------------------------|--------|
| `ON`  | `ACK` | Fire IR ON code for active profile |
| `OFF` | `ACK` | Fire IR OFF code for active profile |
| `ON` / `OFF` (unknown cmd) | `ERR` | Unknown command received |
| `PING` | `PONG` | Daemon liveness check, triggered by button press |

- `sendNEC()` is synchronous — ACK is sent only after the IR signal has fully
  transmitted, so the daemon releases its inhibitor lock the instant it receives ACK.
- No fixed delays anywhere in the pipeline.

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
- Display turns on
- PING sent to daemon
- Shows:
  ```
  Profile: LG
  Daemon: Waiting...
  ```
- Updates to `Daemon: Connected` on PONG, or `Daemon: Not Found` on timeout
- 2 second timer starts, display turns off after

**Press 2 (display on, within timer):**
- Cycle to next visible profile, reset 2 second timer

**IR command received:**
- Display turns on showing `TV On` or `TV Off`
- Turns off after 2 seconds

### Normal Operation — Display Always On Enabled

**Display:** OLED on continuously showing:
```
Profile: LG
Daemon: Connected
```

**Press 1:**
- PING sent
- `Daemon: Waiting...` updates to `Connected` or `Not Found`

**Press 2:**
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
- **5s:** bar full → text changes to:
  ```
  Release To Enter
  Wireless Config!
  ```
- **Released at 5s:** enter wireless config mode
- **Held past 5s, at 8s:** factory reset screen:
  ```
  Hold To Factory Reset
  [▓▓▓░░░░░░░░░░░░░░░░░]  ← 15 segments, 1 per second
  ```
- **Held to 23s:** factory reset triggers automatically, no release needed

### Wireless Config Mode Active

**Display:** OLED on continuously (always, cannot be disabled):
```
Profile: LG
Daemon: Connected
WiFi: Active
```

**Press 1:**
- PING sent
- `Daemon: Waiting...` updates to `Connected` or `Not Found`

**Press 2:**
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
- **5s:** bar full → text changes to:
  ```
  Release To Exit
  Wireless Config!
  ```
- **Released at 5s:** exit wireless config mode, return to normal operation
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
  src/
    main.cpp

daemon/
  CMakeLists.txt
  esp32-ir-remote.service
  src/
    main.cpp
    IPowerMonitor.h
    ITransport.h
    HIDTransport.h / .cpp
    LinuxPowerMonitor.h / .cpp
    WindowsPowerMonitor.h / .cpp
    SerialTransport.h / .cpp     (Phase 1 — retained for reference)
```

### Daemon file summaries

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration. C++17, hidapi on all platforms, sdbus-c++ on Linux. |
| `esp32-ir-remote.service` | systemd unit file. Starts at boot after logind, restarts on failure. |
| `ITransport.h` | Abstract interface: `send(cmd)`. Isolates transport so `main.cpp` is unaffected by Serial → HID swap. |
| `IPowerMonitor.h` | Abstract interface: callbacks for sleep/wake/shutdown + `run()`. Isolates OS-specific event handling. |
| `HIDTransport.h/.cpp` | `ITransport` implementation. Finds ESP32 by VID/PID via hidapi, sends 64-byte reports, blocks until ACK received. Reopens device automatically if it disappears after wake. Unchanged on both platforms. |
| `SerialTransport.h/.cpp` | `ITransport` implementation for Phase 1. Retained for reference. |
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

### Phase 3 — Multi-Profile, Button, WiFi Config (in progress)
- Physical button (GPIO 5): profile cycling, config mode, factory reset
- Multi-manufacturer IR profile support stored on LittleFS as JSON
- WiFi AP config mode with web UI at `192.168.4.1`
- PING/PONG daemon liveness detection
- Display always-on setting
- Factory reset via button hold or web UI

---

## Dependencies

### Firmware (PlatformIO)
- `adafruit/Adafruit SSD1306`
- `adafruit/Adafruit GFX Library`
- `crankyoldgit/IRremoteESP8266`
- `bblanchon/ArduinoJson` — JSON read/write for LittleFS profiles and settings
- ESP32 built-ins: `WiFi`, `WebServer`, `LittleFS`, `Preferences`

### PC Daemon
- **Build:** CMake
- **All platforms:** `hidapi` — USB HID communication
- **Linux:** `sdbus-c++` — D-Bus communication with systemd-logind
- **Windows:** Win32 API only, no extra dependencies beyond hidapi

---

## Open Questions
- Confirm working discrete IR codes for Samsung, Sony, TCL, Hisense
- Assign registered VID/PID before any public or commercial release
- Two-way communication (ESP32-initiated PC actions) — deferred to future phase
