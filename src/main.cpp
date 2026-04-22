#include "hal.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#ifdef CARDPUTER_ADV
// Cardputer's 1.14" TFT is 240x135 native landscape. UI runs at rotation 1
// so the keyboard is on the bottom; every coord in this file that
// references H (e.g. clock baselines, modal centering, nap dim) now
// assumes the smaller vertical axis.
const int W = 240, H = 135;
const int CX = W / 2;
const int CY_BASE = H / 2;
constexpr uint8_t HOME_ROTATION = 1;   // landscape — keyboard on bottom edge
// Info and pet pages both hide the pet on landscape and claim the full
// canvas from y=4 — H=135 is too tight to split the screen between a
// peek-pet strip and a content panel. The pet lives on the home screen.
constexpr int PET_PANEL_TOP  = 4;
constexpr int INFO_PANEL_TOP = 4;
#else
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
constexpr uint8_t HOME_ROTATION = 0;   // portrait — StickC held upright
constexpr int PET_PANEL_TOP  = 70;
constexpr int INFO_PANEL_TOP = 70;
#endif

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

// Reverse of nextPet(). Used by the picker's Left arrow so the user can
// back out of a species they've accidentally jumped past instead of
// wrapping through all ~19 to get back.
static void prevPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → last species
    buddyMode = true;
    uint8_t last = n ? n - 1 : 0;
    buddySetSpeciesIdx(last);
    speciesIdxSave(last);
  } else if (buddySpeciesIdx() == 0 && gifAvailable) {      // first species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    uint8_t idx = buddySpeciesIdx();
    uint8_t prev = (idx == 0) ? (n ? n - 1 : 0) : idx - 1;
    buddySetSpeciesIdx(prev);
    speciesIdxSave(prev);
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 120000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  halGetAccel(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { halBrightness(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    halScreenPower(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

// -------------------------------------------------------------------------
// 8-bit SFX library. Each sound is a short note sequence tuned to C-major
// pentatonic so rapid keystrokes don't sound dissonant. Notes are stored
// as (freq, dur_ms) pairs in parallel const tables; sfxXxx() wrappers
// hand them to halBeepSeq which steps through them in the background.
// Earlier calls are pre-empted — the newest key press always gets a
// fresh sound rather than queueing behind stale ones.
// -------------------------------------------------------------------------
static const uint16_t SFX_NAV_F[]      = { 1760 };                        // A6
static const uint16_t SFX_NAV_D[]      = {   22 };
static const uint16_t SFX_CONFIRM_F[]  = { 1318, 1976 };                  // E6, B6
static const uint16_t SFX_CONFIRM_D[]  = {   35,   55 };
static const uint16_t SFX_BACK_F[]     = { 1568, 1047 };                  // G6, C6
static const uint16_t SFX_BACK_D[]     = {   35,   65 };
static const uint16_t SFX_APPROVE_F[]  = { 1047, 1318, 1568, 2093 };      // C E G C  (Zelda-lite)
static const uint16_t SFX_APPROVE_D[]  = {   40,   40,   40,   90 };
static const uint16_t SFX_DENY_F[]     = { 880, 698, 523 };               // A5, F5, C5
static const uint16_t SFX_DENY_D[]     = {  45,  45,  90 };
static const uint16_t SFX_SAVE_F[]     = { 1568, 1976, 2349 };            // G6, B6, D7
static const uint16_t SFX_SAVE_D[]     = {   45,   45,   90 };
// Mario 1-UP jingle — the most "come look at me" chiptune in existence.
// E5, G5, E6, C6, D6, G6 at ~35ms/note (80ms on the tail) is roughly the
// NES tempo; any slower and it stops sounding like Mario.
static const uint16_t SFX_ALERT_F[]    = { 659, 784, 1319, 1047, 1175, 1568 };
static const uint16_t SFX_ALERT_D[]    = {  35,  35,   35,   35,   35,   90 };
static const uint16_t SFX_MENU_F[]     = { 784, 1047 };                   // G5, C6
static const uint16_t SFX_MENU_D[]     = {  55,   75 };
static const uint16_t SFX_WARN_F[]     = { 330 };                         // E4 low buzz
static const uint16_t SFX_WARN_D[]     = { 140 };
static const uint16_t SFX_WARN2_F[]    = { 220, 165 };                    // A3, E3 — heavier
static const uint16_t SFX_WARN2_D[]    = {  90, 180 };

static void sfxPlay(const uint16_t* f, const uint16_t* d, uint8_t n) {
  if (settings().sound) halBeepSeq(f, d, n);
}
#define SFX_N(arr) (uint8_t)(sizeof(arr) / sizeof((arr)[0]))
static void sfxNav()     { sfxPlay(SFX_NAV_F,     SFX_NAV_D,     SFX_N(SFX_NAV_F));     }
static void sfxConfirm() { sfxPlay(SFX_CONFIRM_F, SFX_CONFIRM_D, SFX_N(SFX_CONFIRM_F)); }
static void sfxBack()    { sfxPlay(SFX_BACK_F,    SFX_BACK_D,    SFX_N(SFX_BACK_F));    }
static void sfxApprove() { sfxPlay(SFX_APPROVE_F, SFX_APPROVE_D, SFX_N(SFX_APPROVE_F)); }
static void sfxDeny()    { sfxPlay(SFX_DENY_F,    SFX_DENY_D,    SFX_N(SFX_DENY_F));    }
static void sfxSave()    { sfxPlay(SFX_SAVE_F,    SFX_SAVE_D,    SFX_N(SFX_SAVE_F));    }
static void sfxAlert()   { sfxPlay(SFX_ALERT_F,   SFX_ALERT_D,   SFX_N(SFX_ALERT_F));   }
static void sfxMenu()    { sfxPlay(SFX_MENU_F,    SFX_MENU_D,    SFX_N(SFX_MENU_F));    }
static void sfxWarn()    { sfxPlay(SFX_WARN_F,    SFX_WARN_D,    SFX_N(SFX_WARN_F));    }
static void sfxWarn2()   { sfxPlay(SFX_WARN2_F,   SFX_WARN2_D,   SFX_N(SFX_WARN2_F));   }
#undef SFX_N

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

// "pet" at index 1 cycles the active species inline (like "demo" toggles);
// menuConfirm() keeps the menu open for this item so the user can mash it
// to flip through all 20+ species without reopening the menu each time.
const char* menuItems[] = { "settings", "pet", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 7;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;

// Full-screen pet preview with a bottom hint bar. Opened from the "pet"
// item in the main menu; while open, Left/Right step species and the HUD
// is suppressed so the pet has the canvas.
bool    petPickerOpen = false;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    sfxWarn();
    return;
  }

  sfxWarn2();
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  // Settings is the tallest modal — 10 rows. On landscape 135 we can't
  // afford 14px pitch (170 tall); drop to 10px pitch and a compact chrome
  // so the modal fits with a few pixels of margin.
#ifdef CARDPUTER_ADV
  const int PITCH = 10;
  const int PAD_TOP = 6;
  const int TEXT_OFFSET = 3;       // y inside the row for text baseline
#else
  const int PITCH = 14;
  const int PAD_TOP = 16;
  const int TEXT_OFFSET = 8;
#endif
  int mw = 118, mh = PAD_TOP + SETTINGS_N * PITCH + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    int rowY = my + TEXT_OFFSET + i * PITCH;
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, rowY);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, rowY);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

// Bottom 24 px hint bar shown while petPickerOpen. Leaves the rest of the
// sprite for the character/buddy tick, so the selection updates live as
// the user steps through species.
static void drawPetPicker() {
  const Palette& p = characterPalette();
  const int BAR_H = 24;
  const int y0 = H - BAR_H;
  spr.fillRect(0, y0, W, BAR_H, p.bg);
  spr.drawFastHLine(0, y0, W, p.textDim);

  uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
  uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;

  char label[32];
  if (buddyMode) snprintf(label, sizeof(label), "%s  %u/%u",
                          buddySpeciesName(), pos, total);
  else           snprintf(label, sizeof(label), "GIF  %u/%u", pos, total);

  spr.setTextSize(1);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(p.text, p.bg);
  spr.drawString(label, CX, y0 + 6);
  spr.setTextColor(p.textDim, p.bg);
#ifdef CARDPUTER_ADV
  spr.drawString(",  /   swap     Enter done", CX, y0 + 16);
#else
  spr.drawString("A: swap   B: done", CX, y0 + 16);
#endif
  spr.setTextDatum(TL_DATUM);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1:
      // Enter the live pet picker: close the menu, force home display so
      // the pet renders at full size, and let Left/Right step species
      // without the menu covering the preview.
      menuOpen = false;
      displayMode = DISP_NORMAL;
      applyDisplayMode();
      petPickerOpen = true;
      characterInvalidate();
      break;
    case 2: halPowerOff(); break;
    case 3:
    case 4:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 3) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 5: dataSetDemo(!dataDemo()); break;
    case 6: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 5) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = halVbusVolts() > 4.0f;
  halRtcGetTime(&_clkTm);
  halRtcGetDate(&_clkDt);
}

static void clockUpdateOrient() {
#ifdef CARDPUTER_ADV
  // Cardputer's home rotation is already landscape, so the tilt-to-rotate
  // clock path is redundant. Stay at the sprite-drawn clock face.
  clockOrient = 0;
  return;
#endif
  float ax, ay, az;
  halGetAccel(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
#ifdef CARDPUTER_ADV
    // Compact top-strip clock at size 1 (6x8 glyphs) in a 10px band so
    // the pet keeps ~125px underneath instead of competing for the
    // vertical space a size-2 clock ate.
    spr.fillRect(0, 0, W, 10, p.bg);
    char line[24];
    snprintf(line, sizeof(line), "%s%s  %s", hm, ss, dl);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(1); spr.setTextColor(p.text, p.bg);
    spr.drawString(line, CX, 5);
    spr.setTextDatum(TL_DATUM);
#else
    // 135x240 portrait. Bottom half — buddy naturally lives at y=0..82,
    // GIF peeks at top via peek mode. Clearing from 90 leaves both
    // untouched.
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
#endif
    return;
  }

  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      // ASCII glyphs don't self-clear; wipe the box each tick. Species
      // hardcode BUDDY_X_CENTER=67 / BUDDY_Y_OVERLAY=6 for particles so
      // keep portrait coords and just swap the surface — pet lands
      // upper-left of landscape, which is where we want it anyway.
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Full-frame GIFs paint every pixel (transparent → pal.bg), so a
      // per-tick clear just adds a visible black flash between wipe and
      // last scanline. The entry fillScreen on paintedOrient change
      // already covers the surround.
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(HOME_ROTATION);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  halGetAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = INFO_PANEL_TOP;
  // Full-sprite wipe on Cardputer: the main loop skips character/buddy
  // rendering in INFO mode, so this is where the old pixels get cleared.
  // StickC keeps the partial wipe so the portrait pet peeks above y=70.
#ifdef CARDPUTER_ADV
  spr.fillSprite(p.bg);
#else
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
#endif
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
#ifdef CARDPUTER_ADV
    ln("I watch your Claude desktop");
    ln("sessions. I sleep when nothing's");
    ln("happening, wake when you start");
    ln("working, get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Y = approve, N = deny");
    ln("on a pending prompt.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("20 species. Menu > pet to cycle.");
#else
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("20 species. Settings");
    ln("> ascii pet to cycle.");
#endif

  } else if (infoPage == 1) {
#ifdef CARDPUTER_ADV
    _infoHeader(p, y, "KEYBOARD", infoPage);
#else
    _infoHeader(p, y, "BUTTONS", infoPage);
#endif
#ifdef CARDPUTER_ADV
    spr.setTextColor(p.text, p.bg);    ln("Enter  confirm / select");
    spr.setTextColor(p.textDim, p.bg); ln("`/Del  close modal (Esc)"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln(";  .   up / down");
    spr.setTextColor(p.textDim, p.bg); ln(",  /   previous / next page"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Y  N   approve / deny");
    spr.setTextColor(p.textDim, p.bg); ln("       on a pending prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("M      open menu");
    spr.setTextColor(p.textDim, p.bg); ln("G      toggle demo mode"); y += 4;
    spr.setTextColor(p.textDim, p.bg); ln("Power: slide switch on side");
#else
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");
#endif

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(halBatteryVolts() * 1000);
    int iBat_mA = (int)halBatteryMilliAmps();
    int vBus_mV = (int)(halVbusVolts() * 1000);
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", halTempC());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
#ifdef CARDPUTER_ADV
    ln("M5 Cardputer ADV");
    ln("ESP32-S3");
#else
    ln("M5StickC Plus");
    ln("ESP32 + AXP192");
#endif
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  uint32_t waited = (millis() - promptArrivedMs) / 1000;

#ifdef CARDPUTER_ADV
  // Full-screen landscape approval — pet is suppressed in prompt mode,
  // so we claim the whole 240x135 canvas. Tool name gets a big size-2
  // centered line; the hint wraps at 38 chars (240 / 6).
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(4, 6);
  spr.printf("approve?  %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 20 ? 2 : 1);
  spr.setTextDatum(MC_DATUM);
  spr.drawString(tama.promptTool, CX, 40);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);

  spr.setTextColor(p.textDim, p.bg);
  const int WRAP = 38;
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, 72);
  if (hlen <= WRAP) {
    spr.print(tama.promptHint);
  } else {
    char buf[WRAP + 1];
    memcpy(buf, tama.promptHint, WRAP); buf[WRAP] = 0;
    spr.print(buf);
    spr.setCursor(4, 84);
    spr.printf("%.38s", tama.promptHint + WRAP);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 14);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 14);
    spr.print("Y: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 52, H - 14);
    spr.print("N: deny");
  }
#else
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Size 2 only if it fits one line (~10 chars at 12px on 135px screen)
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  // Hint wraps at ~21 chars to two lines under the tool name
  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("B: deny");
  }
#endif
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = PET_PANEL_TOP;
#ifdef CARDPUTER_ADV
  spr.fillSprite(p.bg);   // full-screen stats; kill any pet pixels above
#else
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
#endif
  spr.setTextSize(1);

  auto tokFmt = [&](const char* label, uint32_t v, int xPx, int yPx) {
    spr.setCursor(xPx, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };

  uint8_t  mood    = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  uint8_t  fed     = statsFedProgress();
  uint8_t  en      = statsEnergyTier();
  uint16_t enCol   = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  uint32_t nap     = stats().napSeconds;

#ifdef CARDPUTER_ADV
  // 240x135 landscape: widgets on the left 130 px, counters on the right.
  // Pitch tightened from 20/24 to 16/18 so the level badge ends cleanly
  // inside H=135 instead of spilling off the bottom.
  int y = TOP + 14;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 16;
  spr.setCursor(6, y - 2); spr.print("fed");
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else         spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 16;
  spr.setCursor(6, y - 2); spr.print("energy");
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else        spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 18;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  // Right column: labels abbreviated so each row fits in ~95 px at size 1.
  const int RX = 140;
  int yr = TOP + 12;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(RX, yr);      spr.printf("appr   %u", stats().approvals);
  spr.setCursor(RX, yr + 10); spr.printf("deny   %u", stats().denials);
  spr.setCursor(RX, yr + 20); spr.printf("nap    %luh%02lum", nap / 3600, (nap / 60) % 60);
  tokFmt("tokens ", stats().tokens,     RX, yr + 30);
  tokFmt("today  ", tama.tokensToday,   RX, yr + 40);
#else
  // Original 135x240 portrait — widgets stacked with counts below.
  int y = TOP + 16;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else         spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else        spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);      spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10); spr.printf("denied   %u", stats().denials);
  spr.setCursor(6, y + 20); spr.printf("napped   %luh%02lum", nap / 3600, (nap / 60) % 60);
  tokFmt("tokens   ", stats().tokens,   6, y + 30);
  tokFmt("today    ", tama.tokensToday, 6, y + 40);
#endif
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = PET_PANEL_TOP;
#ifdef CARDPUTER_ADV
  spr.fillSprite(p.bg);
#else
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
#endif
  spr.setTextSize(1);

#ifdef CARDPUTER_ADV
  // Two-column landscape layout — MOOD + FED on the left, ENERGY + idle
  // behavior + keybinds on the right. Both columns start under the PET
  // header drawn by drawPet().
  int yL = TOP + 14;
  auto lnL = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, yL); spr.print(s); yL += 9;
  };
  auto gapL = [&]() { yL += 4; };
  lnL(p.body,    "MOOD");
  lnL(p.textDim, " approve fast = up");
  lnL(p.textDim, " deny lots = down"); gapL();
  lnL(p.body,    "FED");
  lnL(p.textDim, " 50K tokens =");
  lnL(p.textDim, " level up + confetti");

  const int RX = 130;
  int yR = TOP + 14;
  auto lnR = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(RX, yR); spr.print(s); yR += 9;
  };
  auto gapR = [&]() { yR += 4; };
  lnR(p.body,    "ENERGY");
  lnR(p.textDim, " face-down to nap");
  lnR(p.textDim, " refills to full");  gapR();
  lnR(p.textDim, "idle 2m = off");
  lnR(p.textDim, "any key = wake");    gapR();
  lnR(p.textDim, "Y: approve  N: deny");
  lnR(p.textDim, "M: menu   ;./,//: nav");
