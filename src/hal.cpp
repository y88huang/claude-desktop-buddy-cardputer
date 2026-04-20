#include "hal.h"
#include <sys/time.h>
#include <time.h>
#include <vector>

// ---------------------------------------------------------------------------
// Tone sequencer. Shared between both boards: halBeep() is a single-note
// primitive (PWM buzzer on StickC, I²S speaker on Cardputer), neither of
// which schedules multi-note sequences on their own in a cross-platform
// way. _pumpSeq fires the next queued note once the previous one's
// duration has elapsed, and halBeepSeq replaces any in-flight sequence so
// rapid keystrokes feel responsive.
// ---------------------------------------------------------------------------
namespace {
const uint16_t* _seqFreqs  = nullptr;
const uint16_t* _seqDurs   = nullptr;
uint8_t         _seqLen    = 0;
uint8_t         _seqIdx    = 0;
uint32_t        _seqNextAt = 0;

void _pumpSeq() {
  if (_seqIdx >= _seqLen) return;
  if ((int32_t)(millis() - _seqNextAt) < 0) return;
  uint16_t f = _seqFreqs[_seqIdx];
  uint16_t d = _seqDurs[_seqIdx];
  halBeep(f, d);
  // +8 ms gap between notes so consecutive tones don't smear into one.
  _seqNextAt = millis() + d + 8;
  _seqIdx++;
}
}  // namespace

void halBeepSeq(const uint16_t* f, const uint16_t* d, uint8_t n) {
  _seqFreqs  = f;
  _seqDurs   = d;
  _seqLen    = n;
  _seqIdx    = 0;
  _seqNextAt = millis();
}

// =============================================================================
// StickC Plus implementation — thin passthrough to M5.Axp / M5.Beep / M5.BtnA /B
// =============================================================================
#ifndef CARDPUTER_ADV

void halInit() {
  M5.begin();
  // StickC has a red status LED on GPIO 10 (active-low). Drive it high
  // (off) immediately so any boot transient doesn't look like attention.
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);
}
void halUpdate() { M5.update(); }

void  halBrightness(int pct)          { M5.Axp.ScreenBreath(pct); }
void  halScreenPower(bool on)         { M5.Axp.SetLDO2(on); }
void  halPowerOff()                   { M5.Axp.PowerOff(); }
float halBatteryVolts()               { return M5.Axp.GetBatVoltage(); }
float halBatteryMilliAmps()           { return M5.Axp.GetBatCurrent(); }
float halVbusVolts()                  { return M5.Axp.GetVBusVoltage(); }
int   halTempC()                      { return (int)M5.Axp.GetTempInAXP192(); }
uint8_t halPowerBtnPress()            { return M5.Axp.GetBtnPress(); }

void halBeepInit()                    { M5.Beep.begin(); }
void halBeepUpdate()                  { M5.Beep.update(); _pumpSeq(); }
void halBeep(uint16_t f, uint16_t d)  { M5.Beep.tone(f, d); }

void halImuInit()                               { M5.Imu.Init(); }
void halGetAccel(float* x, float* y, float* z)  { M5.Imu.getAccelData(x, y, z); }

void halRtcGetTime(RTC_TimeTypeDef* t)       { M5.Rtc.GetTime(t); }
void halRtcGetDate(RTC_DateTypeDef* d)       { M5.Rtc.GetDate(d); }
void halRtcSetTime(const RTC_TimeTypeDef* t) { M5.Rtc.SetTime(const_cast<RTC_TimeTypeDef*>(t)); }
void halRtcSetDate(const RTC_DateTypeDef* d) { M5.Rtc.SetDate(const_cast<RTC_DateTypeDef*>(d)); }

// Thin adapters around M5.BtnA / M5.BtnB. We hold references to the real
// Button objects and forward every call.
namespace {
struct StickBtn : HalBtn {
  Button& b;
  StickBtn(Button& btn) : b(btn) {}
  bool isPressed()             override { return b.isPressed(); }
  bool wasPressed()            override { return b.wasPressed(); }
  bool wasReleased()           override { return b.wasReleased(); }
  bool pressedFor(uint32_t ms) override { return b.pressedFor(ms); }
};
} // namespace
HalBtn& halBtnA() { static StickBtn a(M5.BtnA); return a; }
HalBtn& halBtnB() { static StickBtn b(M5.BtnB); return b; }

// StickC red LED: active-low, one color. Treat any non-zero RGB as "on".
// The pinMode was set in main.cpp setup() — here we only drive the level.
void halLedSet(uint8_t r, uint8_t g, uint8_t b) {
  bool on = (r | g | b) != 0;
  digitalWrite(10, on ? LOW : HIGH);
}

// No keyboard on StickC — event queue stays empty.
HalKey halPollKey() { return HalKey::None; }

// =============================================================================
// Cardputer ADV implementation — M5Unified + M5Cardputer
// =============================================================================
#else  // CARDPUTER_ADV

namespace { void _pollKeys(); }

