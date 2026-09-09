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

### The one idea the whole project rests on

**The TV mirrors what the PC's own screen is doing.**

That is the rule, and everything else follows from it. If the PC's display is
on, the TV should be on. If the display goes off — because you asked it to,
because the machine idled out, because it went to sleep or shut down — the TV
should be off. It is the same behaviour a CEC-connected display gives you, which
is the point: this device exists because CEC usually is not available on a PC.

### Why USB HID and not a serial port

HID (Human Interface Device) is the USB class that keyboards and mice use. The
advantage is that **every OS already has a driver for it**. Plug the stick in
and it works — no COM port to pick, no driver to install, nothing to sign on
Windows. That is why the project moved from serial to HID.

The cost is that the ESP32 has to have native USB hardware. The ESP32-S3 does,
and so does the S2. The ESP32-C3 does **not** (its USB controller only does
serial/JTAG, not arbitrary HID), and neither do the classic ESP32 or the
ESP8266. An earlier serial version sits unbuilt at
`daemon/archive/SerialTransport.*` — see §11.

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

### The USB identity, and why it is `1209:0001`

Every USB device announces a **vendor ID** and a **product ID**. Vendor IDs are
issued by the USB-IF and cost real money, which is not worth spending on a
project that may never leave one desk.

`0x1209` is **pid.codes**, a vendor ID held in trust for open-source hardware
and shared out free. `0x0001` under it is their designated **test PID**, meant
for exactly this situation: a device under development that has not been
allocated a product ID of its own yet.

It replaced `1234:5678`, which was simply made up, and is better in three ways:

- `1209` is a real allocation, so the device is not squatting on some other
  company's identity.
- It is self-documenting — anyone who recognises it knows it is a placeholder.
- The upgrade path is a **PID-only** change. Applying to pid.codes is a free
  pull request, and the vendor half never moves.

**The caveat, stated plainly.** pid.codes ask that products are not
*distributed* on the test PID, precisely because it is shared: several devices
in development can carry it, and the daemon would open whichever it found
first. That is survivable rather than fatal here — the daemon already prefers a
device whose USB product string is `ESP32 IR Remote` — but a real PID is still
needed before this is handed to anybody else. It stays on the list in §8.

The value lives in **five** hand-kept places, which must change together:

| File | Spelling |
|---|---|
| `firmware/src/main.cpp` | `0x1209` / `0x0001` |
| `daemon/src/main.cpp` | `0x1209` / `0x0001` |
| `daemon/99-esp32-ir-remote.rules` | `"1209"` / `"0001"` (lowercase, no `0x`) |
| `daemon/packaging/windows/install-service.ps1` | `"1209"` / `"0001"` |
| `daemon/packaging/windows/verify-windows.ps1` | `"1209"` / `"0001"` |

Five copies is itself the argument for generating them from one source — §9.

---

## 3. The conversation over USB

Every exchange is one message and one reply, in fixed-size 64-byte packets.

**PC → ESP32:** `[sequence byte][ "ON" or "OFF" ][zero padding...]`

**ESP32 → PC:** `[same sequence byte][ "ACK" or "ERR" ][zero padding...]`

The **sequence byte** is a counter that increments on every command (1…255, then
back to 1). The reply carries the same number back, and the daemon ignores any
reply carrying a different one.

It exists to catch one specific race. The daemon sends `OFF`; USB is
re-enumerating, so the reply is slow; the daemon gives up and reports failure.
The ESP32 *did* receive it, transmits, and queues an `ACK` that is now in
flight. The next event sends `ON` — and without the sequence byte the daemon
would read that stale `ACK`, believe `ON` was confirmed, and log a success that
never happened. One byte on the wire prevents it.

`ACK` means **the infrared signal was actually transmitted**. `ERR` means it was
not, which in practice means the active profile has no code stored for that
direction. Both are useful: the daemon treats them the same way (the command
failed), but `ERR` arrives instantly and says *why*, instead of looking
identical to a broken cable for two seconds.

### One timeout, and failing fast when nothing is plugged in

