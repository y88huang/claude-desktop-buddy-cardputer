#pragma once

// Hardware abstraction layer. The firmware was written for M5StickC Plus
// (ESP32 + AXP192 + MPU6886 + buzzer + two buttons). This header lets the
// same source tree build for either that board or the M5Cardputer ADV
// (ESP32-S3 + M5Unified power/display/imu + I2S speaker + QWERTY keyboard),
// selected by the -DCARDPUTER_ADV build flag in platformio.ini.
//
// Everywhere main.cpp / xfer.h / data.h previously called M5.Axp.*, M5.Beep.*,
// M5.BtnA.*, M5.BtnB.*, M5.Imu.*, M5.Rtc.*, they now call hal* wrappers below.

#include <Arduino.h>
#include <stdint.h>

#ifdef CARDPUTER_ADV
  #include <M5Cardputer.h>
  // Keyboard_def.h defines SHIFT as a plain macro which collides with a
  // local identifier in src/buddies/goose.cpp (const char* SHIFT[]).
  // We only need the macro inside the keyboard driver, so drop it here.
  #ifdef SHIFT
    #undef SHIFT
  #endif
  // M5Unified owns M5.Lcd / M5.Display / M5.Imu. M5Cardputer owns the
  // keyboard. We add back the StickC primitives as free functions and
  // re-declare the RTC_*TypeDef structs exactly as M5StickCPlus exposes
  // them so the callers don't care which board they're on.
  struct RTC_TimeTypeDef { uint8_t Hours;   uint8_t Minutes; uint8_t Seconds; };
  struct RTC_DateTypeDef { uint8_t WeekDay; uint8_t Month;   uint8_t Date;    uint16_t Year; };
  // M5Cardputer uses LovyanGFX/M5GFX under the hood. TFT_eSPI is the
  // shared base class used as a pointer target (both M5.Lcd and M5Canvas
  // sprites have to be passable as a TFT_eSPI*). lgfx::LovyanGFX is the
  // common ancestor of LGFX_Device (M5GFX) and LGFX_Sprite (M5Canvas),
  // so aliasing to it keeps the existing signatures valid.
  using TFT_eSPI    = lgfx::LovyanGFX;
  using TFT_eSprite = M5Canvas;
#else
  #include <M5StickCPlus.h>
#endif

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void halInit();
void halUpdate();

// ---------------------------------------------------------------------------
// Power / display power / battery (AXP192 on StickC; M5.Power on Cardputer)
// ---------------------------------------------------------------------------
// Brightness input range 0..100 to match M5.Axp.ScreenBreath(); scaled
// internally on Cardputer (which takes 0..255).
void     halBrightness(int pct);
void     halScreenPower(bool on);
void     halPowerOff();
float    halBatteryVolts();
float    halBatteryMilliAmps();
float    halVbusVolts();
int      halTempC();
// Returns 0x02 for a short press on the StickC power button, 0 otherwise.
// On Cardputer there's no dedicated power button so this is always 0 for
// now — long-press Fn+Esc will be wired up in a later step.
uint8_t  halPowerBtnPress();

// ---------------------------------------------------------------------------
// Beep / speaker
// ---------------------------------------------------------------------------
void halBeepInit();
void halBeepUpdate();
void halBeep(uint16_t freq, uint16_t durMs);
// Queue a short sequence of tones. Each call replaces anything previously
// queued — rapid user input gets fresh feedback instead of a backlog. The
// arrays must outlive the sequence (use static const tables). Caller
// checks its own mute / settings.sound gate before dispatching.
void halBeepSeq(const uint16_t* freqs, const uint16_t* durs, uint8_t n);

// ---------------------------------------------------------------------------
// IMU
// ---------------------------------------------------------------------------
void halImuInit();
void halGetAccel(float* ax, float* ay, float* az);

// ---------------------------------------------------------------------------
// Soft RTC
// ---------------------------------------------------------------------------
void halRtcGetTime(RTC_TimeTypeDef* t);
void halRtcGetDate(RTC_DateTypeDef* d);
void halRtcSetTime(const RTC_TimeTypeDef* t);
void halRtcSetDate(const RTC_DateTypeDef* d);

// ---------------------------------------------------------------------------
// Buttons. On StickC these wrap M5.BtnA / M5.BtnB directly. On Cardputer
// they surface two "virtual buttons" driven by the keyboard:
//   BtnA = Enter (approve / advance / menu-pick when held)
//   BtnB = ` / Esc (deny / next page)
// ---------------------------------------------------------------------------
struct HalBtn {
  virtual bool isPressed()                = 0;
  virtual bool wasPressed()               = 0;
  virtual bool wasReleased()              = 0;
  virtual bool pressedFor(uint32_t ms)    = 0;
  // Called when a HalKey event derived from this button (Enter→Approve,
  // Del/`→Back) was consumed by a modal. Clears the pending rise and
  // flags the upcoming fall as swallowed so the release handler doesn't
  // re-fire the button semantics on the next frame. No-op on StickC
  // since its buttons don't route through HalKey.
  virtual void suppressPending() {}
  virtual ~HalBtn() {}
};
HalBtn& halBtnA();
HalBtn& halBtnB();

// ---------------------------------------------------------------------------
// Attention LED. Single entry point that works on both boards:
//   StickC — red LED on GPIO 10 (active-low). Any non-zero component → on.
//   Cardputer — WS2812B NeoPixel on GPIO 21. 8-bit RGB pushed verbatim.
// Callers treat it as a set-state API; the driver handles protocol details.
// ---------------------------------------------------------------------------
void halLedSet(uint8_t r, uint8_t g, uint8_t b);

// ---------------------------------------------------------------------------
// Keyboard events (Cardputer only; StickC always returns None). A single
// rising-edge is pushed per key press into an internal ring buffer; callers
// drain by calling halPollKey() each loop iteration until it returns None.
//
// Physical layout notes for the Cardputer:
//   Arrow-glyph keys — `;` (up), `.` (down), `,` (left), `/` (right)
//   Shortcut letters — `y` / `n` / `m` / `g`
// The Cardputer has a hardware power slide switch, so there's no software
// power chord — the existing 30 s idle auto-sleep handles the screen.
// ---------------------------------------------------------------------------
enum class HalKey : uint8_t {
  None = 0,
  Up, Down, Left, Right,
  Approve,       // y / Enter
  Deny,          // n
  Back,          // Esc (backtick) / Del / Backspace — close modal
  Menu,          // m
  Demo,          // g
};
HalKey halPollKey();
