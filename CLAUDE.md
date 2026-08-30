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
| TFT SCLK / MOSI | 18 / 23 | VSPI defaults, not named in code — the constructor takes neither |
| IO0 button | 0 | active-LOW, factory reset on a 5 s **runtime** hold |
| Onboard status LED | 23 | **unusable** — shares TFT MOSI, so it flickers with SPI traffic |

**GPIO 0 is a strapping pin.** Held LOW through a reset it selects the serial
bootloader, so a hold-at-boot reset would never reach this firmware — the
factory reset is therefore a *runtime* hold, read in `loop()` once the
bootloader has handed over. Readings are unreliable while a USB-TTL module is
attached, because its auto-reset circuit drives this pin.

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
src/net.*        AP + optional station, and the WiFi trial/commit machinery
src/auth.*       web UI password, sessions, Host allowlist — the ONLY gate
src/button.*     IO0 runtime hold → factory reset. Never touches a relay
src/webui.*      static PROGMEM pages + /status JSON
src/ota.*        ArduinoOTA + browser upload, with the safety preamble
src/secrets.h    gitignored. FACTORY values only; live WiFi config is in NVS
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

**A purge is finite; a fault is not.** Two rules in `enterFault()`, both of which
exist because their absence stranded the fan on permanently:

- `residualHeat` asks whether the element ran *recently* - within
  `FAN_COOLDOWN_TIME` - not whether it has ever run this boot. Testing
  `lastHeaterOffTime != 0` meant every fault for the rest of the boot re-opened
  a purge for an element that went cold hours ago.
- The re-assertion path ends the purge once `FAN_COOLDOWN_TIME` has elapsed.
  While a sensor fault is latched, the gate at the top of `controlSystem()`
  returns before `decideMode()`, which is what normally times COOLDOWN out - so
  nothing else can ever end it. With a probe that never comes back, that was
  forever.

**Pages are static.** Everything dynamic goes through `/status`, which the UI
polls anyway. Do not render values into HTML server-side — the old code did, and
JavaScript overwrote every one of them two seconds later. Serve with `send_P`,
never build pages with `String`.

**Three credentials, three distinct jobs. Never merge them.**

| Secret | Default | Stored | Guards |
|---|---|---|---|
| AP WPA2 | SSID `Green` / `ChangeME` | NVS, seeded from `secrets.h` | joining the radio |
| Web UI password | `GreenAdmin` | NVS, **salted SHA-256** | the whole browser surface |
| `OTA_PASSWORD` | its own value | compiled into `secrets.h` | `espota` — break-glass |

The WiFi password is a link-layer secret shared with every device that joins;
the web password must not be, and the two must never be the same string. There
used to be a fourth (`upd_pass`, an NVS password just for the browser upload,
empty by default) — it is gone, and the upload is guarded by the web session
like everything else.

`OTA_PASSWORD` stays compiled in **on purpose**: losing the web UI must never
lock you out of a board with no USB port. Do not make it web-editable.

**`requireAuth()` in `auth.cpp` is the only gate, and it is called at the top of
the handler.** It does two things: a **Host allowlist** (only the AP IP, the STA
IP and `<hostname>.local` — this is what stops DNS rebinding) and a session
lookup. Do not scatter equivalent checks; add the call.

**Switching an output OFF never requires a session.** `handleOutput()` reads
`on` *before* the gate and passes `needSession=on`. Every failure mode of the
auth code — expired cookie, forgotten password, a bug in `auth.cpp` — must still
leave a way to shed 2200 W. Note `argIsOn()` treats anything that is not `"1"`
as off, so a malformed unauthenticated request can only ever move an output in
the safe direction.

Two things this exemption is deliberately *not*: it does not extend to leaving
manual mode (`/manual`), because handing the element back to the controller may
well start heating; and it does nothing in automatic mode, where the 409 stands
because the controller owns the relay and would re-assert it on the next pass
anyway. Manual is the mode where the interlocks are stripped, which is precisely
why the unauthenticated off-switch belongs there.

**The upload path calls `authCheck()`, not `requireAuth()`, on
`UPLOAD_FILE_START`** — before `otaPrepare()` and `Update.begin()`, so a refused
upload does not shed the heater, release the watchdog or open the flash
partition. `authCheck()` because nothing can be sent from inside a body
callback; `handleUpdateResult()` sends the 401 afterwards. The credential is a
**header** (the session cookie) precisely because headers are parsed before the
body: a form field would not arrive until after the image had been written.

