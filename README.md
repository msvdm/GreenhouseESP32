# 🌱 GreenhouseESP32

**A very small computer whose entire job is making sure the tomatoes don't freeze.**

This is the firmware for a homemade greenhouse climate controller. It watches
the air temperature with a handful of DS18B20 sensors, and when things get
chilly it fires up a 2200 W heating element. When things get too toasty, it
blows air around instead. In between, it mostly sits there being quietly
paranoid about the heater.

There's a tiny screen, a web interface you can pull up on your phone from the
greenhouse doorway, and — because the production board has no USB port at all —
the ability to update its own firmware over WiFi.

---

## What it actually does

Seven temperature sensors, three separate OneWire buses, two relays, one
stubborn opinion about not burning anything down.

**The control loop is boring on purpose.** It sits in one of five states:

| State | What's happening |
|---|---|
| 🟦 `IDLE` | Temperature is fine. Everything off. The good state. |
| 🔥 `HEATING` | Too cold. Fans first, *then* heat. Never the other way round. |
| ❄️ `COOLING` | Too warm. Fans only. |
| 💨 `COOLDOWN` | Heater just stopped. Fans keep running to purge the residual heat. |
| 🔧 `MANUAL` | You're driving. The interlocks are still driving too. |

The interesting part isn't the heating, it's everything that says *no*:

- 🌬️ **Fans must be proven running for 5 seconds** before the element can
  energise. Not "we asked them to" — actually running, timed from the moment the
  relay closed.
- ⏱️ **30-minute maximum continuous run**, then a forced cooldown.
- 😴 **5-minute minimum rest** between heating cycles.
- 🔁 **6 cycles per hour, maximum**, on a sliding window.
- 🌡️ **Two separate over-temperature limits** — one sheds the heater, one sheds
  everything.
- 🚫 **No valid sensor reading, no heating.** It would rather be cold than blind.

If any of that trips, it shuts down, writes down what happened, and quietly
tries again once the world makes sense. Every safety event is counted and
survives a reboot, so you can pull up `/stats` and see exactly how paranoid it's
had to be.

---

## ⚠️ Please read this bit

