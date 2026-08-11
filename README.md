# Rotel HomeKit Remote (ESP32-C3)

Control your Rotel amplifier natively from Apple Home – no bridge, no cloud.

This firmware turns an ESP32-C3 RS232 adapter into a genuine HomeKit accessory
for Rotel integrated amplifiers. The amplifier appears in the Home app as a TV
accessory with power, input selection and volume control, including full
support for the iOS Remote widget in Control Center. A built-in web dashboard
handles Wi-Fi onboarding, amplifier model selection, firmware updates and
HomeKit pairing – no recompiling required for end users. A mobile-friendly
**web remote** is included as well, so the amplifier can be controlled from
any browser even without HomeKit.

![Platform](https://img.shields.io/badge/platform-ESP32--C3-blue)
![Framework](https://img.shields.io/badge/framework-Arduino%20%2B%20HomeSpan-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

---

## Features

- **Native HomeKit accessory** (via [HomeSpan](https://github.com/HomeSpan/HomeSpan)) –
  pairs directly with the Home app, works with Siri, automations and scenes
- **TV accessory with input sources**: power on/off, source selection,
  sources can be renamed and hidden per-user in the Home app (persisted in NVS)
- **iOS Remote widget support**: volume, mute, and a modal scheme to adjust
  bass, treble and balance from the Control Center remote
- **Web remote** at `/remote`: power, source wheel, volume, mute, bypass,
  treble, bass and balance from any browser – no HomeKit pairing required
- **Multi-model support**: 9 Rotel model families selectable at runtime from
  the web dashboard – from the classic RA-12 up to Michi, including
  power amplifiers (M8/S5) as a simplified power-switch device
- **Automatic protocol detection**: models that changed their RS232 dialect
  across firmware revisions (RA-1572/RA-1592) are probed at boot and the
  detected protocol generation is cached in NVS
- **Web dashboard** (port 80): status, model selection, HomeKit setup code,
  device address, OTA firmware update, Wi-Fi settings and factory reset
- **Configurable device address**: change the mDNS hostname (e.g.
  `martin-remote.local`) and the setup Wi-Fi name from the dashboard –
  useful when running more than one adapter in the same network
- **Wi-Fi onboarding** with a captive portal (WiFiManagerLite) – no hardcoded
  credentials
- **OTA updates** over HTTP with a one-click installer (GitFirmwareUpdate)
- **Built for 24/7 operation**: no heap allocation in the hot path, fixed
  buffers everywhere, response-paced TX queue, millis()-rollover safe

## Supported amplifiers

| Model | RS232 protocol | Device type |
|---|---|---|
| Rotel A11 (+MKII) | Gen 2 | Integrated amp |
| Rotel A12 / A14 (+MKII) *(default)* | Gen 2 | Integrated amp |
| Rotel RA-11 / RA-12 | Gen 1 | Integrated amp |
| Rotel RA-1570 | Gen 1 | Integrated amp |
| Rotel RA-1572 (+MKII) | auto-detected | Integrated amp |
| Rotel RA-1592 (+MKII) | auto-detected | Integrated amp |
| Rotel RA-6000 | Gen 2 | Integrated amp |
| Michi X3 / X5 | Gen 2 | Integrated amp |
| Michi M8 / S5 | Gen 2 | Power amp (power switch only) |

*Gen 1* devices answer with `key=value!` frames and use `get_*!` queries,
*Gen 2* devices (A12/A14 era and newer) answer with `key=value$` and use `*?`
queries. The firmware abstracts both dialects behind a single command table,
including quirks like the inverted tone-control logic (`tone_on!` vs.
`bypass_off!`) and the Gen 1 byte-count response format
(`product_version={len},{text}`).

Other models that speak one of the two documented dialects will most likely
work too – pick the family with the closest input list and hide unused inputs
in the Home app.

## Hardware

The firmware targets the **LEOTRO ESP32-C3 RS232 adapter** (Rev 1.1):
an ESP32-C3-MINI-1 module driving an SP3232 level shifter behind a male
DB9 connector, powered over USB-C.

```
ESP32-C3 UART (3.3 V TTL) → SP3232 (±RS232 level shifter) → ESD protection → DB9
```

| Signal | GPIO |
|---|---|
| UART TX (→ RS232) | GPIO10 |
| UART RX (← RS232) | GPIO4 |
| BOOT button (HomeSpan control button) | GPIO9 |
| Flashing / logs | USB-C (USB-Serial-JTAG) |

- Serial parameters: **115200 baud, 8N1, no flow control** (Rotel standard)
- The board is wired as **DTE** – connect it to the amplifier with a
  **straight-through** RS232 cable
- Any ESP32-C3 board with an RS232 transceiver on the pins above will work

## How it works

### Architecture

```
┌────────────────────────────────────────────────────────────┐
│ Rotel_HomeSpan2023_C3.ino                                  │
│   HomeKit services (Television, InputSource, TVSpeaker)    │
│   Event-driven sync: Rotel state → HomeKit characteristics │
├──────────────────────┬─────────────────────────────────────┤
│ RotelCommand.h       │ WebPortal.h                         │
│   RS232 driver:      │   WiFiManagerLite (captive portal)  │
│   TX queue, parser,  │   AsyncWebServer (port 80):         │
│   Gen 1/2 dialects,  │   dashboard, web remote, REST API   │
│   auto-detection     │   GitFirmwareUpdate (OTA)           │
├──────────────────────┴─────────────────────────────────────┤
│ RotelModels.h                                              │
│   Model table in flash: dialect, device type, input list,  │
│   balance range · NVS helpers for selection & gen cache    │
└────────────────────────────────────────────────────────────┘
```

- **HomeSpan** owns HomeKit (HAP server on port **8080**, mDNS hostname
  `rotel-remote.local` by default – renamable from the dashboard).
- **WiFiManagerLite** owns the Wi-Fi connection and the captive portal;
  HomeSpan picks up connectivity passively through network events.
- The dashboard and the WML portal share one **AsyncWebServer** on port 80.
  Handlers never block: they only set flags, all real work (OTA download,
  SRP code calculation, NVS writes, reboots) runs in the main loop.

### RS232 link layer

Rotel amplifiers have **no flow control** and their documentation warns that
command bursts can crash the device CPU. The driver therefore uses:

- a **TX ring buffer** (16 entries, statically allocated) – commands are sent
  strictly sequentially,
- **response-aware pacing** – the next command is only sent after the reply
  frame to the previous one arrived (or after a 300 ms timeout), with a
  minimum spacing of 100 ms,
- an **RX state machine with fixed buffers** – no `String`, no heap in the
  receive path; frames are keyed (`volume=42$`) and dispatched into an
  event callback that updates the HomeKit characteristics.

`rs232_update_on!` (or `display_update_auto!` on Gen 1) puts the amplifier in
push mode, so front-panel and IR changes are reflected in the Home app within
a second.

### Protocol generation auto-detection

For RA-1572/RA-1592 the RS232 dialect depends on the installed amplifier
firmware. At boot the driver runs a non-blocking state machine:

1. Send the Gen 2 power query (`power?`) and wait up to 1 s for a `$` frame.
2. On timeout, send a lone `!` (flushes the partially parsed Gen 2 probe from
   the amplifier's command buffer), then the Gen 1 query
   (`get_current_power!`) and wait for a `!` frame.
3. If both stay silent (amplifier unplugged or powered off), retry every 15 s.

The detected generation is cached in NVS, so detection runs only once per
installation. Changing the model in the dashboard clears the cache.

### HomeKit mapping

| Home app / Remote widget | Amplifier |
|---|---|
| Power tile / power button | `power_on!` / `power_off!` |
| Input picker | source commands (`cd!`, `opt1!`, …) |
| Volume rocker (side buttons) | `vol_up!` / `vol_dwn!` |
| Mute button | `mute_on!` / `mute_off!` |
| **SELECT** | tone control off → enable it; otherwise cycle bass → treble → balance |
| **LEFT / RIGHT** | decrease / increase the selected parameter |
| **BACK** | leave tone mode (re-enable bypass) |
| UP / DOWN | volume up / down |

The currently selected tone parameter is flashed on the amplifier display
using a ±1 "blink trick" that never changes the net value.

Renames, visibility checkboxes and the display order of the input sources are
stored in NVS and survive reboots. For power amps (Michi M8/S5) the accessory
is reduced to a power switch – no inputs, no volume.

### Web dashboard

Reachable at `http://rotel-remote.local` (or the device IP) once Wi-Fi is set
up:

- **Status**: firmware version, selected model, detected protocol generation,
  amplifier state, HomeKit setup code, free heap
- **Open remote**: link to the built-in web remote (see below)
- **Amplifier model**: dropdown with all supported families;
  applying a change stores the selection in NVS, clears the stored
  characteristic values (the input list changes) and reboots
- **HomeKit code**: set a custom 8-digit pairing code (trivial codes are
  rejected, matching HomeSpan's own rules)
- **Device address**: rename the mDNS hostname (`rotel-remote` →
  `martin-remote.local`); the same name is used for the setup access point.
  Stored in NVS, applied after an automatic restart
- **Firmware update**: check + one-click install (HTTP, `latest.json` with
  `{"version": "...", "url": "..."}`)
- **Wi-Fi settings** and **factory reset** (clears Wi-Fi, pairing and all
  preferences)

### Web remote

`http://rotel-remote.local/remote` is a self-contained, mobile-first remote
control that works without any HomeKit pairing – useful for Android users or
guests:

- **Power button**, **source wheel** (native CSS scroll-snap), **volume** with
  mute toggle, and **treble / bass / balance** sliders with a bypass toggle
- Controls that don't apply are dimmed (device in standby, mute active,
  bypass active) or hidden entirely (tone card on models without tone
  control, everything except power on power amps)
- The UI updates optimistically on tap and polls the device state every
  1.5 s to stay in sync with the front panel and IR remote
- **Click coalescing**: rapid +/− taps are debounced client-side and sent as
  a single absolute command (`vol_42!`, `bass_+04!`) – Rotel's documentation
  warns that command bursts can reset the amplifier CPU
- HTTP handlers run in the `async_tcp` task and only push actions into a
  small lock-free ring; the main loop is the sole producer of the RS232 TX
  queue

## Getting started

### Requirements

- Arduino IDE 2.x (or `arduino-cli`) with the **esp32** core (tested with 3.3.x)
- Libraries:
  - [HomeSpan](https://github.com/HomeSpan/HomeSpan) (tested with 2.1.8)
  - ESP Async WebServer + Async TCP
  - WiFiManagerLite
  - GitFirmwareUpdate (+ ArduinoJson)

### Build settings

| Option | Value |
|---|---|
| Board | ESP32C3 Dev Module |
| Partition scheme | **Minimal SPIFFS (1.9 MB APP with OTA)** |
| USB CDC on boot | Enabled |

The sketch ships a `build_opt.h` that disables the MAC suffix in the
setup-AP name (`-DWML_AP_APPEND_MAC=0`).

### First-time setup

1. Flash the firmware and power the board from USB-C.
2. Join the Wi-Fi access point **`Rotel-Remote`** and follow the captive
   portal to enter your Wi-Fi credentials.
3. Open `http://rotel-remote.local` and select your amplifier model
   (default: A12/A14).
4. Connect the DB9 port to the amplifier with a straight-through cable.
5. In the Home app: *Add Accessory* → *More options* → select **Rotel Remote**
   and pair with the default code **466-37-726** (changeable in the
   dashboard).
6. Optional: skip HomeKit entirely and just use the web remote at
   `http://rotel-remote.local/remote`.

### Notes

- HomeKit's HAP server runs on port 8080; port 80 belongs to the dashboard
  and the captive portal. This is transparent to the Home app (mDNS).
- Running two adapters in one network? Rename one via the dashboard's
  *Device address* card so the `.local` hostnames stay unambiguous
  (the setup access point follows the same name).
- The BOOT button (GPIO9) acts as HomeSpan's control button
  (e.g. long-press actions for unpairing); serial CLI is available over USB.

## Project layout

| File | Purpose |
|---|---|
| `Rotel_HomeSpan2023_C3.ino` | HomeKit services, remote-key logic, event sync |
| `RotelCommand.h` | RS232 driver: TX queue, parser, Gen 1/2 command table, auto-detection |
| `RotelModels.h` | Model table (dialect, device type, inputs, limits) + NVS helpers |
| `WebPortal.h` | Wi-Fi onboarding, dashboard, web remote, REST API, OTA, factory reset |
| `build_opt.h` | Compile-time flags for bundled libraries |
| `rotel-rs232-serial.md` | Consolidated Rotel RS232 protocol reference (Gen 1 + Gen 2) |
| `AGENTS.md` | Hardware description of the RS232 adapter board (pinout, constraints) |

## Stability by design

The device is meant to run unattended around the clock:

- No `String`/heap allocation in recurring code paths – fixed `char` buffers
  with `snprintf`/`strlcpy` throughout
- All tables (`const`) live in flash; NVS is only touched at boot and on
  explicit user actions
- Non-blocking everywhere: no `delay()` in the loop, all waits are
  millis()-based and rollover-safe
- Oversized UART RX buffer (1 KB) bridges the blocking phases of HomeKit's
  pairing cryptography
- Automatic re-sync after Wi-Fi reconnects and after the amplifier wakes from
  standby (many models don't answer queries while in standby)

## Credits

- [HomeSpan](https://github.com/HomeSpan/HomeSpan) by Gregg E. Berman – the
  HomeKit implementation this project is built on (MIT license)
- Rotel for publicly documenting their RS232 protocols

## License

MIT – see the license header in the sketch.
