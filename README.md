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

**The control loop is boring on purpose.** It sits in one of four states:

| State | What's happening |
|---|---|
| 🟦 `IDLE` | Temperature is fine. Everything off. The good state. |
| 🔥 `HEATING` | Too cold. Fans first, *then* heat. Never the other way round. |
| ❄️ `COOLING` | Too warm. Fans only. |
| 💨 `COOLDOWN` | Heater just stopped. Fans keep running to purge the residual heat. |

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
git clone https://github.com/klmnr/GreenhouseESP32.git
cd GreenhouseESP32
cp src/secrets.h.example src/secrets.h    # then edit it
pio run -e esp32dev -t upload
```

The board comes up as a WiFi access point. Connect to it, open
**http://192.168.4.1**, and you get the whole interface — live temperatures,
the current mode, setpoint buttons, and a statistics page.

### Updating over the air

The production board has no USB port, which concentrates the mind. Two ways in:

```bash
export GREENHOUSE_OTA_PASS="your-ota-password"
pio run -e ota -t upload
```

…or just drag a `firmware.bin` onto **http://192.168.4.1/update** from your
phone while standing in the greenhouse.

Both paths switch the heater off, leave the fans running, and save your
statistics before writing a single byte. If an upload fails halfway, the ESP32
keeps booting the old image from the other OTA slot — a botched update can't
brick it.

---

## 🧭 A note on the code

It's C++ for PlatformIO, split into small modules that each own one thing:

```
config    pins, limits, saved settings and lifetime statistics
sensors   reading DS18B20s and deciding which readings to believe
control   the relays, the fault policy, the state machine
display   the little screen
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

---

## 📄 Licence

MIT. Have fun, be careful, and put a thermal cutout in it. 🍅
