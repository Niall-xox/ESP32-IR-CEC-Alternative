# ESP32 IR Remote — Project Brief

A USB stick that makes a TV follow a PC's power state. The PC sleeps, the TV
turns off. The PC wakes, the TV turns on. It is a replacement for HDMI-CEC on
machines where CEC does not work, which is most PCs.

This file is the single source of truth for how the project works and where it
stands. It is meant to be read start to finish in about fifteen minutes.

---

## 1. The shape of the thing

Two pieces of software, one cable between them.

```
  PC                                  ESP32-S3 stick                TV
  ──                                  ─────────────                 ──
  daemon  ──── USB HID ────────────►  firmware  ──── IR LED ─────►
  (a background service)              (Arduino)      (infrared)
```

**The daemon** is a small background program on the PC. Its only job is to
notice power events — sleep, wake, shutdown, boot — and send the word `ON` or
`OFF` down the USB cable.

**The firmware** runs on the ESP32. It listens on USB, and when it hears `ON` or
`OFF` it blinks the matching infrared code at the TV, exactly the way the TV's
own remote would. It also drives a small OLED screen and one button, and can put
up a WiFi configuration page.

Neither side knows anything about the other beyond those two words and a reply.

### Why USB HID and not a serial port

HID (Human Interface Device) is the USB class that keyboards and mice use. The
advantage is that **every OS already has a driver for it**. Plug the stick in
and it works — no COM port to pick, no driver to install, nothing to sign on
Windows. That is why the project moved from serial to HID.

The cost is that the ESP32 has to have native USB hardware. The ESP32-S3 does.
Older ESP32 and ESP8266 boards do not, and cannot run this firmware.
(An earlier serial version exists at `daemon/archive/SerialTransport.*`. It is
**not compiled and not used** — see §8.)

---

## 2. Hardware

| Part | Notes |
|---|---|
| ESP32-S3 SuperMini | Native USB. Any S3 board works; pins are set in code, not by the board profile. |
| KY-005 IR LED module | On GPIO 4. |
| SSD1306 OLED, 128×32, I²C | SDA GPIO 2, SCL GPIO 3, address 0x3C. |
| Push button | GPIO 5, to ground (uses the internal pull-up). |

CAD for the enclosure is in `3D modeling/`. The `.FCStd` is the editable master;
`.stl` exports are deliberately not tracked in git.

---

## 3. The conversation over USB

Every exchange is one message and one reply, in fixed-size 64-byte packets.

**PC → ESP32:** `[sequence byte][ "ON" or "OFF" ][zero padding...]`

**ESP32 → PC:** `[same sequence byte][ "ACK" or "ERR" ][zero padding...]`

The **sequence byte** is a counter that increments on every command (1…255, then
back to 1). The reply carries the same number back. This is what lets the daemon
tell "this is the answer to the command I just sent" apart from "this is a stale
answer to something older". Without it the daemon would have to guess from
timing, which is how you end up believing a TV turned off when it did not.

`ACK` means **the infrared signal was actually transmitted**. Not "the message
arrived" — transmitted. `ERR` means the command could not be honoured, almost
always because the selected profile has no code stored for that direction.

That distinction is the backbone of the whole project: the daemon never reports
success it did not observe.

### Timing budgets

Each command carries a **budget** — how long the PC is willing to wait before it
gives up and carries on going to sleep. The budget belongs to the *event*, not
to the transport and not to the platform:

| Event | Budget | Why |
|---|---|---|
| Linux sleep / shutdown / wake / boot | 4 s | logind lets a program delay a sleep by 5 s by default. 4 s leaves margin. |
| Windows sleep | 1.5 s | Windows does **not** let you delay a sleep. The machine goes down about 2 s after telling you. |
| Windows shutdown | 20 s | Windows *does* wait here. The installer sets the OS-side limit to 60 s; 20 s is deliberately under it. |
| Everything else (display changes, wake, device replug) | 4 s (default) | Nothing is waiting on these, so they get the ordinary budget. |

The budget covers the whole attempt: opening the device, retrying a failed
write, and waiting for the ACK.

---

## 4. The firmware (`firmware/`)

Five source files, about 1,100 lines.

| File | Does |
|---|---|
| `main.cpp` | USB HID device, IR sending, the WiFi config web server, and the wiring between button, display and storage. |
| `Profiles.cpp/.h` | The TV profiles and settings, and reading/writing them to flash. |
| `Display.cpp/.h` | Everything drawn on the OLED, and the timers that blank it. |
| `Button.cpp/.h` | Turns raw pin readings into press / hold / release events. |
| `HoldTimings.h` | The four hold durations, in one place. |

### Profiles