**`collectHeaders()` must be called exactly once.** It REPLACES the list rather
than appending, so a second call silently drops `Cookie` and logs everyone out.
`authBegin()` owns it; `ota.cpp` deliberately does not call it.

**CSRF is handled by `SameSite=Strict` on the session cookie**, not by tokens.
Before that existed, every state-changing endpoint was reachable from any page
open in a browser on the same network: `fetch(url, {method:'POST'})` with no
custom header and no body is a CORS *simple request*, so nothing preflights it.
If you add an endpoint that changes something, it needs the gate for that reason
alone.

**The IO0 button restores credentials and settings. It never touches a relay.**
A factory reset is a credential operation, not an emergency stop — conflating
the two would put a second relay writer in the firmware. `button.cpp` resets the
web password, the AP configuration and the setpoints, and deliberately preserves
the statistics: they are a maintenance record, nothing about them can lock
anybody out, and `/stats` has its own reset. The AP change is committed
immediately rather than going on trial, because the trial exists to protect
someone configuring the board remotely and whoever is holding the button is
standing in front of it.

**Bump the `?v=` on `/style.css` and `/ui.js` whenever either changes.** They
once carried a 24-hour `max-age`, and after an OTA browsers kept serving the old
stylesheet against the new HTML - no `.switch`, no `.hidden`, so every element
meant to be styled or concealed appeared raw. It reads as a catastrophically
broken UI rather than a cache. `no-cache` on the response fixes it for the
*next* update, never the one already in someone's browser; only changing the URL
does that. Watch the `StaticJsonDocument` capacity in `handleStatusJSON()` when
adding fields too - a truncated `/status` takes the whole UI down with it.

**Manual mode adds no controls.** The existing badges and readouts become the
controls, gated on `.tap`/`.simtap`. Resist adding a manual-only card: an earlier
version had two, and they were both clutter and a second thing to keep in sync
with the automatic UI. Cards that do nothing in manual are dimmed with `.card.off`
rather than hidden, so the page does not reflow when the mode switch is flipped.

**Manual mode is a TEST mode and its protections are deliberately stripped.**
Since v6.2.0 the heater and fan relays are independent switches with no airflow
proving, no heater-zone arm margin, no `heaterMax` shed, no thirty-minute runtime
cap and no sensor fault gates. `driveManual()` energises the element on request.
This was asked for explicitly, and it is not an oversight to be tidied up.

Exactly two things survive, and both must:

- **The critical trip** at `settings.heaterCritical` in `safetyTick()`, which
  still routes through `enterFault(FAULT_HEATER_CRITICAL)` and still does not
  ventilate. `driveManual()` refuses a request while that fault is latched, so
  clearing it needs a real reading below `heaterMax` and a second deliberate press.
- **`MANUAL_BLIND_HEAT_LIMIT`**, the clock in `safetyTick()` that runs when the
  element is on and the zone probe is unreadable. That is the one state where the
  critical trip cannot fire, so nothing at all is limiting 2200 W; five minutes is
  how long that is allowed to last. `blindSince` resets on a real reading and on
  every manual-mode transition - a stale timestamp would shed the *next* run on
  the spot, which looks exactly like a protection misfiring.

Faults other than the critical trip stay LATCHED through manual - manual ignores
them rather than erasing them. `driveManual()` refuses only on
`FAULT_HEATER_CRITICAL`, and the web UI gates the heater badge on the `crit`
field in `/status` rather than on `fault`. An earlier attempt cleared the latch
on entering manual instead; that made every manual round trip re-raise the same
condition on the way back to automatic, and each one counted as a fresh safety
shutdown - exactly the statistics inflation the transition guard in
`enterFault()` exists to prevent.

**`clampSetpoints()` after any setpoint change and after `loadSettings()`.**
NVS is untrusted input. Individual ranges cannot express `tempMin < tempMax`.
`validateWifiConfig()` is the same rule for the network settings, and is applied
to form input as well as to NVS.

**Simulated readings may only ever make the controller more cautious.** The air
override is free in both directions - air decides whether to *want* heat, and
every protection sits downstream of that. The heater-zone override goes through
`max(real, simulated)` and does not apply at all when the real probe is
unreadable, so it can trip the protections early but can never report the
element cooler than it is. Remove that `max()` and a typed-in 20 C silently
deletes the critical trip. If you add another simulated input, work out which
side of this line it falls on before you write it.

