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

- **ESP32 board** — microcontroller running the firmware
- **IR LED** — transmits IR signals to the TV
- **OLED display (128×32)** — provides status feedback and debug output

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
  released once the daemon confirms the command has been dispatched (see
  Communication Protocol below for per-phase details).
- Startup detection: handled by the systemd service starting at boot.
- Wake detection: handled by `PrepareForSleep(false)` — emitted after resume.
  No lock is needed on wake as the system is already running.

### ESP32 Firmware
Receives commands from the PC over USB, transmits the appropriate IR signal, and
updates the OLED display.

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
- Error states (for debugging):
  - `IR Init Failed`
  - `OLED Init Failed`
  - `Serial Err`

---

## Communication Protocol

### Phase 1 (MVP) — One-way, fixed delay
- Transport: USB CDC Serial at 115200 baud
- Daemon → ESP32 commands: `ON\n`, `OFF\n`
- No response from ESP32
- After sending a command, the daemon waits a fixed delay (~500ms) before
  releasing the inhibitor lock. This is sufficient given that serial
  transmission and IR firing complete in well under 200ms.

### Phase 2 (Final Product) — Two-way, ACK-based
- Transport: USB HID
- Daemon → ESP32 commands: `ON\n`, `OFF\n` (and any future commands)
- ESP32 → Daemon responses: `ACK\n` on success, `ERR\n` on failure
- The daemon releases the inhibitor lock only after receiving `ACK\n`,
  eliminating the fixed delay and confirming the IR signal was actually fired.
- Two-way communication also enables ESP32-initiated events (e.g. a hardware
  button on the device triggering an action on the PC). The scope of these
  features is to be defined as Phase 2 develops.

---

## Daemon — Cross-Platform Design

The daemon is structured to support multiple platforms without rewriting core
logic. Two platform-specific concerns are isolated behind abstract interfaces:

```
IPowerMonitor   — raises OnSleep, OnWake, OnShutdown, OnStartup events
ITransport      — send(const std::string& cmd)
```

`main.cpp` only interacts with these interfaces. Platform implementations are
compiled in or out by CMake based on the target OS.

### Platform implementation map

| Concern          | Linux                        | Windows (Phase 2)            |
|------------------|------------------------------|------------------------------|
| Power events     | sdbus-c++ / systemd-logind   | Win32 Service API            |
| Serial port      | POSIX (`open`/`write`)       | Win32 (`CreateFile`/`WriteFile`) |
| Device discovery | libudev (Phase 2)            | SetupAPI (Phase 2)           |
| Build system     | CMake                        | CMake                        |

### Build system
CMake is used as the build system. It generates the appropriate native build
files per platform (Makefiles on Linux, Visual Studio/Ninja on Windows) from a
single `CMakeLists.txt`. Platform-specific source files and dependencies are
included or excluded via CMake `if(LINUX)` / `if(WIN32)` conditionals, so no
manual changes are needed when building on a different OS.

### Language standard
The daemon targets **C++17**, which is fully supported by GCC, Clang, and MSVC.

### No cross-platform serial library required
Since serial communication is simple (fire-and-forget string writes), no
third-party cross-platform serial library is needed. The `ITransport`
abstraction contains the platform difference to a single implementation file.

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
    LinuxPowerMonitor.h / .cpp
    SerialTransport.h / .cpp
```

### File summaries

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration. Declares source files, C++17 standard, and platform-conditional dependencies. On Linux, pulls in sdbus-c++. Windows sources will be added here in Phase 2. |
| `esp32-ir-remote.service` | systemd unit file. Starts the daemon at boot after logind is ready, restarts it on failure. |
| `ITransport.h` | Abstract interface for sending commands to the ESP32 (`send(cmd)`). Isolates transport details so `main.cpp` is unaffected when Serial is swapped for HID in Phase 2. |
| `IPowerMonitor.h` | Abstract interface for receiving power events. Callers register callbacks for sleep/wake/shutdown and call `run()`. Isolates OS-specific event handling so `main.cpp` is unaffected when the Linux implementation is joined by a Windows one in Phase 2. |
| `SerialTransport.h/.cpp` | `ITransport` implementation for Phase 1. Opens the serial port with POSIX `open()`, configures 115200 8N1 via `termios`, writes commands as `cmd\n`. |
| `LinuxPowerMonitor.h/.cpp` | `IPowerMonitor` implementation for Linux. Connects to systemd-logind over D-Bus via sdbus-c++, subscribes to `PrepareForSleep` and `PrepareForShutdown` signals, manages the delay inhibitor lock, and fires the registered callbacks at the appropriate moment. |
| `main.cpp` | Entry point. Constructs `SerialTransport` and `LinuxPowerMonitor`, wires the callbacks (sleep/shutdown → OFF, wake → ON), sends ON at startup, then calls `monitor->run()` which blocks until the process is killed. |

---

## Roadmap

### Phase 1 — Minimum Viable Product
Validates the core IR control loop before full platform support is added.

**Constraints:**
- Linux only (Arch Linux)
- LG TVs only
- Hardware: ESP32-S3 SuperMini
- Communication: USB CDC Serial (fixed port, e.g. `/dev/ttyACM0`)

**Deliverables:**
- Firmware: receives `ON`/`OFF` over Serial, sends IR, updates OLED ✓
- Daemon: systemd-logind D-Bus listener, fixed-delay inhibitor lock release
- systemd service unit for the daemon

### Phase 2 — Final Product
Expands platform and hardware support. Scope is partially in flux.

**Planned additions:**
- Windows support for the PC daemon
- Multi-manufacturer TV support (configurable IR codes)
- USB HID communication with ACK-based lock release
- Auto-detection of the ESP32 device by USB VID/PID
- Two-way communication channel enabling ESP32-initiated PC actions
  (e.g. hardware buttons on the device — specific features TBD)

---

## Dependencies

### Firmware (PlatformIO)
- `adafruit/Adafruit SSD1306`
- `adafruit/Adafruit GFX Library`
- `crankyoldgit/IRremoteESP8266`

### PC Daemon
- **Build:** CMake
- **Linux:** `sdbus-c++` — D-Bus communication with systemd-logind
- **Windows (Phase 2):** Win32 API only, no extra dependencies

---

## Open Questions
- Define Phase 2 ESP32-initiated features (hardware buttons, etc.).