A **profile** is one TV: a name, an IR protocol (NEC, Samsung or Sony), an ON
code, an OFF code, and a "visible" flag that controls whether the button cycles
through it.

Five ship by default. **Only LG has real codes**; the rest are `0x00000000`,
which means "not configured" — the firmware answers `ERR` and shows *Not
Configured* rather than transmitting a meaningless pulse. Up to 32 profiles.

Codes are stored on the ESP32's own flash filesystem (LittleFS) as two JSON
files: `/profiles.json` and `/settings.json`. Writes go to a `.tmp` file first,
are length-checked, and only then renamed over the real file — so a power cut
mid-write leaves the old file intact rather than half a new one.

### Discrete vs toggle codes

Most TV remotes send one *toggle* code for the power button — press it and the
TV flips state. That is useless here, because if the TV and PC ever drift out of
sync every command makes it worse.

This project needs **discrete** codes: a separate "power on" code and "power
off" code. Most TVs understand them even though their own remote never sends
them, which is why the codes have to be looked up rather than copied off the
remote in front of you.

### The button

One button, four things, decided by how long you hold it:

| Hold for | On release |
|---|---|
| under 0.3 s | **Press** — wake the screen; press again to cycle to the next visible profile |
| 0.3 – 5 s | Nothing (you changed your mind — the screen shows a filling bar as a countdown) |
| 5 – 8 s | **Toggle WiFi config mode** on or off |
| 8 – 23 s | Nothing (again, a bar shows how far you are) |
| held past 23 s | **Factory reset** — fires while still held, not on release |

The press behaviour has one subtlety worth knowing: when the screen is off, the
*first* press just wakes it and shows the current profile, and only the next
press cycles. When "display always on" is enabled the screen never sleeps, so
every press cycles. That is deliberate — it stops a blind press in the dark
silently changing which TV you are controlling.

### The OLED

Shows the active profile. By default it blanks after 2 seconds; "display always
on" keeps it lit. It also shows: `TV On` / `TV Off` confirmations when a command
arrives, the two hold progress bars, `Not Configured` when a profile has no
code, and — in WiFi mode — the AP's IP address.

### WiFi config mode

Hold the button 5–8 seconds and the ESP32 starts its own WiFi access point:

- SSID `ESP32-IR-Remote`, password `irremote123`
- Browse to the IP shown on the OLED

The page lets you add, edit, delete and reorder profiles, set the active one,
toggle "display always on", and factory reset. It is served from LittleFS
(`firmware/data/index.html`, uploaded with `pio run -t uploadfs`).

The AP is transient and user-initiated — it is not running unless you asked for
it. Power syncing keeps working normally while it is up.

---

## 5. The daemon (`daemon/`)

About 1,400 lines of C++, one binary, built with CMake. It is built around two
small interfaces, which is what keeps Linux and Windows from tangling:

- **`ITransport`** — "send this command, tell me if it was confirmed."
  `HIDTransport` is the only implementation and is shared by both platforms.
- **`IPowerMonitor`** — "watch the OS and call me when the TV should change."
  `LinuxPowerMonitor` and `WindowsPowerMonitor` implement it.

`main.cpp` picks the right monitor for the platform and connects them. That
split is why Windows support could be added later without disturbing Linux.

### Linux (`LinuxPowerMonitor`, ~140 lines)

Linux makes this easy. `systemd-logind` broadcasts `PrepareForSleep` and
`PrepareForShutdown` over D-Bus, and — crucially — lets a program take an
**inhibitor lock** that *delays* the sleep until it says it is ready.

So the sequence is honest end to end:

```
sleep requested → daemon sends OFF → ESP32 transmits IR → ACK
               → daemon releases the lock → machine sleeps
```

The TV is genuinely off before the PC goes down. On wake, the lock is re-taken
ready for next time.

Boot is handled separately: on startup the daemon reads `/proc/uptime` and only
sends `ON` if the machine has been up less than 3 minutes. Otherwise restarting
the service would turn your TV on for no reason.

### Windows (`WindowsPowerMonitor`, ~600 lines + capability probe)

Windows is harder, for three reasons.

**1. Only a Service gets power events.** A normal program does not receive
sleep/shutdown notifications at all. So the daemon has to register as a Windows
Service. Run it from a terminal with `--console` and it just checks the ESP32
answers, then exits.

**2. Windows will not wait for you on sleep.** There is no inhibitor lock
equivalent. `PBT_APMSUSPEND` is a *notification*: the machine is going down in
about two seconds whatever you do. Hence the 1.5 s budget. Shutdown is the
opposite — `SERVICE_CONTROL_PRESHUTDOWN` does wait, so there is real time there.

**3. There are three different Windows power models,** and the same code has to
work on all of them:

| Model | What sleep is | How the daemon detects "PC away" |
|---|---|---|
| Classic S3 | Suspend-to-RAM, machine truly off | `PBT_APMSUSPEND` |
| Modern Standby (S0 low-power idle) | Machine stays "on", screen off | Screen going off |
| Hibernate (S4) | Written to disk | `PBT_APMSUSPEND` |

The daemon asks Windows which model this machine is
(`WindowsPowerCapabilities`) and picks its policy from the answer, rather than
guessing. On an S3 machine an idle screen blank is **ignored**, because a real
suspend event is coming. On a Modern Standby machine there is no such event, so
the screen blank is the only signal available and it does drive the OFF.

That asymmetry is on purpose. A wrong ON is annoying — you turn the TV off with
your own remote. A wrong OFF blacks out a TV somebody is watching. So ON and
OFF are never given a symmetric rule.

**Threading.** The service control handler runs on the OS's thread and must
return immediately. So it only *records* what happened and wakes a worker
thread, which does the actual sending. The HID transport is only ever touched by
that one worker thread.

---

## 6. Rules that must not be broken

These are the ones where breaking them re-introduces a bug that was already
fixed once. Short list, kept short on purpose.

**Honesty**
- `ACK` is sent only after the IR signal has finished transmitting.
- A `0x0` code answers `ERR`. Never `ACK`.
- Replies are matched to requests by sequence byte, never by timing.

**Timing**
- The budget travels with the event, in `TvCommand`. It is not a property of the
  transport and not a property of the platform. Two earlier versions got this
  wrong in opposite directions: one capped the 20 s Windows shutdown at 4 s, the
  other gave a harmless screen blank a suspend's 1.5 s panic budget.
- A `false` from `send()` must never stop the machine sleeping or shutting down.

**ON vs OFF are not symmetric**
- An `ON` must be caused by something implying a person is present. A service
  starting is not that; nor is `PBT_APMRESUMEAUTOMATIC` (the machine waking
  itself for a maintenance task at 3am).
- An idle screen blank is an acceptable `OFF` trigger *only* on a machine with
  no usable suspend event, decided from the capability report.

**Threading**
- `HIDTransport` is single-threaded and holds no lock. On Windows only the
  worker thread touches it. A control handler records and returns.

**Storage**
- Flash writes are atomic *and* length-checked. Atomically replacing a file with
  incomplete content is still corruption.
- Every index into the profile list is range-checked where it is used.

**Only one process may hold the device.** Two programs opening the same HID
device eat each other's replies — and the sequence byte cannot catch it, because
the reply is well-formed and correctly numbered, just for somebody else's
request. Linux gets this from the service unit; Windows uses a named mutex.

**Logging must be unbuffered** (`std::unitbuf`). Otherwise the last lines before
the process is killed — including the shutdown `OFF` confirmation, the single
most useful line — are lost.

**Build**
- `ARDUINO_USB_MODE` must be `0`. At `1` the device enumerates as a serial port
  and the daemon never finds it.
- Library versions are pinned exactly, and the flash partition table is stated
  explicitly, so a toolchain bump cannot move LittleFS out from under stored
  profiles.

---

## 7. Building and installing

**Firmware** (needs PlatformIO):
```
cd firmware
pio run -t upload      # the firmware
pio run -t uploadfs    # the web UI into LittleFS — needed once
```

**Daemon, Linux:**
```
cmake -B daemon/build -S daemon && cmake --build daemon/build
sudo cmake --install daemon/build
```
Needs `sdbus-c++` **2.x** (1.x will not compile) and `hidapi`. Then create the
`esp32ir` user and group, install the udev rule, and enable the service — the
packages in `daemon/packaging/` do all of that for you.

**Packaged:** `.deb` and `.rpm` are built by GitHub Actions on a `v*` tag;
Arch builds from `daemon/packaging/PKGBUILD`; NixOS has a flake and a module
(`services.esp32-ir-remote.enable = true;`).

