#include "../buddy.h"
#include "../buddy_common.h"
#include "../hal.h"
#include <string.h>

extern TFT_eSprite spr;

namespace llama {

// Creamy wool. Warm off-white / beige (#EEDDB8 → RGB565 0xEEFD).
static const uint16_t LLAMA_WOOL = 0xEEFD;

// ─── SLEEP ───  neck tucked down, slow breath
static void doSleep(uint32_t t) {
  static const char* const TUCK[5]    = { "            ", "            ", "   .---.    ", "  ( - - )   ", "   \\___/    " };
  static const char* const BREATHE[5] = { "            ", "            ", "   .---.    ", "  ( - _ )   ", "   \\___/~   " };
  static const char* const SNORE[5]   = { "            ", "            ", "   .---.    ", "  ( ~ ~ )   ", "   \\___/    " };
  static const char* const DREAM[5]   = { "            ", "            ", "   .---.    ", "  ( u u )   ", "   \\___/    " };
  const char* const* P[4] = { TUCK, BREATHE, SNORE, TUCK };
  static const uint8_t SEQ[] = { 0,1,0,1,2,2,0,1,3,3,0,1,0,1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, LLAMA_WOOL);

  int pz = t % 14;
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 14 + pz / 2, BUDDY_Y_OVERLAY + 18 - pz);
  buddyPrint("z");
}

// ─── IDLE ───  chewing, ear flick, long neck bob
static void doIdle(uint32_t t) {
  static const char* const REST[5]    = { "    _/|     ", "   /   \\    ", "  ( o o )   ", "   \\_o_/    ", "    | |     " };
  static const char* const CHEW1[5]   = { "    _/|     ", "   /   \\    ", "  ( o o )   ", "   \\_v_/    ", "    | |     " };
  static const char* const CHEW2[5]   = { "    _/|     ", "   /   \\    ", "  ( o o )   ", "   \\_^_/    ", "    | |     " };
  static const char* const EAR_L[5]   = { "    </|     ", "   /   \\    ", "  ( o o )   ", "   \\_o_/    ", "    | |     " };
  static const char* const EAR_R[5]   = { "    _/>     ", "   /   \\    ", "  ( o o )   ", "   \\_o_/    ", "    | |     " };
  static const char* const BLINK[5]   = { "    _/|     ", "   /   \\    ", "  ( - - )   ", "   \\_o_/    ", "    | |     " };
  const char* const* P[6] = { REST, CHEW1, CHEW2, EAR_L, EAR_R, BLINK };
  static const uint8_t SEQ[] = { 0,0,1,2,1,2,0,0,3,0,4,0,5,0,0,1,2,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, LLAMA_WOOL);
}

// ─── BUSY ───  neck stretching, grazing at rhythm
static void doBusy(uint32_t t) {
  static const char* const UP[5]      = { "    _/|     ", "   /   \\    ", "  ( o o )   ", "   \\_v_/    ", "    | |     " };
  static const char* const DOWN[5]    = { "            ", "    _/|     ", "   /   \\    ", "  ( o o )v  ", "    | |     " };
  static const char* const CHOMP[5]   = { "            ", "    _/|     ", "   /   \\    ", "  ( o o )^  ", "    | |     " };
  const char* const* P[3] = { UP, DOWN, CHOMP };
  static const uint8_t SEQ[] = { 0,0,1,2,1,2,0,0,1,2 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, LLAMA_WOOL);

  static const char* const DOTS[] = { ".  ", ".. ", "...", "   " };
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 22, BUDDY_Y_OVERLAY + 14);
  buddyPrint(DOTS[t % 4]);
}

// ─── ATTENTION ───  ears up, eyes wide, ready to spit
static void doAttention(uint32_t t) {
  static const char* const ALERT[5]   = { "    /|\\     ", "   /   \\    ", "  ( O O )   ", "   \\___/    ", "    | |     " };
  static const char* const BRACE[5]   = { "    /|\\     ", "   /   \\    ", "  ( O O )   ", "   \\_O_/    ", "   /| |\\    " };
  static const char* const SPIT[5]    = { "    /|\\     ", "   /   \\    ", "  ( O O )~~*", "   \\___/    ", "    | |     " };
  const char* const* P[3] = { ALERT, BRACE, SPIT };
  static const uint8_t SEQ[] = { 0,0,1,0,2,2,0,1,0,0 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, LLAMA_WOOL);

  if ((t / 2) & 1) {
    buddySetColor(BUDDY_YEL);
    buddySetCursor(BUDDY_X_CENTER - 6, BUDDY_Y_OVERLAY + 2);
    buddyPrint("!");
  }
}

