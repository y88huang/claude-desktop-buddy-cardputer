# claude-desktop-buddy-cardputer

A port of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) to the **M5Stack Cardputer ADV** (ESP32-S3, 240×135 landscape TFT, full QWERTY keyboard, RGB NeoPixel, BMI270 IMU, I²S speaker, hardware power slide switch).

The upstream firmware targets the M5StickC Plus — portrait 135×240 display, AXP192 power controller, two-button input. This fork swaps those dependencies for the Cardputer ADV's peripherals and redesigns the UI for landscape + keyboard input. The BLE bridge, stats, NVS persistence, GIF character pipeline, and all 18 original ASCII species work unchanged.

> **Building your own device?** You still don't need any of the code here — the wire protocol reference in **[REFERENCE.md](REFERENCE.md)** is untouched from upstream.

## What changed from upstream

- **HAL shim** ([`src/hal.h`](src/hal.h) + [`src/hal.cpp`](src/hal.cpp)) — wraps every `M5.Axp` / `M5.Beep` / `M5.BtnA` / `M5.BtnB` / `M5.Imu` / `M5.Rtc` call site so both boards build from the same source tree. `platformio.ini` has two envs: `m5stickc-plus` (unchanged upstream build) and `cardputer-adv`.
- **Landscape UI** — sprite is now 240×135, every modal / info / pet panel relayouted for the wider-than-tall canvas. Menus centered, settings compacted to 10 rows at 10 px pitch, info/pet pages go full-screen with the pet hidden.
- **Keyboard input** — a HalKey event queue rising-edge-detects the Cardputer matrix and emits `Approve` / `Deny` / `Back` / `Up` / `Down` / `Left` / `Right` / `Menu` / `Demo`. Enter and `Del`/`` ` `` also swallow their matching BtnA/BtnB release so modal confirms don't double-fire into a home-screen cycle.
- **Live pet picker** — Menu → **pet** opens a full-size preview with a hint bar at the bottom; step species with `,` / `/` and commit with `Enter`. On commit the pet plays a `P_HEART` one-shot and a 3-note save fanfare.
- **8-bit SFX library** — 10 tuned note sequences (nav blip, confirm arpeggio, Zelda-lite approve chord, Mario 1-UP alert, back, deny, save fanfare, menu, warn, warn2) played through an in-loop tone sequencer shared across both boards. New `halBeepSeq()` replaces the previous single-note `beep()`.
- **RGB NeoPixel attention LED** on GPIO 21 — driven by `neopixelWrite` (shipping with the Arduino-ESP32 core, no extra dep), pulses red-orange while an approval is pending.
- **Compact clock** — the upstream charging-clock face was redesigned as a 10 px top strip (size-1 glyphs) instead of a 20 px size-2 strip, leaving y=10..135 for the pet. Tilt-to-landscape variant disabled since the UI is already landscape.
- **Two new ASCII species** — `doge` and `llama`, bringing the total to 20.
- **Sleep timeout** raised from 30 s to 2 minutes.

## Hardware

M5Stack **Cardputer ADV** (ESP32-S3 StampS3 + 1.14" 240×135 ST7789 + BMI270 IMU + NS4168 I²S speaker + 56-key QWERTY matrix + WS2812B RGB LED on GPIO 21). The stock Cardputer (no IMU) should boot and run everything except shake/face-down nap.

Original M5StickC Plus firmware path is preserved — the HAL passes through to the native `M5.*` APIs on that board.

## Flashing

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/), then:

```bash
# Cardputer ADV:
pio run -e cardputer-adv -t upload

# Original StickC Plus:
pio run -e m5stickc-plus -t upload
```

Wipe a previously-flashed device first with `pio run -e cardputer-adv -t erase`. Factory reset is also available from the device: Menu → settings → reset → factory reset → tap twice.

## Pairing

Identical to upstream. Enable developer mode in Claude (**Help → Troubleshooting → Enable Developer Mode**), then **Developer → Open Hardware Buddy…** and pick your device. The Cardputer advertises as `Claude-XXXX` (last two MAC bytes).

## Keyboard mapping

| Key | Home screen | In a modal (menu / settings / reset / picker) | In an approval prompt |
|---|---|---|---|
| `Enter` | cycle display mode | **confirm / commit** | **approve** |
| `Backspace` / `` ` `` | — | **close modal** | **deny** |
| `;` / `.` | scroll transcript up / down | move highlight up / down | — |
| `,` / `/` | prev / next page (INFO & PET) | picker: prev / next species | — |
| `Y` / `N` | — | (picker) confirm | **approve / deny** |
| `M` | open menu | — | — |
| `G` | toggle demo mode | — | — |

Power is the hardware slide switch on the right edge. Screen auto-sleeps after 2 minutes of no activity (kept on while on USB power or while an approval is pending).

## 20 ASCII species

capybara, duck, goose, blob, cat, dragon, octopus, owl, penguin, turtle, snail, ghost, axolotl, cactus, robot, rabbit, mushroom, chonk, **doge**, **llama** — the last two are new in this fork.

Each species animates across seven states (sleep / idle / busy / attention / celebrate / dizzy / heart). Pick one from Menu → pet.

## GIF pets

Unchanged from upstream — drag a character pack folder onto the drop target in the Hardware Buddy window. See the [upstream README](https://github.com/anthropics/claude-desktop-buddy#gif-pets) for the pack format. GIFs are rendered centered in the upper 80 px of the landscape canvas. The 1.8 MB LittleFS budget is the same.

## Layout notes

- **Home**: pet fills the upper 80 px, the 3-line transcript is block-centered at x=57 in the bottom 28 px.
- **Clock face** (idle + USB): one-line `HH:MM:SS  Mon DD` at size 1 in a 10 px top strip.
- **Pet stats** (the `B`-cycle stats page, separate from the menu's pet picker): two-column landscape layout, mood / fed / energy / Lv badge on the left, counters on the right.
- **Info pages**: full-screen, pet hidden. ABOUT and KEYBOARD copies rewritten for Cardputer bindings; CREDITS hardware line updated to `M5 Cardputer ADV / ESP32-S3`.

## Known limitations

- `halBatteryMilliAmps()` returns 0 — M5Unified doesn't expose a current reading on this hardware. The DEVICE info page shows `current +0mA`.
- `halTempC()` is stubbed at 25 °C (same reason).
- If the BMI270 doesn't come up (stock Cardputer, or M5Unified config mismatch), shake and face-down nap silently no-op. Serial prints `IMU: begin=0 type=0` at boot in that case.
- Some info-page copy still reads long for landscape; the CLAUDE / DEVICE / BLUETOOTH pages fit but CREDITS is tight.

## Credits

- Original firmware by [Felix Rieseberg](https://github.com/felixrieseberg) at Anthropic.
- Cardputer ADV port, layout redesign, HAL shim, keyboard event routing, 8-bit SFX, pet picker, and new `doge` / `llama` species by [@y88huang](https://github.com/y88huang).

## Availability

The BLE API is only available when the Claude desktop apps are in developer mode. It's intended for makers and developers and isn't an officially supported product feature.