A send gets **2 seconds** — total, covering opening the device, any retried
write, and waiting for the reply. There is one number, in
`HIDTransport.cpp`, and nothing else tunes it. A healthy round trip takes well
under 200 ms, and every deadline an OS imposes on us is longer than 2 s.

The more important rule is what happens when the stick is **not connected**:

- **Nothing enumerated** → give up immediately. There is nothing to wait for.
- **A handle we had just stopped working** → retry until the deadline, because
  the device demonstrably exists and is probably re-enumerating.

That distinction is what lets you leave the daemon installed with the stick
unplugged and have sleep and shutdown behave completely normally. Without it,
the daemon sits polling for a device that will never appear, holding the machine
up every single time.

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

Four ship by default, and **every one of them transmits**. Up to 32 profiles.

| Profile | Protocol | ON | OFF |
|---|---|---|---|
| LG | NEC | `0x20DF23DC` | `0x20DFA35C` |
| Samsung | SAMSUNG | `0xE0E09966` | `0xE0E019E6` |
| Sony | SONY | `0x00000750` | `0x00000F50` |
| Toshiba | NEC | `0x02FD7E81` | `0x02FDFE01` |

That is the rule for the factory default: a brand earns a place only once a
discrete on/off pair exists that covers the product line rather than one model.
A default profile that cannot work is worse than no profile, because it looks
like the device supports your TV.

**TCL and Hisense are therefore absent, not blank.** Neither publishes usable
discrete codes: TCL has only a power *toggle*, in a protocol this firmware does
not speak, and Hisense is not in irdb at all. Filling them with a toggle would
be worse still — the same code in both fields flips the TV on every command,
which is the drift discrete codes exist to prevent. Owners of those sets add a
profile through the web UI instead.

The *Not Configured* path still exists and still matters: it catches a
user-added profile saved with one or both codes blank.

That difference is structural, not bad luck. LG, Samsung, Sony and Toshiba
design their own remotes across a whole product line, so one code set covers the
brand. TCL and Hisense are assemblers — the same model number ships as a Roku TV
in one market and a Google TV in another, with different remotes.

**Only LG is proven on hardware.** The other three are derived from corroborated
sources and cross-checked against the encoder, but nobody has pointed the stick
at a Samsung, Sony or Toshiba. Two known risks:

- **Samsung deep sleep.** There are reports that Samsung's discrete power-on
  stops working once the TV has been off for a long time — the set stops
  listening to IR. If true, that hits this project's main use case directly
  (PC wakes in the morning, TV does not). Unconfirmed, and worth being the first
  thing checked if a Samsung ever turns up.
- **Samsung's discrete pair is not in irdb**, which lists only the toggle. It
  comes from RemoteCentral instead and derives consistently to the published
  hex, but it is one source rather than two.

Codes are stored on the ESP32's own flash filesystem (LittleFS) as two JSON
files: `/profiles.json` and `/settings.json`. Writes go to a `.tmp` file first,
are length-checked, and only then renamed over the real file.

The length check is not about validating what you typed — it catches a
*truncated* write, from the flash filling up or power being cut mid-write.
Without it the atomic rename would atomically install a valid-looking but
unparseable file, and every IR code you had entered would be gone on the next
boot.

### Discrete vs toggle codes

Most TV remotes send one *toggle* code for the power button — press it and the
TV flips state. That is useless here, because if the TV and PC ever drift out of
sync every command makes it worse.

This project needs **discrete** codes: a separate "power on" code and "power
off" code. Most TVs understand them even though their own remote never sends
them, which is why the codes have to be looked up rather than copied off the
remote in front of you.

A useful consequence: repeat commands are always safe. Sending `OFF` to a TV
that is already off does nothing at all. The daemon never has to track what it
last sent, and never has to decide whether a command is redundant.

### Where IR codes come from

Kept here because finding these again from scratch is slow, and because getting
a code wrong is worse than having none.