**This firmware was written by Claude (Anthropic's AI), and the hardware was
built and tested by an enthusiastic hobbyist — not by a licensed electrician,
and not by an industrial automation professional.**

That is a genuinely important sentence and it's not false modesty. This thing
switches **2200 watts of mains-voltage heating** through a contactor. Done
badly, that is a fire, and no amount of tidy C++ changes that.

So, honestly:

- 🔌 **The mains side is the dangerous part, and this repo is not the mains side.**
  Contactor sizing, wiring, fusing, earthing, enclosure, isolation — get someone
  qualified to look at it. Please.
- 🧪 **It has not been through any certification.** No UL, no CE, no safety
  audit. It's a hobby project that happens to be written carefully.
- 🐛 **The previous hardware revision used to hard-freeze after long heating
  runs.** That turned out to be electrical (almost certainly EMI or supply
  disturbance from switching the load), not a firmware bug — and it is *not*
  fully solved. If you build one of these, assume it can lock up, and don't let
  it be the only thing standing between a heater and a fire.
- 🔧 **Manual mode is a test tool, not a feature.** It keeps every interlock
  that protects the hardware, but it exists so you can prove the wiring works,
  not so you can run the greenhouse by hand.
- 🔥 **Please put a real thermal cutout in the circuit.** A physical,
  mechanical, doesn't-run-any-code thermostat in series with the element. If the
  firmware is the only thing that can turn the heater off, the design is wrong.
  Software should be the *convenience* layer, not the safety layer.

Use it, fork it, learn from it, improve it. Just don't trust it more than it has
earned. 🙂

---

## 🛠️ Hardware

- **ESP32-WROOM-32E** on a 2-channel relay carrier board (10 A / 250 VAC, 7–60 V DC in)
- **7× DS18B20** temperature sensors on three OneWire buses — left air, right
  air, and one watching the heater outlet
- **ST7735 160×128 TFT** for at-a-glance status
- **2200 W heating element** via contactor, and a 220 V AC circulation fan

| Signal | GPIO |
|---|---|
| Fan relay | 16 |
| Heater contactor | 17 |
| Left / Right / Heater sensor bus | 4 / 15 / 5 |
| TFT CS / RST / DC / SCLK / MOSI | 21 / 22 / 2 / 18 / 23 |

Each OneWire bus wants its own 4.7 kΩ pull-up to 3V3.

---

## 🚀 Getting started

```bash
git clone https://github.com/msvdm/GreenhouseESP32.git
cd GreenhouseESP32
cp src/secrets.h.example src/secrets.h    # then edit it
pio run -e esp32dev -t upload
```

The board comes up as a WiFi access point:

| | |
|---|---|
| **Network** | `Green` |
| **WiFi password** | `ChangeME` |
| **Web password** | `GreenAdmin` |
| **Address** | http://192.168.4.1 |

**Two passwords, because they do two different jobs.** The WiFi password lets a
device onto the radio, and every phone that has ever joined has a copy of it.
The web password is what actually guards the controls, the settings page and the
firmware upload — so it should *not* be a value you hand out to get someone
online.

**Change both.** Neither is a secret; they are printed above. Anyone in radio
range of an unchanged board can switch on a 2200 W element. The interface nags
until the web password is changed, and both take about thirty seconds on the
Settings page.

Readings stay visible without logging in, and **a manually-energised heater can
always be switched off without a password** — no expired session or forgotten
password can leave the element stranded on. A login is needed to switch anything
*on* or to change a setting.

In automatic mode that exemption does not apply and does not need to: the
controller owns the relay, and every interlock — the 30-minute runtime cap, the
5-minute minimum off-time, the `heaterMax` shed and the 50 °C critical trip — is
active. Manual mode is the one where those are deliberately stripped, which is
exactly why the unauthenticated off-switch lives there.

Locked out? Hold the **IO0 button on the board for five seconds**. The TFT
counts down, and everything goes back to the defaults in the table above. See
[Recovery](#-recovery) below.

---

## 🔧 Manual mode

There's an iOS-style switch at the right-hand end of the **System Status** row.
Flip it and the whole page turns light grey — that's deliberate, and it's the
mode indicator. You should be able to tell from across the greenhouse that the
controller has stopped looking after itself.

Manual mode doesn't add any controls. **The readouts you already have become the
controls**, and a blue outline marks what's become tappable:

| Tap this | To do this |
|---|---|
| The `Heater` badge | switch the element |
| The `Fans` badge | switch the circulation fan |
| The big average temperature | type a simulated air temperature |
| The heater-zone figure | type a simulated heater-zone temperature |

The `Mode` badge tells you what's happening, including *why* something didn't
happen — `MANUAL - proving airflow`, or the name of an active fault.

### What manual mode does *not* turn off

This is the part worth reading twice. Manual mode replaces the *thermostat*, not
the *safety system*:

| Interlock | In manual | |
|---|---|---|
| Fans proven running 5 s before the element energises | ✅ **still armed** | tapping the heater starts the fans itself and waits |
| Heater-zone limit sheds the element | ✅ **still armed** | |
| Critical shutdown sheds everything | ✅ **still armed** | |
| Valid heater-zone reading required | ✅ **still armed** | |
| 30-minute maximum continuous runtime | ✅ **still armed** | |
| 5-minute minimum rest between cycles | ⛔ bypassed | it's a test, you shouldn't have to wait |
| 6 cycles per hour | ⛔ bypassed | same |

The two bypassed limits are still **recorded**. Manual cycles go into the same
history as automatic ones, so the moment you switch back to automatic, both
limits apply again and count everything you just did.

If a safety path takes the element away from you — the 30-minute cap, an
over-temperature, a sensor dropping out — the heater request drops itself. It
does not re-arm when the condition clears. You have to ask again.

### Two things that will surprise you

**A fault outranks manual mode.** With no heater-zone probe on GPIO 5 the
controller faults, and the element cannot be energised by any route — the `Mode`
badge turns red and names the fault, and tapping the heater tells you the same
thing. The **fan** stays tappable in that state, so you can still prove that half
of the wiring. This is the correct trade-off for 2200 W, but it means "test the
heater relay on a bare bench" is not something this feature will let you do.

**Manual mode survives a reboot; the outputs don't.** The switch position is
saved. The relays are not — after a power cut the board comes back in manual mode
with everything **off**. A power blip should never be able to restore a latched
heater relay with nobody watching.

---

## 🎭 Simulated sensors

Tap either temperature readout to feed the controller an invented value, so you
can watch the state machine react without standing outside waiting for weather.
A simulated reading shows in orange as `4.0C SIM`; clearing the box puts the real
sensor back. This works in **automatic mode as well as manual** — watching the
controller decide by itself that it wants heat is most of the point, and it can't
do that while manual mode is holding it still.

There is exactly one rule, and it's enforced in `control.cpp` rather than in the
UI:

> **Simulation may only ever make the controller more cautious, never less.**

- **Air temperature** overrides freely, in either direction. Air isn't a safety
  input — it answers "should we want heat?", and every protection that matters
  sits downstream of that answer.
- **Heater zone** overrides **upwards only**. The value the safety logic sees is
  `max(real, simulated)`, so a simulated reading can trip the protections early
  — which is exactly what you want to be able to test — but can never report the
  element cooler than it actually is. And if the real probe isn't readable, the
  override doesn't apply at all: a failed heater sensor still faults, whatever
  you typed in.

Without that asymmetry, a fake 20 °C could have masked a real 60 °C element and
silently deleted the critical trip. That would have been the most dangerous thing
in this entire repository.

Both overrides **expire after 15 minutes** and clear themselves, and neither
survives a reboot. That timer is the *only* thing that ends a simulation —
switching modes doesn't — so there's one rule to remember rather than two. While
one is armed the little TFT shows `SIM` next to the mode: if the screen and the
phone disagree about the temperature, that needs to be visible from the doorway.

---

## 📶 WiFi configuration

There's a **WiFi** page in Quick Links. You can change the access point's name
and password, and optionally have the board join your home network as well.

### The access point never goes away

Turning on "join home network" puts the ESP32 into `WIFI_AP_STA` — it joins your
router *and* keeps hosting its own network. It is never one or the other. That
means **192.168.4.1 always works**, whatever you've configured, which keeps the
OTA upload target fixed and makes locking yourself out very difficult.

Your router picks the address on the home network, and that address can change.
The access point is the reliable way in; treat the station side as a
convenience.

### The 60-second bargain

Changing WiFi settings on a device you're talking to *over that WiFi* is the
classic way to lose a device. So this works the way a router does:

1. You press **Save and test**.
2. The new settings are applied to the radio — **and not written to memory**.
3. You get disconnected. That's the test.
4. You reconnect using the new details, reopen the page, and press
   **Keep these settings**.
5. Only *then* is anything saved.

Miss the 60-second window — wrong password, typo in the SSID, phone that won't
reconnect — and the controller quietly puts the previous settings back on its
own.

The candidate configuration lives only in RAM, so **pulling the power during the
test is safe too**: the board boots on the last confirmed settings. There is no
sequence of events that leaves unconfirmed credentials in flash.

**Factory reset WiFi** goes through the same trial, so even a misfired reset is
something you can back out of. It restores whatever is compiled in from
`secrets.h` — `Green` / `ChangeME` on a fresh clone, or your own values if you
changed that file before flashing.

Worth being precise about, because it's what matters when you're locked out: the
60-second timeout reverts to **the last configuration you confirmed**, never to a
default. A configuration you never confirmed can't become the one you fall back
to, and on a board that has never had a WiFi change confirmed, that fallback is
the `secrets.h` values.

A few details worth knowing:

- Password boxes come up **blank**, and blank means *unchanged* — the board
  never sends a stored password to the browser.
- The control loop doesn't stop for any of this. Sensor reads, the 1 Hz safety
  tick and the watchdog all keep running at their normal rates through the
  entire trial window.
- The **OTA password is not editable from the web UI**, deliberately. It's
  compiled in from `secrets.h`, so losing the web interface can never lock you
  out of the recovery path. It must never be the same string as the WiFi
  password — sharing the WiFi with a visitor would otherwise hand them the
  firmware-push credential too.

---

## 🔑 Recovery

This board has no USB port, so every credential it holds has to be recoverable
without one. **Hold the IO0 button on the carrier board for five seconds.** The
TFT counts down and you can let go to cancel; at zero the web password, the
access point and the setpoints all go back to their `secrets.h` defaults. The
statistics are kept — they are a maintenance record, and nothing about them can
lock anyone out.

Two things about that button worth knowing:

- It is a **runtime** hold, not a hold-at-boot. GPIO 0 is a strapping pin:
  holding it down through a reset selects the serial bootloader instead, and
  the firmware never runs. Let the board boot first, *then* hold it.
- Readings are unreliable while a **USB-TTL module is plugged in**, because its
  auto-reset circuit drives the same pin. Unplug it before relying on the
  button.

If the button is out of reach, `espota` still works — its password is compiled
in and nothing in the web UI can change it. That is the whole reason it is
compiled in.

---

## 📡 Updating over the air

The production board has no USB port, which concentrates the mind. Two ways in.

**Browser or curl** — a POST of `.pio/build/esp32dev/firmware.bin` to `/update`,
either by dragging the file onto **http://192.168.4.1/update** from your phone,
or with curl. The upload is guarded by the same web session as everything else,
so log in first and keep the cookie:

```bash
curl -c jar -d "pass=YOUR_WEB_PASSWORD" http://192.168.4.1/login && curl -b jar -F "firmware=@.pio/build/esp32dev/firmware.bin" http://192.168.4.1/update
```

This is the route to reach for first. It's a single outbound request, and it's
the one that has actually been used in anger — around 10 seconds for a 900 KB
image.

**espota**, which is what `[env:ota]` in `platformio.ini` is set up for:

```bash
export GREENHOUSE_OTA_PASS="your-ota-password"
pio run -e ota -t upload
```

Be aware that espota works by asking the ESP32 to open a connection *back* to
your machine, so a host firewall has to allow that inbound connection. When it
fails it tends to just hang, which looks like a dead board and isn't.

Both paths switch the heater off, leave the fans running, and save your
statistics before writing a single byte. If an upload fails halfway, the ESP32
keeps booting the old image from the other OTA slot — a botched update can't
brick it.

Your computer has to be joined to the board's access point either way. Expect to
have to rejoin it manually after the reboot — the access point disappears for a
few seconds and not every client comes back on its own.

Page assets are versioned (`/style.css?v=611`). Bump that number in `webui.cpp`
whenever the CSS or JS changes, or browsers that cached the old copy will render
the new page with the old stylesheet, which looks like a badly broken update
rather than a caching problem.

---

## 🧭 A note on the code

It's C++ for PlatformIO, split into small modules that each own one thing:

```
config    pins, limits, saved settings, WiFi configuration and statistics
sensors   reading DS18B20s and deciding which readings to believe
control   the relays, the fault policy, the state machine, manual mode
display   the little screen
net       the access point, the optional station, the WiFi trial/commit
webui     static pages served from flash + a /status JSON endpoint
ota       firmware updates, safely
```

One rule matters more than the rest: **only `control.cpp` is allowed to touch a
relay pin.** Everything about heater bookkeeping — how long it ran, when it last
stopped, whether it's allowed to start again — lives behind two functions. An
earlier version of this firmware turned the heater off in seven different
places, and four of them forgot to write something down. That's the kind of bug
that doesn't crash anything; it just quietly removes a safety interlock and
waits.

Manual mode obeys that rule too. The web handlers don't switch anything — they
set a *request*, and `control.cpp` decides whether the relay actually moves,
against the same interlocks every automatic path uses.

---

## 📄 Licence

MIT. Have fun, be careful, and put a thermal cutout in it. 🍅