#else
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 2m = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
#endif
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = PET_PANEL_TOP;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 3, LH = 8, WIDTH = 21;
  const int AREA = SHOW * LH + 4;
  // 21-char wrap × 6px per size-1 glyph = 126 px block. Portrait (135 wide)
  // pads left at x=4; landscape (240 wide) block-centers so the strip
  // sits under the pet instead of crammed into the bottom-left corner.
#ifdef CARDPUTER_ADV
  const int HUD_X = (W - WIDTH * 6) / 2;
#else
  const int HUD_X = 4;
#endif
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
#ifdef CARDPUTER_ADV
    // Single-line fallback — center on the actual string length.
    spr.setTextDatum(MC_DATUM);
    spr.drawString(tama.msg, CX, H - LH / 2 - 2);
    spr.setTextDatum(TL_DATUM);
#else
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
#endif
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][24];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(HUD_X, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  halInit();
  M5.Lcd.setRotation(HOME_ROTATION);
  halImuInit();
  halBeepInit();
  startBt();
  halLedSet(0, 0, 0);   // make sure the LED isn't stuck on from a reset
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.createSprite(W, H);
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  halUpdate();
  halBeepUpdate();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off. halLedSet dispatches to the
  // StickC red GPIO or the Cardputer NeoPixel; both clear on (0,0,0).
  if (activeState == P_ATTENTION && settings().led) {
    bool on = (now / 400) % 2;
    halLedSet(on ? 180 : 0, on ? 40 : 0, 0);   // red-orange pulse
  } else {
    halLedSet(0, 0, 0);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      sfxAlert();       // prompt arrived
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = petPickerOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (halBtnA().isPressed() || halBtnB().isPressed()) {
    if (screenOff) {
      if (halBtnA().isPressed()) swallowBtnA = true;
      if (halBtnB().isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  // No-op on Cardputer for now — halPowerBtnPress() always returns 0.
  if (halPowerBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      halScreenPower(false);
      screenOff = true;
    }
  }

  if (halBtnA().pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    sfxMenu();
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (halBtnA().wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      // On Cardputer, HalKey::Approve (also wired to Enter) handles all
      // modal confirmations. BtnA wasReleased fires BtnA in *parallel*
      // with the Approve event, so we skip the modal branches here to
      // avoid double-firing (Enter would first cycle the highlight, then
      // confirm the wrong row). Prompts and home-screen cycling keep the
      // BtnA path since they don't overlap with Approve's targets.
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        sfxApprove();
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
#ifndef CARDPUTER_ADV
      } else if (resetOpen) {
        sfxNav();
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        sfxNav();
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        sfxNav();
        menuSel = (menuSel + 1) % MENU_N;
#endif
      } else if (!menuOpen && !settingsOpen && !resetOpen) {
        sfxNav();
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (halBtnB().wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      sfxDeny();
#ifndef CARDPUTER_ADV
    } else if (resetOpen) {
      sfxConfirm();
      applyReset(resetSel);
    } else if (settingsOpen) {
      sfxConfirm();
      applySetting(settingsSel);
    } else if (menuOpen) {
      sfxConfirm();
      menuConfirm();
#endif
    } else if (!menuOpen && !settingsOpen && !resetOpen && displayMode == DISP_INFO) {
      sfxNav();
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (!menuOpen && !settingsOpen && !resetOpen && displayMode == DISP_PET) {
      sfxNav();
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else if (!menuOpen && !settingsOpen && !resetOpen) {
      sfxNav();
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // Keyboard shortcuts (Cardputer). halPollKey() returns None on StickC,
  // so this loop is a no-op there. Up/Down navigate within whichever menu
  // is open; Left/Right flip pages in INFO/PET modes; Y/N answer pending
  // prompts directly; M opens the menu; G toggles demo mode. The Cardputer
  // has a hardware power slide switch, so no software power key.
  while (true) {
    HalKey k = halPollKey();
    if (k == HalKey::None) break;
    wake();

    // Re-evaluate inPrompt here: Enter fires both BtnA (which approves
    // above) and HalKey::Approve (this loop). If we trusted the outer
    // snapshot, a single Enter would approve twice in one frame.
    bool promptLive = tama.promptId[0] && !responseSent;

    // Pending approval overrides everything else — Y/N answer directly.
    if (promptLive) {
      if (k == HalKey::Approve) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        sfxApprove();
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (k == HalKey::Deny) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        statsOnDenial();
        sfxDeny();
      }
      continue;
    }

    // Modal menus — Up/Down move highlight without wrapping through all
    // items like BtnA does; Approve (Y or Enter-via-BtnA analogue) picks
    // the current row by invoking the same apply* functions BtnB triggers.
    // Any time Approve/Back is consumed by a modal, suppress the matching
    // button's next release. Enter/Del are wired to both BtnA/B *and*
    // HalKey events; without this the release fires BtnA.wasReleased on
    // the next frame and triggers home-screen displayMode cycling.
    auto consumedEnter = [&]() { halBtnA().suppressPending(); };
    auto consumedDel   = [&]() { halBtnB().suppressPending(); };

    // Pet picker overrides the modal checks below: the picker isn't a
    // modal that covers the canvas, so we drive its arrow-key behavior
    // here before falling into the main-menu branches.
    if (petPickerOpen) {
      if      (k == HalKey::Left)    { prevPet(); sfxNav(); }
      else if (k == HalKey::Right)   { nextPet(); sfxNav(); }
      else if (k == HalKey::Approve
            || k == HalKey::Back) {
        // "Saved" feedback: rising 3-note fanfare + P_HEART one-shot so
        // the pet visibly thanks the user. The index is already
        // persisted by nextPet/prevPet — this is just the affordance.
        sfxSave();
        triggerOneShot(P_HEART, 1500);
        petPickerOpen = false;
        characterInvalidate();
        if (buddyMode) buddyInvalidate();
        if (k == HalKey::Approve) consumedEnter(); else consumedDel();
      }
      continue;
    }

    if (resetOpen) {
      if      (k == HalKey::Up)      { resetSel = (resetSel + RESET_N - 1) % RESET_N; sfxNav(); resetConfirmIdx = 0xFF; }
      else if (k == HalKey::Down)    { resetSel = (resetSel + 1) % RESET_N;           sfxNav(); resetConfirmIdx = 0xFF; }
      else if (k == HalKey::Approve) { sfxConfirm(); applyReset(resetSel); consumedEnter(); }
      else if (k == HalKey::Back)    { sfxBack(); resetOpen = false; consumedDel(); }
      continue;
    }
    if (settingsOpen) {
      if      (k == HalKey::Up)      { settingsSel = (settingsSel + SETTINGS_N - 1) % SETTINGS_N; sfxNav(); }
      else if (k == HalKey::Down)    { settingsSel = (settingsSel + 1) % SETTINGS_N;              sfxNav(); }
      else if (k == HalKey::Approve) { sfxConfirm(); applySetting(settingsSel); consumedEnter(); }
      else if (k == HalKey::Back)    { sfxBack(); settingsOpen = false; characterInvalidate(); consumedDel(); }
      continue;
    }
    if (menuOpen) {
      if      (k == HalKey::Up)      { menuSel = (menuSel + MENU_N - 1) % MENU_N; sfxNav(); }
      else if (k == HalKey::Down)    { menuSel = (menuSel + 1) % MENU_N;          sfxNav(); }
      else if (k == HalKey::Approve) { sfxConfirm(); menuConfirm(); consumedEnter(); }
      else if (k == HalKey::Back)    { sfxBack(); menuOpen = false; characterInvalidate(); consumedDel(); }
      continue;
    }

    // Idle home screen.
    switch (k) {
      case HalKey::Menu:
        menuOpen = true; menuSel = 0;
        sfxMenu();
        break;
      case HalKey::Demo:
        dataSetDemo(!dataDemo());
        sfxConfirm();
        break;
      case HalKey::Left:
        if      (displayMode == DISP_INFO) infoPage = (infoPage + INFO_PAGES - 1) % INFO_PAGES;
        else if (displayMode == DISP_PET)  petPage  = (petPage  + PET_PAGES  - 1) % PET_PAGES;
        sfxNav();
        break;
      case HalKey::Right:
        if      (displayMode == DISP_INFO) infoPage = (infoPage + 1) % INFO_PAGES;
        else if (displayMode == DISP_PET)  { petPage = (petPage + 1) % PET_PAGES; applyDisplayMode(); }
        sfxNav();
        break;
      case HalKey::Up:
        msgScroll = (msgScroll >= 30) ? 30 : msgScroll + 1;
        sfxNav();
        break;
      case HalKey::Down:
        msgScroll = msgScroll == 0 ? 0 : msgScroll - 1;
        sfxNav();
        break;
      default: break;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  // Clock-mood picker overwrites activeState unconditionally, which also
  // silently wipes one-shots (shake→dizzy, level-up→celebrate, etc.). Skip
  // it while a one-shot is live so those animations play out first.
  if (clocking && (int32_t)(now - oneShotUntil) >= 0) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); sfxAlert(); }
  lastPasskey = pk;

  // On Cardputer both INFO and PET pages claim the full canvas (no pet
  // underneath), so skip the pet tick there to avoid drawing pixels that
  // drawInfo/drawPet will immediately wipe. Same deal for approval
  // prompts — the landscape prompt is full-screen. Portrait StickC still
  // renders the peek pet above the info/pet panels and the partial
  // bottom-strip approval.
  bool suppressPet = napping || screenOff || landscapeClock;
#ifdef CARDPUTER_ADV
  suppressPet = suppressPet || displayMode == DISP_INFO
                            || displayMode == DISP_PET
                            || inPrompt;
#endif
  if (suppressPet) {
    // skip sprite render — face-down, powered off, landscape clock, or
    // full-screen info (Cardputer).
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (petPickerOpen) drawPetPicker();   // thin hint bar + live pet above
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    halBrightness(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    halScreenPower(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