**Manual intent and current mode are separate facts, not duplicates.**
`settings.manualMode` is what the operator asked for, and is persisted;
`MODE_MANUAL` is what the machine is doing right now, and is not. They diverge
whenever a fault is active - `enterFault()` drives the mode to IDLE or COOLDOWN,
and `decideMode()` returns to MODE_MANUAL once the condition clears. This is not
the `inCooldownMode` mistake above: that was two names for one fact, this is a
setpoint and a temperature.

**Manual mode persists across a reboot. Output requests never do.** Manual
heater and fan requests, and both simulated values, start clear on every boot.
A power blip must not be able to restore a latched heater relay unattended.
Simulation additionally expires after `SIM_TIMEOUT`, because unlike manual mode
it is invisible from across the greenhouse.

**Web handlers set requests; `control.cpp` decides.** The manual endpoints are
not an exception to the relay-writer rule - `/output` records what the operator
asked for, and `driveManual()` decides. That stays true even though manual now
has very little left to decide: `setManualHeater()` and `setManualFan()` record
the request and then call `driveManual()` themselves, so a tap moves the relay at
once instead of up to five seconds later, and there is still exactly one function
that touches a relay in manual. Every bypassed limit is still RECORDED -
`setHeater()` stamps `lastHeaterOffTime` and the cycle history regardless - so
automatic re-applies the five-minute rest and the cycles-per-hour cap the moment
it takes over.

**The WiFi trial covers the access point only.** `netApplyAp()` and
`netFactoryReset()` go on trial because they can lock the operator out of the
board; `netApplySta()` applies and commits immediately because it cannot. The old
code put everything on trial through one `applyRadio()` that tore the AP down on
every change, so joining a home network cost the operator their access point and
a 60-second confirmation for nothing. The keep/revert prompt lives on the CONTROL
page, driven by `trial` in `/status`, because that is where someone lands after
reconnecting.

**`net.cpp` owns mDNS; `ota.cpp` is told not to start it.** `ArduinoOTA::begin()`
calls `MDNS.begin()` internally, so without `setMdnsEnabled(false)` the two race
over one responder and the configured hostname sometimes loses. `net.cpp` must
therefore call `MDNS.enableArduino(3232, true)` itself - dropping it removes
espota discovery from a board with no USB recovery path.

**Any WiFi scan must be asynchronous.** `WiFi.scanNetworks(true, false)`, polled
through `netScanState()`. A blocking scan parks `loop()` for two to four seconds,
which stalls the 1 Hz heater-zone read and `safetyTick()` with it. The 20 s task
watchdog does not catch that - it is simply four seconds of an unsupervised
element. Note that `scanNetworks()` calls `WiFi.enableSTA(true)` internally, so a
scan works with the station side switched off and leaves the radio in AP_STA.

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

### A malformed POST to `/update` stalls `loop()` and trips the watchdog

**Reproduced on hardware, v7.0.1, 2026-08-30.** A single unauthenticated
`curl -X POST http://<board>/update` with no multipart body makes the board stop
serving for ~18 s — every concurrent `/status` times out — and then reboot.
Confirmed by watching the uptime reset and `shutdowns` increment.

The stall is in the Arduino `WebServer` library, before any handler of ours
runs, so no gate in `auth.cpp` can prevent it:

- `Parsing.cpp:101` — `if (!isForm && _currentHandler && _currentHandler->canRaw(...))`.
  `canRaw()` is true for any route registered with an upload handler, which is
  exactly and only `/update`. Other POST routes answer 401 instantly.
- That path then waits for a request body that never arrives.
- `HTTP_MAX_POST_WAIT` (`WebServer.h:50`) is **not** `#ifndef`-guarded, so it
  cannot be raised or lowered with a `build_flag`.

Why it matters here rather than being a curiosity: the stall parks `loop()`, so
the 1 Hz heater-zone read and `safetyTick()` stop with it. The failure mode is
safe-ish — the watchdog reboot lands in `controlBegin()` with the relays off —
but an element can run unsupervised for the length of the stall, and any
unauthenticated device on the network can trigger it repeatedly.

`uploadStarted` in `ota.cpp` is a *different*, adjacent fix and does not address
this. Do not confuse the two.

Options, none taken yet: drop the upload handler and read the image from the raw
body; move the browser upload to a second `WebServer` on another port so a stall
cannot touch the control path; or vendor a patched `Parsing.cpp`. All three are
real work and the platform pin makes the last one unattractive.