// ─── CELEBRATE ───  llama hops, wool puffs
static void doCelebrate(uint32_t t) {
  static const char* const HOP_UP[5]  = { "    /|\\     ", "   /   \\    ", "  ( ^ ^ )   ", "   \\_W_/    ", "   /| |\\    " };
  static const char* const HOP_DN[5]  = { "            ", "    /|\\     ", "   /   \\    ", "  ( ^ ^ )   ", "   \\_W_/    " };
  static const char* const POSE[5]    = { "    /|\\     ", "   /   \\    ", "  ( * * )   ", "  /\\_W_/\\   ", "    | |     " };
  const char* const* P[3] = { HOP_UP, HOP_DN, POSE };
  static const uint8_t SEQ[] = { 0,1,0,1,2,2,0,1,0,1,2,0 };
  static const int8_t Y_BOB[] = { -3,0,-3,0,0,0,-3,0,-3,0,0,-3 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_BOB[beat], LLAMA_WOOL);

  static const uint16_t cols[] = { BUDDY_YEL, BUDDY_CYAN, BUDDY_WHITE, BUDDY_GREEN, BUDDY_HEART };
  for (int i = 0; i < 5; i++) {
    int phase = (t * 2 + i * 9) % 20;
    int x = BUDDY_X_CENTER - 30 + i * 14;
    int y = BUDDY_Y_OVERLAY - 4 + phase;
    if (y > BUDDY_Y_BASE + 18 || y < 0) continue;
    buddySetColor(cols[i % 5]);
    buddySetCursor(x, y);
    buddyPrint((i + (int)(t / 2)) & 1 ? "*" : "o");
  }
}

// ─── DIZZY ───  neck sways, orbit stars
static void doDizzy(uint32_t t) {
  static const char* const SWAY_L[5]  = { "   /|       ", "   / \\      ", "  ( @ @ )   ", "   \\___/    ", "   | |      " };
  static const char* const SWAY_R[5]  = { "      |\\    ", "     / \\    ", "    ( @ @ ) ", "     \\___/  ", "      | |   " };
  static const char* const WOOZY[5]   = { "    _/|     ", "   /   \\    ", "  ( x @ )   ", "   \\_~_/    ", "    | |     " };
  const char* const* P[3] = { SWAY_L, SWAY_R, WOOZY };
  static const uint8_t SEQ[] = { 0,1,0,1,2,2,0,1,0,1 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, LLAMA_WOOL);

  static const int8_t OX[] = { 0, 6, 8, 6, 0, -6, -8, -6 };
  static const int8_t OY[] = { -5, -3, 0, 3, 5, 3, 0, -3 };
  uint8_t p = t % 8;
  buddySetColor(BUDDY_YEL);
  buddySetCursor(BUDDY_X_CENTER + OX[p] - 2, BUDDY_Y_OVERLAY + 6 + OY[p]);
  buddyPrint("*");
}

// ─── HEART ───  smitten stare, gentle sway, rising hearts
static void doHeart(uint32_t t) {
  static const char* const DREAMY[5]  = { "    _/|     ", "   /   \\    ", "  ( ^ ^ )   ", "   \\_u_/    ", "    | |     " };
  static const char* const BLUSH[5]   = { "    _/|     ", "   /   \\    ", "  (#^ ^#)   ", "   \\_u_/    ", "    | |     " };
  static const char* const NUZZLE[5]  = { "    _/|     ", "   /   \\~   ", "  ( ^ ^ )   ", "   \\_u_/    ", "    | |     " };
  const char* const* P[3] = { DREAMY, BLUSH, NUZZLE };
  static const uint8_t SEQ[] = { 0,0,1,0, 2,2,0, 1,0,1, 0,0 };
  static const int8_t Y_BOB[] = { 0,-1,0,-1, 0,-1,0, -1,0,0, -1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_BOB[beat], LLAMA_WOOL);

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

}  // namespace llama

extern const Species LLAMA_SPECIES = {
  "llama",
  llama::LLAMA_WOOL,
  { llama::doSleep, llama::doIdle, llama::doBusy, llama::doAttention,
    llama::doCelebrate, llama::doDizzy, llama::doHeart }
};
