#include "../buddy.h"
#include "../buddy_common.h"
#include "../hal.h"
#include <string.h>

extern TFT_eSprite spr;

namespace doge {

// Much coin color. Shiba tan (roughly #FD9830 → RGB565 0xFCC6).
static const uint16_t DOGE_TAN = 0xFCC6;

// ─── SLEEP ───  curled shiba with drifting Zzz
static void doSleep(uint32_t t) {
  static const char* const FLAT[5]    = { "            ", "            ", "   ,--.-.   ", "  ( - . - ) ", "   `~~~~`   " };
  static const char* const SNORE[5]   = { "            ", "            ", "   ,--.-.   ", "  ( - _ - ) ", "   `~~~~`   " };
  static const char* const DREAM[5]   = { "            ", "            ", "   ,--.-.   ", "  ( u . u ) ", "   `~~~~`   " };
  static const char* const PAW[5]     = { "            ", "            ", "   ,--.-.   ", "  ( - . - )~", "   `~~~~`   " };
  const char* const* P[4] = { FLAT, SNORE, DREAM, PAW };
  static const uint8_t SEQ[] = { 0,0,1,1,0,0,2,0,3,0,1,1,2,2,0,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, DOGE_TAN);

  // Zzz drift up-right
  int p1 = t % 14;
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 18 + p1 / 2, BUDDY_Y_OVERLAY + 20 - p1);
  buddyPrint("z");
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 22 + (p1 + 6) / 2, BUDDY_Y_OVERLAY + 14 - p1 / 2);
  buddyPrint("Z");
}

// ─── IDLE ───  blinks, side glance, tongue pant
static void doIdle(uint32_t t) {
  static const char* const REST[5]    = { "            ", "   ,-..-,   ", "  ( o . o ) ", "   \\  w  /  ", "    `~~'    " };
  static const char* const BLINK[5]   = { "            ", "   ,-..-,   ", "  ( - . - ) ", "   \\  w  /  ", "    `~~'    " };
  static const char* const LOOK_L[5]  = { "            ", "   ,-..-,   ", "  (o  .  o) ", "   \\  w  /  ", "    `~~'    " };
  static const char* const PANT[5]    = { "            ", "   ,-..-,   ", "  ( o . o ) ", "   \\ :P  /  ", "    `~~'    " };
  static const char* const EARS[5]    = { "            ", "   /-..-/   ", "  ( o . o ) ", "   \\  w  /  ", "    `~~'    " };
  const char* const* P[5] = { REST, BLINK, LOOK_L, PANT, EARS };
  static const uint8_t SEQ[] = { 0,0,0,1,0,0,2,0,0,3,3,0,4,0,0,1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, DOGE_TAN);
}

// ─── BUSY ───  "typing" with tongue + dot ticker. such code.
static void doBusy(uint32_t t) {
  static const char* const TYPE1[5]   = { "            ", "   ,-..-,   ", "  ( o . o ) ", "   \\ :v  /  ", "    `~~'    " };
  static const char* const TYPE2[5]   = { "            ", "   ,-..-,   ", "  ( o . o ) ", "   \\  v: /  ", "    `~~'    " };
  static const char* const STARE[5]   = { "            ", "   ,-..-,   ", "  ( O . O ) ", "   \\  w  /  ", "    `~~'    " };
  const char* const* P[3] = { TYPE1, TYPE2, STARE };
  static const uint8_t SEQ[] = { 0,1,0,1,2,2,0,1,0,1 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, DOGE_TAN);

  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 22, BUDDY_Y_OVERLAY + 14);
  buddyPrint(DOTS[t % 6]);
}

// ─── ATTENTION ───  ears up, alert bark, very watch
static void doAttention(uint32_t t) {
  static const char* const ALERT[5]   = { "            ", "   /^.^\\    ", "  ( O . O ) ", "   \\  v  /  ", "    `~~'    " };
  static const char* const BARK[5]    = { "    !       ", "   /^.^\\    ", "  ( O ! O ) ", "   \\ woof/  ", "    `~~'    " };
  static const char* const SCAN_L[5]  = { "            ", "   /^.^\\    ", "  (O    O ) ", "   \\  v  /  ", "    `~~'    " };
  static const char* const SCAN_R[5]  = { "            ", "   /^.^\\    ", "  ( O    O) ", "   \\  v  /  ", "    `~~'    " };
  const char* const* P[4] = { ALERT, BARK, SCAN_L, SCAN_R };
  static const uint8_t SEQ[] = { 0,0,2,0,3,0,1,1,0,0 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, DOGE_TAN);

  if (((t / 2) & 1) && (SEQ[beat] != 1)) {
    buddySetColor(BUDDY_YEL);
    buddySetCursor(BUDDY_X_CENTER - 6, BUDDY_Y_OVERLAY + 2);
    buddyPrint("!");
  }
}

