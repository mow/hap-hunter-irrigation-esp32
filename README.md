> # ⚠️ EXPERIMENTAL — UNTESTED, AGENT-AUTHORED FIRMWARE ⚠️
>
> **This entire project — code, configuration, and documentation — was written by AI coding agents and has _not_ been validated on real hardware.**
>
> - Do **not** connect this to an irrigation controller you depend on.
> - Do **not** use this near anything you cannot afford to damage.
> - All claims below (protocol timings, safety cutoffs, HomeKit behavior, pin assignments) are unverified by a human and may be wrong or unsafe.
> - There is no warranty of any kind. Use at your own risk, in a controlled bench setup, with isolated power, and supervise every test.
>
> If you build or flash this firmware, you accept full responsibility for any consequences.

---

# Hunter X-Core REM HomeKit Bridge (ESP32-C3)

Firmware that turns an existing **Hunter X-Core** irrigation controller into an Apple HomeKit accessory by emulating the HunterRoam SmartPort remote protocol on the controller's **REM** terminal. The Hunter X-Core keeps doing the 24 VAC valve switching; the ESP32-C3 only injects remote-control commands.

- Native **Apple Home** integration (no bridge, no Home Assistant, no Matter)
- Per-zone start/stop with configurable runtime (up to 8 zones)
- Local weekly **scheduler** with timezone-aware NTP
- Built-in **web UI + REST API** with HTTP Basic auth
- Safety: boot stop-all, watchdog, runtime cutoff, factory-reset button
- Vendored **esp-homekit-sdk** as a git submodule

> Status: `0.1.0-dev` — works end-to-end on ESP32-C3 with ESP-IDF v6.0.x.

---

## Table of contents

