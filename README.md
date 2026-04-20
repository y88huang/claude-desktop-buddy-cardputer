# Claude desktop buddy port for m5stack cardputer

> **Physical desk pet for Claude Code / Cowork** — runs on the **M5Stack Cardputer ADV** (ESP32-S3). Approve permission prompts with `Y`/`N` on a real keyboard, watch your pet react to what Claude is doing, get a Mario 1-UP in your ear when an approval is waiting, and flip the device face-down to make it nap.

[![PlatformIO](https://img.shields.io/badge/built_with-PlatformIO-orange.svg)](https://platformio.org/)
[![Target](https://img.shields.io/badge/target-ESP32--S3-blue.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Hardware](https://img.shields.io/badge/hardware-M5Stack_Cardputer_ADV-lightblue.svg)](https://shop.m5stack.com/products/m5stack-cardputer-adv)
[![Fork of](https://img.shields.io/badge/fork_of-anthropics%2Fclaude--desktop--buddy-purple)](https://github.com/anthropics/claude-desktop-buddy)

A port of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) from the M5StickC Plus to the Cardputer ADV. The BLE bridge, stats, NVS persistence, GIF character pipeline, and all 18 original ASCII species work unchanged; this fork adds a landscape UI, full-keyboard input, 8-bit sound library, a live pet picker, two new species (`doge`, `llama`), and a handful of UX fixes.

## What it does
<img width="1498" height="958" alt="default screen for claude buddy cardputer in idle mode" src="https://github.com/user-attachments/assets/b4272ba9-3735-48a4-b76d-4af2c46088d2" />
<img width="1165" height="832" alt="info screen for claude desktop buddy cardputer in idle mode" src="https://github.com/user-attachments/assets/1af36f57-5696-4fb3-aaa4-f749ca196f30" />
<img width="1469" height="976" alt="pet screen for claude desktop buddy cardputer" src="https://github.com/user-attachments/assets/2de63c37-2c55-45ba-9c60-28c0c9b29619" />

The device connects to the Claude desktop apps over BLE (developer mode required) and acts as a physical session dashboard + permission-approval affordance:

- **Permission prompts** — when Claude asks to run a command, the device plays a Mario 1-UP jingle, pulses the RGB LED red-orange, and shows the tool name. Press `Y` to approve, `N` to deny. Approving in under 5 seconds triggers a heart animation.
- **Live transcript** — the last three wrapped lines from Claude scroll at the bottom of the home screen.
- **Seven pet moods** — sleep, idle, busy (3+ sessions running), attention (approval pending), celebrate (session just completed), dizzy (you shook it), heart (fast approval).
- **20 ASCII species + custom GIFs** — cycle with the live picker (Menu → pet) or drag a character pack folder onto the Hardware Buddy window to stream a custom GIF character over BLE.
- **Tamagotchi mechanics** — the pet has mood, fed, energy, and level stats that drift based on your approval cadence and Claude's token usage. Flip the device face-down and it naps (screen dims, energy refills). Shake it to make it dizzy.
- **Compact clock face** — `HH:MM:SS Mon DD` appears across the top when idle on USB power, with the pet still rendering underneath.
- **Stats page** — press `Enter` to cycle to PET mode for mood hearts, fed bar, energy bar, level badge, and lifetime counters (approvals, denials, nap hours, tokens, tokens today).

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
