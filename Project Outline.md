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
- **IR LED** — transmits IR signals to the TV
- **OLED display (128×32, SSD1306)** — provides status feedback

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
Receives commands from the PC over USB HID, transmits the appropriate IR signal,
updates the OLED display, and sends ACK or ERR back to the daemon.

**IR Codes (LG NEC protocol, confirmed on LG C2):**
- Power ON:  `0x20DF23DC` (discrete — confirmed working on LG C2)
- Power OFF: `0x20DFA35C` (discrete — confirmed working on LG C2)
- Power Toggle: `0x20DF10EF` (fallback for other models)

Note: `0x20DFB34C` was also tested as a candidate ON code but triggers LG's
smart TV network prompt on the C2 — it maps to a "Smart Home" power-on mode
rather than a plain power-on.

Discrete on/off codes are preferred over the toggle code, as they guarantee the
correct TV state regardless of any prior state drift.

**OLED display states:**
- `Waiting...` — idle, no command received
- `TV On` — ON command sent
- `TV Off` — OFF command sent
- `HID Err` — unknown command received

---

## Communication Protocol

### Phase 1 (complete) — One-way, fixed delay
- Transport: USB CDC Serial at 115200 baud
- Daemon → ESP32 commands: `ON\n`, `OFF\n`
- No response from ESP32
- Daemon waited a fixed 500ms delay before releasing the inhibitor lock.

### Phase 2 (current) — Two-way, ACK-based
- Transport: USB HID (64-byte vendor-defined reports)
- Daemon → ESP32: 64-byte output report, command string in first bytes (`ON` / `OFF`)
- ESP32 → Daemon: 64-byte input report, `ACK` on success or `ERR` on unknown command
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

| Concern       | Linux                      | Windows (Phase 2)                |
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

### Language standard
The daemon targets **C++17**, which is fully supported by GCC, Clang, and MSVC.

---

## File Structure

```
daemon/
  CMakeLists.txt
  esp32-ir-remote.service
  src/
    main.cpp
    IPowerMonitor.h
    ITransport.h
    HIDTransport.h / .cpp
    LinuxPowerMonitor.h / .cpp
    SerialTransport.h / .cpp     (Phase 1 — retained for reference)
```

### File summaries

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration. C++17, hidapi on all platforms, sdbus-c++ on Linux. |
| `esp32-ir-remote.service` | systemd unit file. Starts at boot after logind, restarts on failure. |
| `ITransport.h` | Abstract interface: `send(cmd)`. Isolates transport so `main.cpp` is unaffected by Serial → HID swap. |
| `IPowerMonitor.h` | Abstract interface: callbacks for sleep/wake/shutdown + `run()`. Isolates OS-specific event handling. |
| `HIDTransport.h/.cpp` | `ITransport` implementation for Phase 2. Finds ESP32 by VID/PID via hidapi, sends 64-byte reports, blocks until ACK received. Reopens device automatically if it disappears after wake. |
| `SerialTransport.h/.cpp` | `ITransport` implementation for Phase 1. Retained for reference. |
| `LinuxPowerMonitor.h/.cpp` | `IPowerMonitor` implementation for Linux. D-Bus via sdbus-c++, manages inhibitor lock, releases it immediately after `send()` returns with ACK. |
| `main.cpp` | Entry point. Constructs `HIDTransport` and `LinuxPowerMonitor`, wires callbacks, sends ON at startup, runs event loop. |

---

## Roadmap

### Phase 1 — Minimum Viable Product ✓
- Firmware: USB CDC Serial, receives ON/OFF, fires IR, updates OLED ✓
- Daemon: D-Bus logind listener, fixed-delay inhibitor lock release ✓
- Confirmed working: sleep, wake, shutdown ✓

### Phase 2 — Final Product (in progress)
- USB HID communication with ACK-based lock release ✓
- Device discovery by VID/PID — no hardcoded port numbers ✓
- Firmware: sends ACK after IR confirmed transmitted ✓
- Daemon installed as systemd service — starts at boot, restarts on failure ✓
- Graceful handling when ESP32 is unplugged — daemon starts cleanly, sleep/shutdown
  proceed normally, device is found automatically when next plugged in ✓
- Confirmed working: sleep, wake, shutdown, boot ✓
- Windows support for the PC daemon
- Multi-manufacturer TV support (configurable IR codes)
- Two-way communication: ESP32-initiated PC actions (scope TBD)

---

## Dependencies

### Firmware (PlatformIO)
- `adafruit/Adafruit SSD1306`
- `adafruit/Adafruit GFX Library`
- `crankyoldgit/IRremoteESP8266`

### PC Daemon
- **Build:** CMake
- **All platforms:** `hidapi` — USB HID communication
- **Linux:** `sdbus-c++` — D-Bus communication with systemd-logind
- **Windows (Phase 2):** Win32 API only, no extra dependencies beyond hidapi

---

## Open Questions
- Define Phase 2 ESP32-initiated features (hardware buttons, etc.).
- Assign registered VID/PID before any public or commercial release.