// ─── CELEBRATE ───  much wow. many hearts + rising stars
static void doCelebrate(uint32_t t) {
  static const char* const JUMP[5]    = { "   \\o/      ", "   ,-..-,   ", "  ( ^ . ^ ) ", "   \\  W  /  ", "    `~~'    " };
  static const char* const WOW[5]     = { "  wow!      ", "   ,-..-,   ", "  ( o . o ) ", "   \\  W  /  ", "    `~~'    " };
  static const char* const DANCE_L[5] = { "            ", "   ,-..-,   ", " /( * . * )  ", "   \\  W  /  ", "    `~~'    " };
  static const char* const DANCE_R[5] = { "            ", "   ,-..-,   ", "  ( * . * )\\ ", "   \\  W  /  ", "    `~~'    " };
  const char* const* P[4] = { JUMP, WOW, DANCE_L, DANCE_R };
  static const uint8_t SEQ[] = { 0,0,2,3,2,3,1,1,0,0,2,3,1,0 };
  static const int8_t Y_BOB[] = { -3,0,0,0,0,0,0,0,-3,0,0,0,0,-3 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_BOB[beat], DOGE_TAN);

  static const uint16_t cols[] = { BUDDY_YEL, BUDDY_HEART, BUDDY_CYAN, BUDDY_WHITE, BUDDY_GREEN };
  for (int i = 0; i < 5; i++) {
    int phase = (t * 2 + i * 9) % 20;
    int x = BUDDY_X_CENTER - 30 + i * 14;
    int y = BUDDY_Y_OVERLAY - 4 + phase;
    if (y > BUDDY_Y_BASE + 18 || y < 0) continue;
    buddySetColor(cols[i % 5]);
    buddySetCursor(x, y);
    buddyPrint((i + (int)(t / 2)) & 1 ? "*" : ".");
  }
}

// ─── DIZZY ───  head tilt + orbit stars. much confuse.
static void doDizzy(uint32_t t) {
  static const char* const TILT_L[5]  = { "            ", "  ,-..-,    ", " ( @ . @ )  ", "  \\  ~  /   ", "   `~~'     " };
  static const char* const TILT_R[5]  = { "            ", "    ,-..-,  ", "   ( @ . @ )", "    \\  ~  / ", "     `~~'   " };
  static const char* const WOOZY[5]   = { "            ", "   ,-..-,   ", "  ( x . @ ) ", "   \\  v  /~ ", "    `~~'    " };
  const char* const* P[3] = { TILT_L, TILT_R, WOOZY };
  static const uint8_t SEQ[] = { 0,1,0,1, 2,2, 0,1,0,1 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, DOGE_TAN);

  static const int8_t OX[] = { 0, 6, 8, 6, 0, -6, -8, -6 };
  static const int8_t OY[] = { -5, -3, 0, 3, 5, 3, 0, -3 };
  uint8_t p = t % 8;
  buddySetColor(BUDDY_CYAN);
  buddySetCursor(BUDDY_X_CENTER + OX[p] - 2, BUDDY_Y_OVERLAY + 6 + OY[p]);
  buddyPrint("*");
}

// ─── HEART ───  smitten eyes + rising hearts. very love.
static void doHeart(uint32_t t) {
  static const char* const SMITTEN[5] = { "            ", "   ,-..-,   ", "  ( ^ . ^ ) ", "   \\  u  /  ", "    `~~'    " };
  static const char* const DREAMY[5]  = { "            ", "   ,-..-,   ", "  (<3 . <3) ", "   \\  u  /  ", "    `~~'    " };
  static const char* const NUZZLE[5]  = { "            ", "   ,-\\/-,   ", "  ( * . * ) ", "   \\  u  /~ ", "    `~~'    " };
  const char* const* P[3] = { SMITTEN, DREAMY, NUZZLE };
  static const uint8_t SEQ[] = { 0,0,1,0, 2,2,0, 1,0,1, 0,0 };
  static const int8_t Y_BOB[] = { 0,-1,0,-1, 0,-1,0, -1,0,0, -1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_BOB[beat], DOGE_TAN);

  buddySetColor(BUDDY_HEART);
  for (int i = 0; i < 5; i++) {
    int phase = (t + i * 4) % 16;
    int y = BUDDY_Y_OVERLAY + 16 - phase;
    if (y < -2 || y > BUDDY_Y_BASE) continue;
    int x = BUDDY_X_CENTER - 20 + i * 8 + ((phase / 3) & 1) * 2 - 1;
    buddySetCursor(x, y);
    buddyPrint("v");
  }
}

}  // namespace doge

extern const Species DOGE_SPECIES = {
  "doge",
  doge::DOGE_TAN,
  { doge::doSleep, doge::doIdle, doge::doBusy, doge::doAttention,
    doge::doCelebrate, doge::doDizzy, doge::doHeart }
};