void halInit() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);   // true = also init the keyboard matrix
  M5.Speaker.begin();
  M5.Speaker.setVolume(160);
}
void halUpdate() {
  M5Cardputer.update();
  _pollKeys();
}

// M5.Display.setBrightness expects 0..255, pct comes in as 0..100.
void  halBrightness(int pct) { M5.Display.setBrightness((pct * 255) / 100); }
void  halScreenPower(bool on) {
  if (on) { M5.Display.wakeup(); M5.Display.setBrightness(200); }
  else    { M5.Display.setBrightness(0); M5.Display.sleep(); }
}
void  halPowerOff()          { M5.Power.powerOff(); }
float halBatteryVolts()      { return M5.Power.getBatteryVoltage() / 1000.0f; }
float halBatteryMilliAmps()  { return 0.0f; }   // not exposed on Cardputer
// M5.Power.isCharging(): -1 = discharging (on battery), 0 = USB but full,
// 1 = actively charging. Treat anything ≥ 0 as "USB plugged in" so the
// charging-clock face still shows when the battery has topped off.
float halVbusVolts()         { return M5.Power.isCharging() >= 0 ? 5.0f : 0.0f; }
int   halTempC()             { return 25; }     // no temp sensor on S3 accessible here
uint8_t halPowerBtnPress()   { return 0; }      // no dedicated power button

// Cardputer's status LED is a single WS2812B on GPIO 21. The Arduino-ESP32
// core ships neopixelWrite() for exactly this — no library dependency.
constexpr int CARDPUTER_RGB_PIN = 21;
void halLedSet(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(CARDPUTER_RGB_PIN, r, g, b);
}

void halBeepInit()                    { /* done in halInit() */ }
void halBeepUpdate()                  { _pumpSeq(); }
void halBeep(uint16_t f, uint16_t d)  { M5.Speaker.tone(f, d); }

void halImuInit() {
  // The default M5Cardputer.begin() doesn't enable the BMI270 on the ADV
  // variant (the stock Cardputer has no IMU, so that's what the config
  // defaults to). Kick M5Unified's IMU scan explicitly. If it returns
  // false, shake and face-down nap just no-op rather than crashing.
  bool ok = M5.Imu.begin();
  Serial.printf("IMU: begin=%d type=%d\n", ok, (int)M5.Imu.getType());
}
void halGetAccel(float* x, float* y, float* z) {
  // Seed the outputs so callers see a sane stationary reading (1g on Z)
  // if the IMU didn't initialize — otherwise checkShake() would be
  // comparing zeros and never trigger dizzy.
  float ax = 0, ay = 0, az = 1.0f;
  M5.Imu.getAccel(&ax, &ay, &az);
  if (x) *x = ax;
  if (y) *y = ay;
  if (z) *z = az;
}

// Cardputer has no battery-backed RTC chip. Back the RTC API with the ESP32
// system clock: settimeofday() when the bridge pushes a sync, localtime_r()
// when the UI reads. Year/month/day/weekday fields match the StickC format.
void halRtcGetTime(RTC_TimeTypeDef* t) {
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  t->Hours = lt.tm_hour; t->Minutes = lt.tm_min; t->Seconds = lt.tm_sec;
}
void halRtcGetDate(RTC_DateTypeDef* d) {
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  d->WeekDay = lt.tm_wday;
  d->Month   = lt.tm_mon + 1;
  d->Date    = lt.tm_mday;
  d->Year    = lt.tm_year + 1900;
}
// SetTime/SetDate are both called together in data.h after a time-sync
// packet, so we reconstruct the full epoch on whichever arrives second and
// ignore the first. Keeps the public API simple at the cost of a static.
static struct tm _pending = {};
static bool _pendingTime = false;
static bool _pendingDate = false;
static void _flush() {
  if (!_pendingTime || !_pendingDate) return;
  time_t epoch = mktime(&_pending);
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  _pendingTime = _pendingDate = false;
}
void halRtcSetTime(const RTC_TimeTypeDef* t) {
  _pending.tm_hour = t->Hours; _pending.tm_min = t->Minutes; _pending.tm_sec = t->Seconds;
  _pendingTime = true; _flush();
}
void halRtcSetDate(const RTC_DateTypeDef* d) {
  _pending.tm_wday = d->WeekDay; _pending.tm_mon = d->Month - 1;
  _pending.tm_mday = d->Date;    _pending.tm_year = d->Year - 1900;
  _pendingDate = true; _flush();
}

// Keyboard-backed virtual buttons. Poll Cardputer.Keyboard on each
// halUpdate (M5Cardputer.update() runs the scan for us). We debounce
// transitions and derive isPressed/wasPressed/wasReleased/pressedFor by
// tracking press timestamps.
namespace {
struct KeyBtn : HalBtn {
  // `isDown()` is implemented per-button to check the keyboard matrix.
  virtual bool isDown() = 0;

