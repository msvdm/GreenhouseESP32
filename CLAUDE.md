# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

Firmware for an ESP32 that heats a greenhouse. It switches a **2200 W heating
element** through a contactor and a circulation fan through a second relay,
using DS18B20 temperature sensors for feedback.

**Treat this as safety-critical.** A bug here does not throw an exception, it
energises 2200 W in a wooden structure full of plants, unattended, for hours.
Bias every judgement call toward the failure-safe option.

## Hardware

Target: **ESP32-WROOM-32E on a 2-channel relay carrier** (10 A / 250 VAC relays,
DC 7–60 V supply, no onboard USB). Relay inputs are **active-HIGH**.

| Signal | GPIO | Notes |
|---|---|---|
| Fan relay (CH1) | 16 | fixed by carrier board |
| Heater contactor (CH2) | 17 | fixed by carrier board |
| Left air sensors | 4 | OneWire, needs 4k7 pull-up |
| Right air sensors | 15 | OneWire, strapping pin — pull-up keeps it HIGH at boot |
| Heater zone sensor | 5 | OneWire, strapping pin — pull-up keeps it HIGH at boot |
| TFT CS / RST / DC | 21 / 22 / 2 | CS and RST were moved off 5/17 when the relays landed there |
| TFT SCLK / MOSI | 18 / 23 | |

**The board has no USB port.** The only serial access is a separate USB-TTL
module on RX/TX/GND. Assume OTA is the normal update path.

## Layout

Single-tasked by design — the Arduino `WebServer` dispatches handlers inline
from `server.handleClient()` in `loop()`, and nothing calls `xTaskCreate`.
**There is no concurrency here.** Do not reintroduce a mutex; an earlier
version had one that protected nothing and whose timeout path silently skipped
the safety-critical heater read.

```
src/main.cpp     wiring only — setup order and the periodic dispatch in loop()
src/config.*     pins, limits, timings, Settings/SystemStats, NVS persistence
src/sensors.*    DS18B20 acquisition and validation. Knows nothing about relays
src/control.*    actuators, fault policy, mode machine — the ONLY relay writer
src/display.*    ST7735 status screen
src/webui.*      static PROGMEM pages + /status JSON
src/ota.*        ArduinoOTA + browser upload, with the safety preamble
src/secrets.h    gitignored. Copy from secrets.h.example
```

## Rules that exist for a reason

**Only `setHeater()` / `setFans()` in `control.cpp` may write a relay pin.**
They own runtime accounting, the cycle history and `lastHeaterOffTime`. An
earlier version turned the heater off in seven places; four of them forgot part
of that bookkeeping, which silently disabled the minimum-off-time interlock on
the two most dangerous paths (30-minute runaway trip and 50 °C critical trip).
If you find yourself writing `digitalWrite(HEATER_RELAY_PIN, ...)` anywhere
else, stop.

**Never compare timestamps as `now - WINDOW`.** Always `(now - then) < WINDOW`.
The cycle limiter used the former and underflowed for the first hour after
every boot, silently disabling the cycles-per-hour cap.

**`MODE_COOLDOWN` is the only representation of "purging".** Do not add a
parallel boolean; there used to be an `inCooldownMode` flag alongside it and the
two drifted.

**Fault response is data, not branching.** `FAULT_POLICY[]` in `control.cpp`
maps each fault to its counter and whether it ventilates. Add a row, don't add
an `if`. Note the deliberate asymmetry: the critical trip does *not* ventilate,
because `heaterCritical` is the fan's own rated ambient.

**Pages are static.** Everything dynamic goes through `/status`, which the UI
polls anyway. Do not render values into HTML server-side — the old code did, and
JavaScript overwrote every one of them two seconds later. Serve with `send_P`,
never build pages with `String`.

**`clampSetpoints()` after any setpoint change and after `loadSettings()`.**
NVS is untrusted input. Individual ranges cannot express `tempMin < tempMax`.

## Working here

- Build: `pio run -e esp32dev`. Always build before claiming something works.
- Serial flash: `pio run -e esp32dev -t upload --upload-port COM5`
- OTA: set `GREENHOUSE_OTA_PASS`, then `pio run -e ota -t upload`
- Platform is pinned to `espressif32@6.12.0` **deliberately**. 7.x moves to
  Arduino core 3.x, which changes `esp_task_wdt_init` and the `WebServer` API.
  Do not bump it casually — this board has no USB recovery path.
- Serial is silent by design once faulted: the fault latches and nothing prints
  until the 5-minute stats log. Silence is not evidence of a hang.
- The USB-TTL module holds EN via DTR/RTS while a serial port is open, so the
  board may appear dead on serial while running perfectly. Check for the WiFi
  AP before concluding anything is wrong.

## Known-unresolved

The **previous** hardware would hard-freeze after long heating runs — display
showing noise, requiring a manual power cycle. The task watchdog never
recovered it, which rules out brownout resets and watchdog reboots (both
auto-recover) and points at flash-access failure or EMI from switching 2200 W.
**No firmware change in this repo addresses that**; it is an electrical problem
(coil snubbing, decoupling, routing). If asked about crashes, do not attribute
them to the control-logic bugs that were fixed — those cause wrong behaviour,
not hangs.

Not yet done, worth suggesting: log `esp_reset_reason()` at boot and surface it
on `/status`, and add a watchdog that does not depend on the CPU being sane.
