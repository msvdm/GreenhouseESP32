# 🌱 GreenhouseESP32

A small ESP32 thermostat for a greenhouse. It averages the air temperature from
DS18B20 probes on two sensor lines, switches **radiant heaters** (through a
contactor) and **ceiling fans** with the two onboard relays, and shows
everything on a 1.8" ST7735 screen and a password-protected web page. Firmware
updates go over WiFi, because the board has no USB port.

## Hardware

**Board:** ESP32-WROOM-32E 2-channel relay module ("ESP32 Relay x2", 7–60 V DC
input, 2 × 10 A relays, active-HIGH, no USB).

| Signal | GPIO | Notes |
|---|---|---|
| Relay 1 – ceiling fans | 16 | onboard |
| Relay 2 – heater contactor coil | 17 | onboard; the contactor switches the 1200 W radiant heaters |
| Sensor line 1 | 4 | DS18B20, any number of probes, 4k7 pull-up to 3V3 |
| Sensor line 2 | 15 | DS18B20, any number of probes, 4k7 pull-up to 3V3 |
| TFT SCK / SDA (MOSI) | 18 / 26 | ST7735 160×128, hardware SPI with remapped pins |
| TFT CS / A0 (DC) / RST | 25 / 27 / 33 | |
| Onboard LED | 23 | status light, see below |
| IO0 button | 0 | hold 5 s while running = factory reset of password + access point |

Unused: 2, 5, 13, 14, 19 (claimed by the SPI driver as MISO), 21, 22, 32.

**Status LED**

| Pattern | Meaning |
|---|---|
| short blink every 2 s | all good (the firmware is alive) |
| slow blink, 1 Hz | home WiFi configured but not connected |
| fast blink, 4 Hz | no valid temperature: automatic relays are off |
| solid on | firmware update in progress |

See [Wiring Diagram.txt](Wiring%20Diagram.txt) for the wiring.

## How it controls

- **Temperature.** Each line's average is the mean of its working probes; the
  room temperature is the mean of the two line averages, so both lines weigh the
  same however many probes they carry. Probes are re-detected every 30 s, so you
  can add or remove them without rebooting. A line that has had no valid reading
  for 30 s drops out of the average.
- **Heating** turns on at or below the *heating* setpoint and off at
  setpoint + hysteresis.
- **Fans (cooling)** turn on at or above the *fan* setpoint and off at
  setpoint − hysteresis.
- **Heating mode.** *Zone*: heaters only. *Room*: the ceiling fans also run
  whenever the heaters are on, to spread the warm air through the whole space.
- **Manual override.** Each relay can be set to Auto / On / Off in the web UI.
  Overrides are not saved: after a reboot both relays start in Auto.
- **No temperature at all** (both lines lost): both automatic outputs switch off.
- A 30 s watchdog reboots the board if the firmware ever hangs; relays always
  start off.

Setpoints, hysteresis and the heating mode are saved in flash. The heating
setpoint must sit below the fan setpoint by more than twice the hysteresis,
so heating can never trigger cooling.

## Web interface

Open the address shown at the top of the screen (e.g. `http://192.168.0.102`)
or `http://greenhouse.local`. Default password: `GreenAdmin`; change it under
**Settings**.

- **Home page** – average and per-line temperatures, relay states with
  Auto/On/Off, Zone/Room heating mode, setpoints.
- **Settings** – home WiFi, the board's own access point, hostname, web
  password, reboot.
- **Update** – upload a `firmware.bin`.

The board always keeps its own access point up (factory: `Green` / `ChangeME`,
at `http://192.168.4.1`), so it stays reachable if the home network is down.

## Building and updating

Needs [PlatformIO](https://platformio.org/) (`pip install platformio`).

```bash
cp src/secrets.h.example src/secrets.h    # then edit it
pio run -e esp32dev                       # build
```

Update over WiFi, either:

- **Browser:** *Update* page → upload `.pio/build/esp32dev/firmware.bin`, or
- **espota:** set `GREENHOUSE_OTA_PASS` to the `OTA_PASSWORD` from `secrets.h`,
  then `pio run -e ota -t upload`. On Windows the firewall must allow the
  inbound connection the board makes back to the PC.

First flash of a blank board: a USB-TTL adapter on the 6-pin header, IO0 held
low at reset, `pio run -e esp32dev -t upload --upload-port COMx`.

## ⚠️ Mains safety

This controller only drives a contactor coil, but that contactor switches
mains-powered heaters. Have the panel side wired and checked by a qualified
electrician, and keep the heaters' own built-in overheat protection in place.
The firmware is a thermostat, not a safety device.

## Licence

MIT – see [LICENSE](LICENSE).