  bool   _wasDown       = false;
  bool   _rose          = false;   // consumed by wasPressed()
  bool   _fell          = false;   // consumed by wasReleased()
  bool   _suppressFall  = false;   // if set, next _fell is swallowed
  uint32_t _downAt      = 0;

  void poll() {
    bool d = isDown();
    if (d && !_wasDown) { _rose = true; _downAt = millis(); }
    if (!d && _wasDown) { _fell = true; }
    _wasDown = d;
  }

  bool isPressed()              override { return _wasDown; }
  bool wasPressed()             override { bool r = _rose; _rose = false; return r; }
  bool wasReleased() override {
    // Swallow exactly one fall if the press was already consumed by a
    // HalKey handler. Without this, pressing Enter in the menu closes
    // the menu via HalKey::Approve in frame N, then the release in
    // frame N+1 falls through to the home-screen branch and cycles
    // displayMode onto DISP_PET.
    if (_suppressFall && _fell) {
      _fell = false;
      _suppressFall = false;
      return false;
    }
    bool r = _fell; _fell = false;
    return r;
  }
  bool pressedFor(uint32_t ms)  override { return _wasDown && (millis() - _downAt >= ms); }

  void suppressPending() override {
    // Only arm the swallow if the key is actually held right now —
    // otherwise we'd swallow an unrelated future release (e.g. a Y press
    // that happens to emit Approve would arm Enter's next release).
    if (_wasDown) { _rose = false; _suppressFall = true; }
  }
};

struct EnterBtn : KeyBtn {
  bool isDown() override {
    auto st = M5Cardputer.Keyboard.keysState();
    return st.enter;
  }
};
struct EscBtn : KeyBtn {
  bool isDown() override {
    // Cardputer has no physical Esc, so deny = backtick (`) which sits
    // where Esc would be, plus the dedicated `del` key. Either works.
    auto st = M5Cardputer.Keyboard.keysState();
    if (st.del) return true;
    for (auto c : st.word) if (c == '`') return true;
    return false;
  }
};

EnterBtn gBtnA;
EscBtn   gBtnB;
}  // namespace

HalBtn& halBtnA() { return gBtnA; }
HalBtn& halBtnB() { return gBtnB; }

// ---------------------------------------------------------------------------
// Keyboard event queue. `.word` reports all currently-held printable keys;
// we diff against the previous frame to turn that into rising-edge events
// and drop them into a tiny ring buffer. Callers drain by halPollKey().
// ---------------------------------------------------------------------------
namespace {
constexpr size_t KEY_Q_SIZE = 16;
HalKey    _keyQ[KEY_Q_SIZE];
uint8_t   _keyHead = 0, _keyTail = 0;

void _pushKey(HalKey k) {
  uint8_t next = (_keyHead + 1) % KEY_Q_SIZE;
  if (next == _keyTail) return;   // queue full; drop rather than overwrite
  _keyQ[_keyHead] = k;
  _keyHead = next;
}

std::vector<char> _prevWord;
bool _prevEnter = false;
bool _prevDel   = false;

bool _wordContains(const std::vector<char>& v, char c) {
  for (char x : v) if (x == c) return true;
  return false;
}

void _pollKeyEvents() {
  auto& st = M5Cardputer.Keyboard.keysState();
  // Rising-edge scan of printable keys. Skip while Fn is held so the
  // Fn-layer (if ever used for something) can't spuriously fire shortcuts.
  if (!st.fn) {
    for (char c : st.word) {
      if (_wordContains(_prevWord, c)) continue;
      HalKey k = HalKey::None;
      switch (c) {
        case ';': k = HalKey::Up;      break;
        case '.': k = HalKey::Down;    break;
        case ',': k = HalKey::Left;    break;
        case '/': k = HalKey::Right;   break;
        case 'y': case 'Y': k = HalKey::Approve; break;
        case 'n': case 'N': k = HalKey::Deny;    break;
        case 'm': case 'M': k = HalKey::Menu;    break;
        case 'g': case 'G': k = HalKey::Demo;    break;
        case '`':           k = HalKey::Back;    break;  // physical Esc position
      }
      if (k != HalKey::None) _pushKey(k);
    }
  }
  // Dedicated keys (Enter / Del) aren't in .word — they have their own
  // state bools. Edge-detect each so Enter confirms and Del closes modals
  // even though those keys also drive the HalBtn backup.
  if (st.enter && !_prevEnter) _pushKey(HalKey::Approve);
  if (st.del   && !_prevDel)   _pushKey(HalKey::Back);
  _prevEnter = st.enter;
  _prevDel   = st.del;
  _prevWord  = st.word;
}
}  // namespace

HalKey halPollKey() {
  if (_keyHead == _keyTail) return HalKey::None;
  HalKey k = _keyQ[_keyTail];
  _keyTail = (_keyTail + 1) % KEY_Q_SIZE;
  return k;
}

namespace { void _pollKeys() { gBtnA.poll(); gBtnB.poll(); _pollKeyEvents(); } }

#endif  // CARDPUTER_ADV