**Daemon, Windows** (needs Visual Studio Build Tools and vcpkg's hidapi):
```
cmake -B daemon/build -S daemon -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build daemon/build --config Release
```
Then from an **elevated** PowerShell:
```
powershell -ExecutionPolicy Bypass -File .\install-service.ps1 -BinaryPath <path>\esp32-ir-daemon.exe
```
Logs go to `C:\ProgramData\ESP32IRRemote\daemon.log`.
`verify-windows.ps1` writes a report of what the machine is and what the daemon
did, which is how the three power models are compared across machines.

---

## 8. Where the project stands

### Working and verified on hardware (Linux)

Sleep, wake, shutdown and boot all confirmed on NixOS: the inhibitor lock held
the poweroff off until the ESP32 confirmed the IR had gone out. The uptime gate
was proven both ways. Unconfigured-profile handling was proven (Samsung
selected, nothing transmitted, `Not Configured` on screen).

### Working, less proven (Windows)

Builds in CI on every push. Has been run as a service on three machines covering
the three power models. Behaves correctly in the cases tested, but has had
nothing like Linux's exercise.

### Not verified

- Boot with "display always on" enabled.
- Factory reset and profile deletion through the web UI.
- Recovery from a corrupt `profiles.json` — the code path exists, but testing it
  means flashing a deliberately broken filesystem image. Judged not worth it.

### Known gaps

| Gap | Severity |
|---|---|
| **No watchdog in the firmware.** Nothing detects a hung `loop()` on a device meant to sit powered for months. It stays hung until unplugged, with no diagnostic anywhere. | **Highest** |
| **Placeholder USB IDs `1234:5678`.** Common hobbyist defaults, so another device could collide — and they are hand-copied into **five** files. | High |
| **No README, no LICENSE.** Both the Arch and RPM packages declare MIT while no licence text exists in the repo. | High |
| **Four of five default profiles have no codes.** Anyone without an LG TV has to find discrete hex codes themselves, with nothing in the repo telling them where. | High |
| **Serial support is dead code.** `daemon/archive/SerialTransport.*` is not compiled and cannot be selected. The firmware is HID-only and requires an ESP32-S3. | Medium — decide and act |
| **Windows service runs as LocalSystem.** Linux runs unprivileged; Windows should move to LocalService to match. | Medium |
| CI does not build the firmware — only the daemon. | Medium |
| WiFi AP password is hardcoded. Better: derive it per-device from the chip ID and show it on the OLED. | Low |
| The web server serves any file on LittleFS, including `profiles.json`. Harmless today. | Low |

---

## 9. What to do next

In the order that gets the project finished.

1. **Add the firmware watchdog.** ~5 lines: enable the task watchdog in
   `setup()` with a timeout that comfortably exceeds the longest legitimate
   blocking operation (an IR send is ~70 ms), and feed it in `loop()`. This is
   the one remaining defect that can make the device silently stop working,
   which is exactly the failure mode the project exists to beat.

2. **Get real USB IDs, and generate them from one file.** `pid.codes` allocates
   free PIDs under VID `0x1209` for open hardware. Then have the build generate
   the firmware header, the udev rule and the two PowerShell defaults from a
   single source file, so the count cannot go 5 → 6.

3. **Write the README and add a LICENSE.** For the stated audience — hobbyists
   who want this to work — this is the actual blocker, more than any code issue.
   The README needs: what it is, what hardware, how to flash, how to install per
   OS, and *where to find discrete IR codes* (the LIRC and irdb databases are
   the standard answer).

4. **Decide the fate of serial.** Recommendation: delete `daemon/archive/`. The
   firmware cannot run on a board that needs it, so it is not a fallback — it is
   an unbuildable copy of a design that was replaced.

5. **Fill in or remove the empty profiles.** Shipping four profiles that cannot
   work is worse than shipping one that does. Either find the codes or ship only
   LG plus an "Add your own" note in the web UI.

6. **Finish Windows.** Move the service to LocalService; add the firmware to CI.

### Worth considering, not required

**An IR receiver.** A VS1838B costs about £1 and one more GPIO. With it, the web
UI could have a "learn" button: point your remote at the stick, press power, and
it captures the code. `IRremoteESP8266` already includes the decoder, so this is
mostly UI work. It would turn "works if you can find hex codes for your TV" into
"works for anyone" — the single biggest improvement available to the product,
and the reason to weigh it against the scope discipline everywhere else.

---

## 10. Things worth knowing that are easy to miss

- **`onButtonHold` in `firmware/src/main.cpp` hardcodes `5000`, `8000` and
  `23000`** instead of using the `HoldTimings` constants right next to it.
  Changing a timing in `HoldTimings.h` will therefore not fully take effect.
  Worth fixing on the next touch.

- **The Windows daemon's "already off, skip the send" optimisation** (the
  `lastAsserted_` / `generation_` machinery) only ever suppresses one thing: a
  repeat OFF during a suspend. It costs roughly 40 lines of careful concurrency
  bookkeeping to save a send that is harmless anyway, because discrete codes are
  not toggles. If the Windows path ever needs simplifying, that is the first
  thing to remove.

- **`logReportDescriptor()` in `HIDTransport.cpp`** exists to diagnose one
  Windows bug that has since been fixed in the firmware. It can go.

- **If the Windows capability query ever fails**, the daemon falls back to
  letting a screen blank turn the TV off — on a machine that might be S3. That
  is the one place the code does the less-safe thing when it does not know. Rare
  enough to leave, but worth an eye.