**[irdb](https://github.com/probonopd/irdb)** is the one to reach for first —
the largest crowd-sourced database, and it stores codes as *protocol, device,
subdevice, function* rather than as raw hex, which is exactly what you need.
Browse `codes/<Brand>/TV/` and read the CSVs:

```
curl -s https://api.github.com/repos/probonopd/irdb/contents/codes/Sony/TV
curl -s https://cdn.jsdelivr.net/gh/probonopd/irdb@master/codes/Sony/TV/1,-1.csv | grep -i power
  POWER ON/OFF,Sony12,1,-1,21
  POWER ON,Sony12,1,-1,46
  POWER OFF,Sony12,1,-1,47
```

Two things that reads out at a glance: whether **discrete** on/off codes exist
at all (many brands only have a toggle), and **how many code sets** the brand
has. One code set for a whole brand means one consistent scheme and good
coverage; five means it varies by model and you should not ship a guess.

**Turning those parameters into the hex the firmware wants.** Do *not* copy hex
off a forum — convert from the protocol parameters using the same encoder the
firmware links against, so the bit ordering cannot disagree:

- NEC — `IRsend::encodeNEC(address, command)` in `ir_NEC.cpp`
- Sony — `IRsend::encodeSony(nbits, command, address, extended)` in `ir_Sony.cpp`

Both live in `firmware/.pio/libdeps/lolin_s3_mini/IRremoteESP8266/src/`. They are
small and pure, so the quickest check is to copy them into a throwaway C file and
print the values.

**Sanity-check any result two ways.** Run the brand's *toggle* code through the
same path and confirm it matches a value you can find independently — Sony's
toggle should come out `0xA90`, LG's ON should come out `0x20DF23DC`, which is
the code already proven on real hardware here. If the toggle reproduces, the
encoding is right and the discrete pair can be trusted.

**Other sources**, in rough order of usefulness:

- [RemoteCentral's discrete code library](https://files.remotecentral.com/library/index.html)
  — curated per brand, and the best source for discrete codes irdb lacks. This
  is where the Samsung discrete pair came from; irdb only lists Samsung's toggle.
- [SB-Projects protocol reference](https://www.sbprojects.net/knowledge/ir/sirc.php)
  — how the protocols are actually framed. Read this when a code looks right but
  does not work; the Sony 12/15/20-bit distinction is the classic trap.
- The Flipper Zero and LIRC remote databases, and manufacturer RS-232/IR PDFs
  (Hisense publish one) — worth a look, but model-specific far more often.

**Watch out for.** Forum hex with no protocol stated (unusable — you cannot tell
12-bit Sony from 20-bit). Single-model blog posts presented as brand-wide. And
NEC codes whose checksum does not validate: in NEC the second command byte is
the bitwise inverse of the first, so `0x…8B75` is wrong on its face because
`0x8B` inverts to `0x74`. That one check rejects a surprising amount of bad data.

### The button

One button, four things, decided by how long you hold it:

| Hold for | On release |
|---|---|
| under 0.3 s | **Press** — wake the screen; press again to cycle to the next visible profile |
| 0.3 – 4 s | Nothing (you changed your mind — the screen shows a filling bar as a countdown) |
| 4 – 7 s | **Toggle WiFi config mode** on or off |
| 7 – 16 s | Nothing (again, a bar shows how far you are) |
| held past 16 s | **Factory reset** — fires while still held, not on release |

The press behaviour has one subtlety worth knowing: when the screen is off, the
*first* press just wakes it and shows the current profile, and only the next
press cycles. When "display always on" is enabled the screen never sleeps, so
every press cycles. That is deliberate — it stops a blind press in the dark
silently changing which TV you are controlling.

All four durations live in `HoldTimings.h` and are used from there.

### The watchdog

The ESP32 has a hardware timer that reboots the chip if the firmware stops
feeding it. `loop()` feeds it on every pass, so if `loop()` ever stops running —
a hang, a deadlock, a wait that never returns — the device resets itself after
**20 seconds** and comes back working.

This matters more than it sounds. A hung stick and an unplugged stick look
identical from the PC: the daemon just sees a command that got no reply. So
without the watchdog a hang would sit there until somebody noticed the TV had
stopped following the PC — which is exactly the unreliability this device exists
to beat. A reset costs nothing: profiles live in flash, and the daemon reopens
the device on its next command.

The 20 seconds is set by the **web server**, not by anything in our own code.
Serving the config page can legitimately block for around 12 s inside one pass
of `loop()` if a browser stalls mid-request (the server waits 5 s for the
request, 5 s for data to be acknowledged and 2 s for the close). Everything else
— button, display, IR, USB — is under 200 ms. The generous margin is deliberate:
a false reset while somebody is saving IR codes would be worse than a slow one.

It is armed at the very end of `setup()`, so start-up is never watched. A hang
before that point is a bricked-at-boot device you find the moment you flash it,
and leaving setup out keeps first-boot flash formatting from having to fit
inside the timeout.

### The OLED

Shows the active profile. By default it blanks after 2 seconds; "display always
on" keeps it lit. It also shows: `TV On` / `TV Off` confirmations when a command
arrives, the two hold progress bars, `Not Configured` when a profile has no
code, and — in WiFi mode — the AP's IP address.

**The progress bars are one block per second**, and the block count is derived
from the thresholds in `HoldTimings.h` rather than written down separately — so
the hold bar has 4 blocks and the reset bar 9, and both follow automatically if
a timing changes. That makes them countable rather than merely watchable: four
blocks means four seconds.

Each bar stops one block short of full, because the moment it would complete is
the moment the screen changes — to *Release To Enter Wireless Config!* at 4 s,
and to the status screen when the reset fires at 16 s. The screen changing is
the completion signal.

### WiFi config mode

Hold the button 4–7 seconds and the ESP32 starts its own WiFi access point:

- SSID `ESP32-IR-Remote`, password `irremote123`
- Browse to the IP shown on the OLED

The page lets you add, edit, delete and reorder profiles, set the active one,
toggle "display always on", and factory reset. It is served from LittleFS
(`firmware/data/index.html`, uploaded with `pio run -t uploadfs`).

The AP is transient and user-initiated — it is not running unless you asked for
it. Power syncing keeps working normally while it is up.

---

## 5. The daemon (`daemon/`)

About 1,100 lines of C++, one binary, built with CMake. It is built around two
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

The TV is genuinely off before the PC goes down.

**The lock has to be taken in advance — that is the API, not a choice we made.**
`PrepareForSleep(true)` is not a request you can answer; it is an announcement
that the delay window has already opened. logind collects who holds delay locks,
*then* broadcasts, *then* waits for those specific file descriptors to close. A
program that was not already holding one is not on the list being waited for. So
the daemon takes the lock at startup and re-takes it on every wake.

Boot is handled separately: on startup the daemon reads `/proc/uptime` and only
sends `ON` if the machine has been up less than 3 minutes. Without that gate, a
package upgrade restarting the service at 3 a.m. would turn your TV on.

### Windows (`WindowsPowerMonitor`, ~450 lines)

Windows is harder, for two reasons.

**1. Only a Service gets power events.** A normal program does not receive
sleep or shutdown notifications at all. So the daemon registers as a Windows
Service — which the installer does for you; you never do it by hand. (This is
completely standard for PC accessories: G HUB, iCUE, Synapse, Stream Deck and
most printer software all install services.) Run the binary from a terminal with
`--console` and it just checks the ESP32 answers, then exits. That is a
debugging aid, nothing more.

**2. Windows will not wait for you on sleep.** There is no inhibitor lock
equivalent. `PBT_APMSUSPEND` is a *notification*: the machine is going down in
about two seconds whatever you do. Shutdown is the opposite —
`SERVICE_CONTROL_PRESHUTDOWN` genuinely does wait, and is the real equivalent of
the Linux lock. The installer sets that timeout to 60 s explicitly.

So the guarantee is not uniform, and that is accepted rather than solved:

| Event | Guaranteed? |
|---|---|
| Linux sleep / shutdown | Yes — inhibitor lock |
| Windows shutdown | Yes — preshutdown |
| Windows sleep | Best effort — ~2 s of grace, a send takes ~200 ms |

**What drives the decision.** The console display state, on every machine. The
daemon registers for `CONSOLE_DISPLAY_STATE` notifications: display on → TV on,
display off → TV off, dimmed → treated as on.

There is no branching on the machine's sleep model. There used to be — the
daemon probed whether the machine was classic S3, hibernate-capable or Modern
Standby, and ignored a screen blank on machines where a real suspend event was
coming. That whole apparatus is gone, because the premise was wrong: when
Windows reports "display off" it has *already blanked every display, the TV
among them*. Acting on it does not black out a screen somebody is watching — it
turns off one that is already showing black.

Suspend and resume are kept underneath as a second trigger, in case a lid-close
or an explicit Sleep ever reaches us without a display-state change first.
Duplicate commands are harmless because the codes are discrete.

One nice consequence: `PBT_APMRESUMEAUTOMATIC` — the machine waking itself for a
maintenance task at 3 a.m. — needs no special case. The display stays off, so no
`ON` is sent. The behaviour falls out of the model instead of being hand-coded.

**Threading.** The service control handler runs on the OS's thread and must
return immediately, so it only *records* what happened and wakes a worker
thread, which does the actual sending. Linux needs none of this — sdbus-c++
dispatches signals on the one thread it is already using.

---

## 6. Rules that must not be broken

Short list, kept short on purpose. Each one, if broken, re-introduces a bug that
was already fixed once.

**The model**
- The TV mirrors the PC's display state. Anything else — suspend, resume,
  shutdown, boot — is a secondary trigger under that, not a competing policy.

**Honesty**
- `ACK` is sent only after the IR signal has finished transmitting. A `0x0` code
  answers `ERR`, never `ACK`.
- Replies are matched to requests by sequence byte, never by timing.
- A `false` from `send()` must never stop the machine sleeping or shutting down.

**Timing**
- One timeout for a whole send, and it lives in the transport. Callers do not
  get to pass a budget; earlier versions let them, and it went wrong in both
  directions.
- When no device is enumerated, fail immediately. Retrying is only ever correct
  for a device that was open a moment ago.

**Threading**
- `HIDTransport` is single-threaded and holds no lock. On Windows only the
  worker thread touches it; a control handler records and returns.

**Storage**
- Flash writes are atomic *and* length-checked. Atomically replacing a file with
  incomplete content is still corruption.
- Every index into the profile list is range-checked where it is used.

**Only one process may hold the device.** Two programs opening the same HID
device eat each other's replies — and the sequence byte cannot catch it, because
the reply is well-formed and correctly numbered, just for somebody else's
request. The workflow that causes it is the one we built: service running, then
someone runs the binary with `--console`. Linux is protected by the udev group;
Windows uses a named mutex.

**Logging must be unbuffered** (`std::unitbuf`). Otherwise the last lines before
the process is killed — including the shutdown `OFF` confirmation, the single
most useful line — are lost.

**Build**
- `ARDUINO_USB_MODE` must be `0`. At `1` the device enumerates as a serial port
  and the daemon never finds it.
- The flash partition table is stated explicitly. If a toolchain bump moved the
  default layout, LittleFS would relocate and every stored IR code would be lost
  on the next firmware update.

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
did, for comparing behaviour across machines.

---

## 8. Where the project stands

### Working and verified on hardware (Linux)

Sleep, wake, shutdown and boot all confirmed on NixOS: the inhibitor lock held
the poweroff off until the ESP32 confirmed the IR had gone out. The uptime gate
was proven both ways. Unconfigured-profile handling was proven (Samsung
selected, nothing transmitted, `Not Configured` on screen).

### Working, less proven (Windows)

Builds in CI on every push. Has been run as a service on three machines covering
the three sleep models. Behaves correctly in the cases tested, but has had
nothing like Linux's exercise — and the display-state simplification below
changes its behaviour on S3 machines, so those need re-testing.

### Needs re-testing after the 2026-09-09 changes

1. **Windows, S3 machine**: an idle screen blank should now turn the TV off. It
   previously did not.
2. **Both platforms, normal sleep/wake**: confirm the single 2 s timeout is
   still comfortably enough.
3. **Windows, stick unplugged**: shutdown should be instant, where it was
   previously delayed by up to 20 s.

**Confirmed 2026-09-09 — Linux, stick unplugged.** Sleep is visibly faster with
no stick connected, which is the transport's fail-fast open doing its job. The
old behaviour is preserved in the journal from that morning's boot, and is worth
keeping as the before-picture:

```
15:17:10  [transport] ESP32 not found at startup — will retry when needed
15:17:14  [transport] ESP32 not found — skipping IR command: ON
15:17:14  [cmd] ON FAILED — no ACK, TV state not changed (startup)
```

Four seconds between finding no device and giving up — the old send budget
polling for something that was never going to appear. That delay landed on every
sleep and every shutdown.

### Not verified

- Boot with "display always on" enabled.
- Factory reset and profile deletion through the web UI.
- Recovery from a corrupt `profiles.json` — the code path exists, but testing it
  means flashing a deliberately broken filesystem image. Judged not worth it.

### Known gaps

| Gap | Severity |
|---|---|
| **Test USB PID `1209:0001`.** A real vendor ID, but a *shared* test PID that pid.codes ask not to be distributed on. Needs a PID of its own before release, and it is hand-copied into **five** files. See §2. | High |
| **No README, no LICENSE.** Both the Arch and RPM packages declare MIT while no licence text exists in the repo. | High |
| **No profile for TCL or Hisense** — no usable discrete codes appear to exist for either brand. Not fixable from a database; it needs somebody with the TV in front of them. Those users add a profile by hand. See §4. | Low |
| **Samsung, Sony and Toshiba codes are unverified on hardware.** Derived from corroborated sources and cross-checked against the encoder, but nobody has pointed the stick at one of those TVs. | Medium |
| **Linux does not watch display state.** Your screen blanks, the TV stays on. Now that the whole project is framed as mirroring the display, this is a real inconsistency rather than a design choice. | Medium |
| **Windows service runs as LocalSystem.** Linux runs unprivileged; Windows should move to LocalService to match. | Medium |
| CI does not build the firmware — only the daemon. | Medium |
| A cancelled shutdown does not re-take the Linux inhibitor lock, so the next sleep goes undelayed. Needs the cancel to land in a sub-second window; a sleep/wake cycle repairs it. | Low |
| WiFi AP password is hardcoded. Better: derive it per-device from the chip ID and show it on the OLED. | Low |
| The web server serves any file on LittleFS, including `profiles.json`. Harmless today. | Low |

---

## 9. What to do next

In the order that gets the project finished.

1. **Write the README and add a LICENSE.** For the stated audience — hobbyists
   who want this to work — this is the actual blocker, more than any code issue.
   The README needs: what it is, what hardware, how to flash, how to install per
   OS, and *where to find discrete IR codes* (the LIRC and irdb databases are
   the standard answer).

2. **Verify the new codes on real TVs** if any are within reach — Samsung, Sony
   and Toshiba are all derived rather than tested. A Samsung is the one most
   worth finding, both because it is the most common brand and because of the
   deep-sleep caveat in §4.

3. **Apply to pid.codes for a product ID**, and generate the five copies from
   one source so the count cannot go 5 → 6. Free, and only the PID half moves —
   see §2. Needed before this is handed to anybody else; nothing before that
   depends on it.

4. **Re-test Windows** against the three cases in §8, then move the service to
   LocalService. The installer already grants LOCAL SERVICE rights on the log
   directory, so the only open question is whether that account can open the HID
   device — and the vendor usage page (`0xFF00`) is not one Windows restricts,
   so it very likely can. Test with
   `sc.exe config esp32-ir-remote obj= "NT AUTHORITY\LocalService"`.

5. **Add display-state watching on Linux**, closing the inconsistency in §8.

6. Add the firmware to CI.

### Worth considering, not required

**An IR receiver.** A VS1838B costs about £1 and one more GPIO. With it, the web
UI could have a "learn" button: point your remote at the stick, press power, and
it captures the code. `IRremoteESP8266` already includes the decoder, so this is
mostly UI work. It would turn "works if you can find hex codes for your TV" into
"works for anyone" — the single biggest improvement available to the product.

---

## 10. Decisions taken, and why

Reviewed 2026-09-09. Kept as-is, with the reasoning, so they are not re-litigated:

- **The sequence byte stays.** One byte, ~6 lines, prevents a stale `ACK` being
  read as confirmation of a different command. See §3.
- **`ERR` stays.** ~7 lines, and it turns a misleading two-second timeout into
  an instant accurate log line. If it is ever removed, `ACK` must *not* start
  meaning "received" — instead make an uncoded profile unselectable, so the
  failure cannot occur.
- **The length check on flash writes stays.** It guards against truncated
  writes, not against bad user input. See §4.
- **The Windows named mutex stays.** A registered VID/PID does not address it;
  those are different problems.
- **Exact dependency pins and the explicit partition table stay.** The partition
  one protects stored IR codes across a firmware update.
- **The Linux uptime gate stays**, on the strength of the 3 a.m. unattended
  upgrade case.

### Changed 2026-09-09

- `onButtonHold` now uses the `HoldTimings` constants rather than hardcoded
  literals.
- The Windows "already off, skip the send" optimisation is gone, with the
  `lastAsserted_` / `generation_` bookkeeping that existed only to support it.
- `logReportDescriptor()` is gone from `HIDTransport`.
- **Per-event timing budgets are gone.** `TvCommand::budget`, `budgetFor()` and
  the four budget constants collapsed into one `SEND_TIMEOUT`.
- **The transport now fails immediately when no device is enumerated**, which is
  what fixes sleep and shutdown being delayed with the stick unplugged.
- **`WindowsPowerCapabilities` is deleted** and display-off drives the OFF on
  every machine.

Net: 281 lines removed.

### Added 2026-09-09

- **The firmware watchdog** (§4). Clears what was the highest-severity gap.
  Flashed and verified enumerating.
- **The USB identity moved from `1234:5678` to `1209:0001`** (§2) — a made-up
  pair replaced by pid.codes' open-hardware vendor ID and its test PID.
  **Requires a re-flash to take effect**, since the firmware announces it.
- **The factory default is now four profiles that all transmit** (§4). Samsung
  and Sony filled in, Toshiba added, and TCL and Hisense **removed** rather than
  shipped blank — a default profile that cannot work looks like support the
  device does not have.
- **Button hold timings shortened**: wireless config at 4 s (was 5), factory
  reset at 16 s (was 23). The release window between them stays 3 s. Both
  progress bars derive their span from these constants, so they rescale on
  their own.
- **Sony now transmits at 12 bits, not 20.** A real bug: SIRC has three lengths
  and a TV is the 12-bit form, so the Sony profile could never have worked at
  20 bits regardless of the codes.

  Note that **existing devices keep their old profiles**. `Profiles::begin()`
  only writes the defaults when `profiles.json` is absent, so a device that has
  booted before needs a factory reset — hold the button past 16 s — to pick the
  new set up. Flashing alone is not enough.

---

## 11. Still open

- **A second serial transport**, for ESP32 boards without native USB.
  `ITransport` is exactly the right seam and the archived implementation is a
  usable starting point, but the serial I/O is not the work: **finding the
  device is.** A board without native USB is seen through a CP2102 or CH340
  bridge whose VID/PID (`10c4:ea60`, `1a86:7523`) is shared with thousands of
  unrelated devices, so there is nothing to match on. It needs either a config
  file naming the port or an identify-handshake across candidate ports, plus a
  second firmware build variant, a `dialout` udev rule, and a relaxed systemd
  sandbox. Perhaps 150 lines for the config-file version.

- **Whether the Windows service should stop retrying eventually**, rather than
  restarting forever. Currently `restart/5000` three times with a daily reset.