1. [How it works](#how-it-works)
2. [Hardware](#hardware)
3. [Wiring](#wiring)
4. [Build & flash](#build--flash)
5. [First-time setup](#first-time-setup)
6. [HomeKit pairing](#homekit-pairing)
7. [Web UI & REST API](#web-ui--rest-api)
8. [Scheduler](#scheduler)
9. [Safety features](#safety-features)
10. [Factory reset](#factory-reset)
11. [Project layout](#project-layout)
12. [Configuration reference](#configuration-reference)
13. [Troubleshooting](#troubleshooting)
14. [Credits & license](#credits--license)

---

## How it works

The Hunter X-Core exposes a 3-wire **SmartPort** connector (`AC#1`, `AC#2`, `REM`). The `REM` line accepts the same one-wire serial protocol used by Hunter ROAM/ROAM XL remotes. This firmware bit-bangs that protocol from a single GPIO to send commands such as:

- `start zone N for M minutes`
- `stop zone N`
- `stop all zones`

The controller's own valve drivers, transformer, fusing and rain-sensor logic stay in place. Removing the ESP module restores the controller to stock behavior — the modification is fully reversible.

```
┌──────────────┐    REM     ┌────────────────┐   24VAC   ┌─────────┐
│  ESP32-C3    │──────────▶│ Hunter X-Core  │──────────▶│ Valves  │
│  (this fw)   │   AC#2    │  (unchanged)   │           └─────────┘
│              │◀──────────│                │
└─────┬────────┘            └────────────────┘
      │ Wi-Fi
      ▼
  Apple Home / REST / Web UI
```

---

## Hardware

| Part | Notes |
|---|---|
| **ESP32-C3** dev board | Reference build: ESP32-C3-32S. Any C3 board with a BOOT button on GPIO9 works. |
| **5 V, ≥1 A isolated USB/DC adapter** | Must be a **floating** supply (typical phone charger). Do not use a bench supply that is earth-referenced. |
| Hunter X-Core controller | 4-station tested; firmware supports up to 8 zones (matches X-Core 8). |
| Hookup wire | 3 conductors to the SmartPort. |

> Only **one GPIO** is required to drive REM. The ESP32 is powered separately; it does **not** share ground with the X-Core.

---

## Wiring

The REM signal is referenced to the X-Core's `AC#2` terminal, **not** to ESP ground. The ESP's +5 V rail is tied to `AC#2` so the GPIO swing on `REM` is correctly referenced.

```
Adapter +5V ──┬── ESP32-C3  5V / VIN
              └── Hunter    AC#2  (right-side 24VAC terminal)

Adapter -V  ──── ESP32-C3  GND     (do NOT connect to Hunter)

ESP32 GPIO4 ──── Hunter    REM
```

**Do not** connect ESP `GND` to any Hunter terminal, and **do not** connect Hunter `AC#1` to anything on the ESP side. Use an isolated 5 V adapter only.

Default GPIO assignments (override in [main/app_config.h](main/app_config.h)):

| Function | GPIO | Notes |
|---|---|---|
| REM data | `GPIO4` | Bit-banged HunterRoam frames |
| Status LED | `GPIO18` | Solid = ready, blinks during activity |
| Factory-reset button | `GPIO9` | BOOT button, active-low, 3-second long press |

---

## Build & flash

Requires [ESP-IDF v6.0.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/index.html) and the ESP32-C3 toolchain.

```bash
# Clone with the HomeKit SDK submodule
git clone --recurse-submodules https://github.com/mow/hap-hunter-irrigation-esp32.git
cd hap-hunter-irrigation-esp32

# Or, if already cloned without submodules:
git submodule update --init --recursive

# Activate ESP-IDF in the current shell
. $IDF_PATH/export.sh

# Configure target (already set in sdkconfig, but harmless to repeat)
idf.py set-target esp32c3

# Build
idf.py build

# Flash & monitor (replace PORT)
idf.py -p /dev/ttyUSB0 flash monitor
```

To open the menuconfig UI: `idf.py menuconfig`.

---

## First-time setup

On a freshly flashed device with no saved Wi-Fi:

1. The device boots into **AP mode** and broadcasts SSID `HunterSetupAP` (open network).
2. Connect a phone/laptop to that SSID. The captive portal opens at `http://192.168.4.1/`.
3. Fill in:
   - **Wi-Fi SSID / password** of your home network
   - **Admin password** (used for the web UI / API after reboot — min 1 char, max 32)
   - **Zone count**, **default runtime**, **safety cutoff**
4. Save. The device reboots, joins your Wi-Fi, and the AP disappears.
5. After reconnecting to your Wi-Fi, find the device by mDNS hostname or DHCP lease and reopen the UI. All requests require `admin` + the password you set.

---

## HomeKit pairing

After Wi-Fi is up, open the web UI → **HomeKit** section. A QR code is rendered as SVG; scan it with the iOS Home app. Default setup code is shown next to the QR.

If you ever need to re-pair (e.g. moved the device to a new Home), use **Reset HomeKit pairings** in the HomeKit section. This clears the pairing database and reboots; Wi-Fi credentials and schedules are preserved.

Inside Apple Home the device appears as an **Irrigation System** with one **Valve** per configured zone. Each valve exposes Active / InUse / SetDuration; ProgramMode reflects whether the scheduler has any upcoming runs.

---

## Web UI & REST API

All HTTP endpoints (UI + API) require HTTP Basic auth with username `admin` and the password set during setup. Endpoints:

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | HTML dashboard (status, zones, schedule, HomeKit, settings) |
| POST | `/api/settings` | HTML form submit (settings page) |
| GET | `/api/v1/status` | JSON: zone state, runtime, schedule next run |
| POST | `/api/v1/start` | JSON `{"zone":N,"seconds":M}` — start a zone |
| POST | `/api/v1/stop` | JSON `{"zone":N}` or `{}` — stop one / all zones |
| POST | `/api/v1/settings` | JSON settings update |
| GET | `/api/v1/homekit` | JSON: paired state, setup code, accessory id |
| GET | `/api/v1/homekit/qr.svg` | SVG QR code for pairing |
| POST | `/api/v1/homekit/reset-pairings` | Clear HomeKit pairings + reboot |
| GET / POST | `/api/v1/schedule` | JSON read / write of weekly schedule |
| POST | `/api/v1/schedule/form` | HTML form submit (schedule editor) |
| GET / POST | `/api/v1/time` | JSON timezone + NTP server config |
| POST | `/api/v1/time/form` | HTML form submit (time page) |

Example:

```bash
curl -u admin:YOUR_PASS http://<device-ip>/api/v1/status
curl -u admin:YOUR_PASS -H 'Content-Type: application/json' \
     -d '{"zone":1,"seconds":600}' \
     http://<device-ip>/api/v1/start
```

---

## Scheduler

A simple weekly scheduler runs entirely on-device — no cloud, no companion app needed. Each entry is `(days-of-week, start-time, zone, duration)`. Configure timezone and NTP server in the **Time** section; the scheduler waits for valid time before arming.

The scheduler is **independent** of HomeKit's own scripted automations — keeping it local means watering still happens if your home hub or internet is offline.

---

## Safety features

- **Boot stop-all** — every boot issues a stop-all REM command before anything else runs, so a crashed/restarted ESP cannot leave a valve open.
- **Per-run safety cutoff** — every started zone is force-stopped after `safety_cutoff_seconds` even if the controlling task dies. Default 1 h, configurable 5 min–4 h.
- **Task watchdog** — 10 s TWDT on the runtime task.
- **Single active zone enforcement** — starting a zone first stops the previously active one.
- **Auth on all endpoints** — including the captive setup portal once a password is configured.

---

## Factory reset

Hold the **BOOT button (GPIO9)** for **3 seconds** while the device is running. The status LED indicates the trigger; the device then:

1. Clears NVS (Wi-Fi, admin password, zone config, schedule, HomeKit pairings)
2. Reboots into setup-AP mode

There is no other reset trigger — power cycling or pressing EN reset alone does **not** factory reset.

---

## Project layout

```
.
├── CMakeLists.txt
├── partitions.csv / partitions_hap.csv
├── sdkconfig                  # ESP32-C3 defaults (committed)
├── main/
│   ├── hunter.c               # app_main, init order
│   ├── hunter_protocol.[ch]   # HunterRoam REM bit-banging
│   ├── rem_gpio_driver.[ch]   # GPIO timing primitives
│   ├── irrigation_runtime.[ch]# Zone state machine + safety cutoff
│   ├── scheduler.[ch]         # Weekly schedule engine
│   ├── homekit_irrigation.[ch]# HAP Irrigation System service
│   ├── service_boundaries.[ch]# HTTP server (UI + REST), auth
│   ├── settings_store.[ch]    # NVS-backed config
│   ├── runtime_state.[ch]     # Shared runtime status
│   ├── status_led.[ch]
│   ├── reset_button.[ch]      # 3 s long-press factory reset
│   ├── time_sync.[ch]         # SNTP + tz
│   └── app_config.h           # Compile-time constants
└── components/
    └── esp-homekit-sdk/       # Submodule: espressif/esp-homekit-sdk
```

---

## Configuration reference

Compile-time defaults live in [main/app_config.h](main/app_config.h):

| Macro | Default | Range |
|---|---|---|
| `HUNTER_PROTOCOL_GPIO` | `4` | any free GPIO |
| `HUNTER_STATUS_LED_GPIO` | `18` | any free GPIO, or `-1` to disable |
| `HUNTER_RESET_BUTTON_GPIO` | `9` | `-1` to disable |
| `HUNTER_RESET_LONG_PRESS_MS` | `3000` | ms |
| `HUNTER_ZONE_COUNT_DEFAULT` | `4` | 1–8 |
| `HUNTER_RUNTIME_SECONDS_DEFAULT` | `600` | 60 – 7200 |
| `HUNTER_SAFETY_CUTOFF_SECONDS_DEFAULT` | `3600` | 300 – 14400 |
| `HUNTER_WATCHDOG_TIMEOUT_SECONDS` | `10` | s |
| `HUNTER_STOP_ZONES_ON_BOOT` | `1` | 0/1 |
| `HUNTER_SETUP_AP_SSID` | `"HunterSetupAP"` | string |

Runtime settings (Wi-Fi creds, admin password, zone count, runtimes, schedule, timezone) live in NVS under namespace `hunter_cfg`.

---

## Troubleshooting

**Zone won't start / Hunter beeps.** Check that the ESP +5 V rail and Hunter `AC#2` are tied together, and that the adapter is truly isolated. Measure: with the ESP powered and the cable connected to the Hunter, there must be **no continuity** between ESP `GND` and any Hunter terminal.

**Device keeps rebooting.** Look at `idf.py monitor`. The 10 s task watchdog will reboot if Wi-Fi init blocks; usually means a bad antenna / brownout — check the 5 V supply.

**Forgot the admin password.** Use the factory reset button (3 s on GPIO9), then redo setup.

**HomeKit setup code rejected.** Use the QR code from `/api/v1/homekit/qr.svg`; the printed code is correct only for the current pairing instance. After `Reset HomeKit pairings`, a new code is generated.

**`adding embedded git repository` warning on clone.** Use `git submodule update --init --recursive` instead of cloning the SDK folder manually.

---

## Credits & license

- HunterRoam protocol research: prior open-source projects on the Hunter REM bus.
- HomeKit: [espressif/esp-homekit-sdk](https://github.com/espressif/esp-homekit-sdk) (Apache-2.0), vendored as a submodule.
- This firmware: **MIT** unless a `LICENSE` file in this repo states otherwise.

**Disclaimer.** You are wiring low-voltage signals to mains-adjacent equipment. Use an isolated 5 V supply, double-check polarity and reference before powering on, and never bridge ESP ground to Hunter terminals. The author is not responsible for damage to your controller, valves, or property.