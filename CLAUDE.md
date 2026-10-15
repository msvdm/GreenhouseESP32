# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

ESP32 greenhouse thermostat (firmware 8.x). Two DS18B20 sensor lines → room
average → heater contactor (relay 2, GPIO 17) and ceiling fans (relay 1,
GPIO 16). ST7735 status screen, password-protected web UI, OTA updates. See
README.md for hardware and behaviour.

8.0.0 was a deliberate rewrite. The 7.x firmware drove a stove element and a fan
mounted together (a DIY fan heater), and carried airflow proving, runtime caps,
cycle limits, cooldown purges, fault latching, simulation and a WiFi trial
system for that. The hardware is now ceiling fans plus off-the-shelf radiant
heaters, and the user asked for all of that to be removed. **Do not bring it
back** without being asked.

## Layout

```
src/config.h       pins and timing constants, FW_VERSION
src/settings.*     NVS: setpoints, WiFi, relay runtime totals
src/sensors.*      probe discovery (re-scanned every 30 s) and averaging
src/control.*      thermostat + manual override - the ONLY relay writer
src/display.*      ST7735 screen, drawn into a GFXcanvas16 and pushed whole
src/led.*          onboard LED (GPIO 23) status patterns
src/net.*          AP (always on) + home WiFi, mDNS, ArduinoOTA (espota)
src/auth.*         web password (salted SHA-256) and session cookies
src/web.*          routes, JSON API, browser firmware upload
src/web_pages.h    static PROGMEM HTML/CSS/JS
src/secrets.h      gitignored; factory AP, UI password, OTA_PASSWORD
```

Single-threaded: everything runs from `loop()`; `WebServer` handlers run inline.

## Rules

- **Only `control.cpp` writes the relay pins.** Web handlers set
  `ctrl.heaterMode` / `ctrl.fanMode` or settings, then call `controlUpdate()`.
- **NVS keys are shared with 7.x on purpose** (namespace `greenhouse`:
  `wifi_sta_en`, `sta_ssid`, `sta_pass`, `ap_ssid`, `ap_pass`, `host`,
  `ui_salt`, `ui_hash`, `ui_def`). Renaming them would drop the board off the
  network or lose the password after an OTA. New 8.x keys: `heat_on`,
  `cool_on`, `hyst`, `room_heat`, `rt_heat_s`, `rt_fan_s`.
- **Room average = mean of line averages**, not of all probes. This is what the
  user asked for: the number of probes per line must not matter.
- **Manual overrides are never persisted.** Every boot starts in Auto, relays off.
- **Validate untrusted numbers with `numberArg()`** in `web.cpp`, never
  `String::toFloat()`, which turns junk into 0.
- **Pages are static**; live data comes from `/api/status`. The stylesheet and
  script URLs carry `?v=FW_VERSION`, so **bump `FW_VERSION` whenever
  `web_pages.h` changes**, otherwise browsers keep the cached copy for a day.
- **`collectHeaders()` is called once, in `authBegin()`.** A second call
  replaces the list and drops `Cookie`.
- **The TFT is on remapped VSPI pins** (SCK 18, MOSI 26, CS 25, DC 27, RST 33)
  so GPIO 23 is free for the LED. `SPI.begin(sck, -1, mosi, -1)` must run before
  `tft.initR()`. Do not reuse GPIO 19: the ESP32 core attaches it as MISO even
  when -1 is passed.
- **Long blocking work must feed the watchdog.** The browser upload and espota
  transfers run inside a single `handleClient()` / `ArduinoOTA.handle()` call,
  which is why both call `esp_task_wdt_reset()` per chunk.

## Working here

- Build: `pio run -e esp32dev` (PlatformIO is installed system-wide).
- Deploy: browser upload at `/update`, or `pio run -e ota -t upload` with
  `GREENHOUSE_OTA_PASS` set. espota needs the Windows firewall to allow the
  board's connection back to the PC.
- Device on the LAN: `http://192.168.0.102` (password `GreenAdmin` unless changed).
- Platform pinned to `espressif32@6.12.0` (Arduino core 2.0.x). 7.x moves to
  core 3.x with different watchdog/WebServer APIs; do not bump it casually, as
  the board has no USB.
- Known library quirk: a POST to `/update` with no multipart body makes the
  Arduino `WebServer` wait for a body that never comes (~18 s). Relays keep
  their state during the stall; at worst the watchdog reboots the board, and
  relays come back off.
