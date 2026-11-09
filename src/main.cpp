// Wireless DMX giant bulb: ESP32 + nRF24L01+ (RX only) + SK6812 RGBW + SSD1306
//
// Wireless DMX protocol (Donner / cheap nRF24 transmitters):
//   250kbps, CRC16, no auto-ack, 32-byte payload, 5-byte pipe address:
//   [rfChan, unitID, ~rfChan, ~unitID, rfChan+unitID]
//   payload: magic(0x80|0xA0), payloadID, highestChannel(u16), 28 DMX bytes

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#include <Adafruit_NeoPixel.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include "config.h"

// ================================================================ types
struct RGBW { uint8_t r, g, b, w; };

enum Screen : uint8_t { SCR_ADDR = 0, SCR_CHAN, SCR_COLOR, SCR_COUNT };

struct Settings {
  uint16_t addr    = 1;   // DMX start address 1..509
  uint8_t  id      = 0;   // color group: 0=Auto, 1..7
  uint8_t  screen  = SCR_ADDR;
  uint8_t  program = 0;
  uint8_t  rfch    = 0;   // last locked RF channel (scan starts here)
  uint8_t  lockedId = 1;  // last locked unit ID (used when id==Auto)
  uint8_t  bright  = 100; // color-mode brightness, percent 5..100
} cfg;

// ================================================================ color programs
struct Preset { const char* name; RGBW c; };
// Ordered roughly around the color wheel so fade/step look like a rainbow.
const Preset PRESETS[] = {
  {"Red",         {255,   0,   0,   0}},
  {"Orange",      {255,  60,   0,   0}},
  {"Gold",        {255, 150,   0,   0}},
  {"Warm White",  {  0,   0,   0, 255}},
  {"Green",       {  0, 255,   0,   0}},
  {"Pastel Mint", { 30, 255, 100, 70}},
  {"Teal",        {  0, 200, 110,   0}},
  {"Pastel Blue", { 30,  50, 255,  25}},
  {"Blue",        {  0,   0, 255,   0}},
  {"Purple",      {100,   0, 255,   0}},
  {"Hot Pink",    {255,   0,  70,   0}},
  {"Pastel Pink", {255,  15, 100,  15}},
};
constexpr uint8_t NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

const char* const EFFECT_NAMES[] = {"Fade (slow)", "Fade (fast)", "Step (slow)", "Step (fast)", "Candle"};
constexpr uint8_t NUM_EFFECTS  = sizeof(EFFECT_NAMES) / sizeof(EFFECT_NAMES[0]);
constexpr uint8_t NUM_PROGRAMS = NUM_PRESETS + NUM_EFFECTS;

const char* const ID_NAMES[] = {"Auto", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"};

const char* programName(uint8_t p) {
  return p < NUM_PRESETS ? PRESETS[p].name : EFFECT_NAMES[p - NUM_PRESETS];
}

// ================================================================ globals
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_LED_DATA, LED_COLOR_ORDER);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_OLED_SCL, PIN_OLED_SDA);
RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);
Preferences prefs;

constexpr uint8_t WDMX_MAGIC_1 = 0x80, WDMX_MAGIC_2 = 0xA0;
constexpr uint8_t PKT_DATA = 28, PKT_COUNT = 19;  // 19*28 = 532 >= 512
struct __attribute__((packed)) WdmxPkt {
  uint8_t magic, payloadID;
  uint16_t highest;
  uint8_t data[PKT_DATA];
};
static_assert(sizeof(WdmxPkt) == 32, "packet must be 32 bytes");

uint8_t  dmx[PKT_COUNT * PKT_DATA];
bool     radioOk = false, locked = false, firstTry = true;
uint8_t  scanCh = 0, scanId = 1;
uint32_t dwellStart = 0, lastPkt = 0, lastPoll = 0;  // lastPkt==0 -> never received

bool     dirty = false;
uint32_t dirtyAt = 0, lastActivity = 0;
bool     displayOn = true;

// ================================================================ persistence
void loadSettings() {
  prefs.begin("bulb", true);
  cfg.addr     = prefs.getUShort("addr", 1);
  cfg.id       = prefs.getUChar("id", 0);
  cfg.screen   = prefs.getUChar("screen", SCR_ADDR);
  cfg.program  = prefs.getUChar("prog", 0);
  cfg.rfch     = prefs.getUChar("rfch", 0);
  cfg.lockedId = prefs.getUChar("lid", 1);
  cfg.bright   = constrain(prefs.getUChar("bri", 100), 5, 100);
  prefs.end();
  if (cfg.addr < 1 || cfg.addr > 509) cfg.addr = 1;
  if (cfg.id > 7) cfg.id = 0;
  if (cfg.screen >= SCR_COUNT) cfg.screen = SCR_ADDR;
  if (cfg.program >= NUM_PROGRAMS) cfg.program = 0;
  if (cfg.rfch > 126) cfg.rfch = 0;
  if (cfg.lockedId < 1 || cfg.lockedId > 7) cfg.lockedId = 1;
}

void saveSettings() {
  prefs.begin("bulb", false);
  prefs.putUShort("addr", cfg.addr);
  prefs.putUChar("id", cfg.id);
  prefs.putUChar("screen", cfg.screen);
  prefs.putUChar("prog", cfg.program);
  prefs.putUChar("rfch", cfg.rfch);
  prefs.putUChar("lid", cfg.lockedId);
  prefs.putUChar("bri", cfg.bright);
  prefs.end();
}

void markDirty() { dirty = true; dirtyAt = millis(); }

// ================================================================ radio
uint64_t wdmxAddress(uint8_t ch, uint8_t id) {
  return (uint64_t)ch | ((uint64_t)id << 8) | ((uint64_t)(uint8_t)~ch << 16) |
         ((uint64_t)(uint8_t)~id << 24) | ((uint64_t)(uint8_t)(ch + id) << 32);
}

void tune(uint8_t ch, uint8_t id) {
  scanCh = ch; scanId = id;
  radio.stopListening();
  radio.flush_rx();
  radio.openReadingPipe(0, wdmxAddress(ch, id));
  radio.setChannel(ch);
  radio.startListening();
  dwellStart = millis();
}

void startScan() {
  locked = false;
  firstTry = true;
  tune(cfg.rfch, cfg.id ? cfg.id : cfg.lockedId);
}

void advanceScan() {
  uint8_t ch = scanCh, id = scanId;
  if (firstTry) {
    firstTry = false;
    ch = 0;
  } else if (++ch > 126) {
    ch = 0;
    if (cfg.id == 0) id = (id >= 7) ? 1 : id + 1;  // Auto: cycle groups per sweep
  }
  tune(ch, id);
}

// Drain RX FIFO; returns true if at least one valid DMX packet arrived.
bool drainRadio() {
  bool got = false;
  WdmxPkt p;
  while (radio.available()) {
    radio.read(&p, sizeof(p));
    if ((p.magic == WDMX_MAGIC_1 || p.magic == WDMX_MAGIC_2) && p.payloadID < PKT_COUNT) {
      memcpy(&dmx[p.payloadID * PKT_DATA], p.data, PKT_DATA);
      lastPkt = millis();
      if (lastPkt == 0) lastPkt = 1;
      got = true;
    }
  }
  return got;
}

void serviceRadio() {
  if (!radioOk) return;
  uint32_t now = millis();
  // IRQ (active low) avoids SPI traffic when idle; slow fallback poll for safety.
  if (digitalRead(PIN_NRF_IRQ) == LOW || now - lastPoll >= 20) {
    lastPoll = now;
    if (drainRadio() && !locked) {
      locked = true;
      if (cfg.rfch != scanCh || cfg.lockedId != scanId) {
        cfg.rfch = scanCh; cfg.lockedId = scanId;
        markDirty();
      }
    }
  }
  if (locked) {
    if (now - lastPkt > LOCK_LOST_MS) startScan();
  } else if (now - dwellStart >= SCAN_DWELL_MS) {
    advanceScan();
  }
}

// ================================================================ output
RGBW lerp(const RGBW& a, const RGBW& b, uint32_t t256) {
  auto mix = [&](uint8_t x, uint8_t y) -> uint8_t { return x + ((int)y - (int)x) * (int)t256 / 256; };
  return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.w, b.w)};
}

RGBW scaled(const RGBW& c, uint32_t s256) {  // s256: 0..256
  return {(uint8_t)(c.r * s256 >> 8), (uint8_t)(c.g * s256 >> 8),
          (uint8_t)(c.b * s256 >> 8), (uint8_t)(c.w * s256 >> 8)};
}

// Keep total estimated LED current within LED_MAX_MA (all pixels identical).
RGBW limitCurrent(RGBW c) {
  float ma = (c.r + c.g + c.b + c.w) * (MA_PER_CHANNEL / 255.0f) * NUM_PIXELS;
  if (ma > LED_MAX_MA) {
    uint32_t s = (uint32_t)(256.0f * LED_MAX_MA / ma);
    c = scaled(c, s);
  }
  return c;
}

constexpr uint8_t CANDLE_PROGRAM = NUM_PRESETS + 4;  // index of "Candle" in EFFECT_NAMES
const RGBW CANDLE_BASE = {255, 60, 0, 35};
constexpr float CANDLE_GAIN = 2.0f;  // overall candle brightness multiplier

// First-order temporal dither: v16 is an 8.8 fixed-point level (0..65280).
// Returns the 8-bit value to output now; the remainder carries to the next frame.
uint8_t dither(uint32_t& acc, float v16) {
  if (v16 > 65280.0f) v16 = 65280.0f;
  acc += (uint32_t)v16;
  uint8_t out = acc >> 8;
  acc &= 0xFF;
  return out;
}

// Give every pixel/channel a different starting dither phase so that, at any
// instant, some pixels round up and some down. The total light stays steady
// (no whole-strip flicker) even when a single LED is toggling between levels.
void initDitherPhases(uint32_t (*acc)[4], int n) {
  for (int i = 0; i < n; i++)
    for (int c = 0; c < 4; c++) acc[i][c] = (uint32_t)((i * 4 + c) * 159) & 0xFF;  // ~golden-ratio spread
}

// Smoldering embers: a dim, slowly drifting glow made of large sections
// (a few control points interpolated around the strip), plus occasional slow
// "swells" where a small group of 3-5 LEDs gently brightens and fades back.
void candleRender(uint32_t now) {
  constexpr int NODES = 8;   // control points around the strip -> section size
  constexpr int SWELLS = 6;  // max concurrent swells
  struct Swell { bool on; int start, len; uint32_t t0, dur; float peak; };
  static float nodeCur[NODES], nodeTgt[NODES];
  static uint32_t nodeNext[NODES];
  static Swell sw[SWELLS];
  static float swl[NUM_PIXELS], lvl[NUM_PIXELS];
  static uint32_t acc[NUM_PIXELS][4];  // dither error accumulators
  static bool init = false;
  if (!init) {
    for (int n = 0; n < NODES; n++) { nodeCur[n] = nodeTgt[n] = 0.12f; nodeNext[n] = 0; }
    for (int k = 0; k < SWELLS; k++) sw[k].on = false;
    initDitherPhases(acc, NUM_PIXELS);
    init = true;
  }
  // Slow smolder: each node wanders between near-dark and a modest glow.
  for (int n = 0; n < NODES; n++) {
    if (now >= nodeNext[n]) {
      float r = random(0, 1000) / 1000.0f;
      nodeTgt[n] = 0.02f + 0.28f * r * r;  // skewed toward dim
      nodeNext[n] = now + random(500, 2000);
    }
    nodeCur[n] += (nodeTgt[n] - nodeCur[n]) * 0.02f;
  }
  // Spawn a swell now and then in a free slot.
  if (random(0, 100) < 2) {
    for (int k = 0; k < SWELLS; k++) {
      if (sw[k].on) continue;
      sw[k] = {true, (int)random(0, NUM_PIXELS), (int)random(3, 6), now,
               (uint32_t)random(700, 1800), 0.4f + (random(0, 1000) / 1000.0f) * 0.5f};
      break;
    }
  }
  // Accumulate swells: smooth in time (sin^2 envelope) and across the group (sine window).
  for (int i = 0; i < NUM_PIXELS; i++) swl[i] = 0;
  for (int k = 0; k < SWELLS; k++) {
    if (!sw[k].on) continue;
    uint32_t age = now - sw[k].t0;
    if (age >= sw[k].dur) { sw[k].on = false; continue; }
    float e = sinf(PI * age / sw[k].dur);
    e = e * e * sw[k].peak;
    for (int j = 0; j < sw[k].len; j++) {
      int i = (sw[k].start + j) % NUM_PIXELS;
      swl[i] = max(swl[i], e * sinf(PI * (j + 0.5f) / sw[k].len));
    }
  }
  float sum = 0;
  for (int i = 0; i < NUM_PIXELS; i++) {
    float pos = (float)i * NODES / NUM_PIXELS;
    int n0 = (int)pos, n1 = (n0 + 1) % NODES;
    float f = pos - n0;
    float base = nodeCur[n0] * (1 - f) + nodeCur[n1] * f;
    lvl[i] = min(1.0f, base + swl[i]) * CANDLE_GAIN;
    sum += lvl[i];
  }
  float ma = (CANDLE_BASE.r + CANDLE_BASE.g + CANDLE_BASE.b + CANDLE_BASE.w) *
             (MA_PER_CHANNEL / 255.0f) * sum;
  float lim = ma > LED_MAX_MA ? LED_MAX_MA / ma : 1.0f;
  for (int i = 0; i < NUM_PIXELS; i++) {
    float b = min(1.0f, lvl[i] * lim * cfg.bright / 100.0f);
    // Dim embers are redder; white only comes in as it gets brighter.
    // Temporal dithering (error accumulation) keeps slow dim fades smooth
    // instead of stepping one 8-bit level at a time.
    strip.setPixelColor(i, strip.Color(dither(acc[i][0], CANDLE_BASE.r * 256.0f * b),
                                       dither(acc[i][1], CANDLE_BASE.g * 256.0f * b),
                                       dither(acc[i][2], CANDLE_BASE.b * 256.0f * b),
                                       dither(acc[i][3], CANDLE_BASE.w * 256.0f * b * b)));
  }
  strip.show();
}

struct RGBW16 { uint32_t r, g, b, w; };  // 8.8 fixed point (255 == 65280)

RGBW16 to16(const RGBW& c) { return {(uint32_t)c.r << 8, (uint32_t)c.g << 8, (uint32_t)c.b << 8, (uint32_t)c.w << 8}; }

RGBW16 lerp16(const RGBW& a, const RGBW& b, uint32_t pos, uint32_t per) {
  auto l = [&](uint8_t x, uint8_t y) -> uint32_t {
    return (uint32_t)(((int64_t)x << 8) + ((((int64_t)y - x) << 8) * pos) / per);
  };
  return {l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.w, b.w)};
}

RGBW16 programFrame16(uint32_t now) {
  uint8_t p = cfg.program;
  if (p < NUM_PRESETS) return to16(PRESETS[p].c);
  switch (p - NUM_PRESETS) {
    case 0: case 1: {  // fades
      uint32_t per = (p - NUM_PRESETS == 0) ? FADE_SLOW_MS : FADE_FAST_MS;
      uint32_t pos = now % (per * NUM_PRESETS);
      uint8_t seg = pos / per;
      return lerp16(PRESETS[seg].c, PRESETS[(seg + 1) % NUM_PRESETS].c, pos % per, per);
    }
    case 2: case 3: {  // steps
      uint32_t per = (p - NUM_PRESETS == 2) ? STEP_SLOW_MS : STEP_FAST_MS;
      return to16(PRESETS[(now / per) % NUM_PRESETS].c);
    }
    default:
      return to16(CANDLE_BASE);  // candle is rendered per-pixel in updateLeds()
  }
}

RGBW dmxFrame(uint32_t now) {
  if (lastPkt == 0) return {0, 0, 0, 0};
  uint32_t age = now - lastPkt;
  uint32_t s256 = 256;
  if (age > SIGNAL_HOLD_MS) {
    uint32_t over = age - SIGNAL_HOLD_MS;
    s256 = over >= SIGNAL_FADE_MS ? 0 : 256 - over * 256 / SIGNAL_FADE_MS;
  }
  const uint8_t* d = &dmx[cfg.addr - 1];
  return scaled({d[0], d[1], d[2], d[3]}, s256);
}

void updateLeds(uint32_t now) {
  static uint32_t lastFrame = 0, lastShown = 0xFFFFFFFF;
  // Color mode updates faster (~125fps) so dithering is above flicker perception.
  if (now - lastFrame < (cfg.screen == SCR_COLOR ? 8u : 16u)) return;
  lastFrame = now;
  if (cfg.screen == SCR_COLOR && cfg.program == CANDLE_PROGRAM) {
    candleRender(now);
    lastShown = 0xFFFFFFFE;  // force repaint when leaving candle mode
    return;
  }
  if (cfg.screen == SCR_COLOR) {
    // Color mode: 16-bit pipeline + temporal dither so dimmed fades stay smooth.
    static uint32_t acc[NUM_PIXELS][4];
    static bool accInit = false;
    if (!accInit) { initDitherPhases(acc, NUM_PIXELS); accInit = true; }
    RGBW16 f = programFrame16(now);
    f.r = f.r * cfg.bright / 100; f.g = f.g * cfg.bright / 100;
    f.b = f.b * cfg.bright / 100; f.w = f.w * cfg.bright / 100;
    float ma = (f.r + f.g + f.b + f.w) / 256.0f * (MA_PER_CHANNEL / 255.0f) * NUM_PIXELS;
    if (ma > LED_MAX_MA) {
      float k = LED_MAX_MA / ma;
      f.r *= k; f.g *= k; f.b *= k; f.w *= k;
    }
    bool exact = !(f.r & 0xFF) && !(f.g & 0xFF) && !(f.b & 0xFF) && !(f.w & 0xFF);
    uint32_t first = 0;
    for (int i = 0; i < NUM_PIXELS; i++) {
      uint32_t px = strip.Color(dither(acc[i][0], f.r), dither(acc[i][1], f.g), dither(acc[i][2], f.b), dither(acc[i][3], f.w));
      if (i == 0) first = px;
      strip.setPixelColor(i, px);
    }
    if (exact && first == lastShown) return;  // static, no dither needed: skip the ~3ms show()
    strip.show();
    lastShown = exact ? first : 0xFFFFFFFE;
    return;
  }
  RGBW c = limitCurrent(dmxFrame(now));
  uint32_t packed = strip.Color(c.r, c.g, c.b, c.w);
  if (packed != lastShown) {  // skip identical frames (show() blocks ~3ms)
    lastShown = packed;
    strip.fill(packed);
    strip.show();
  }
}

// ================================================================ display
void drawUI() {
  char buf[24];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  const char* title = cfg.screen == SCR_ADDR ? "DMX ADDRESS" : cfg.screen == SCR_CHAN ? "WIRELESS CHANNEL" : "COLOR MODE";
  oled.drawStr(0, 9, title);
  if (cfg.screen != SCR_COLOR) {
    const char* st = !radioOk ? "NO RADIO" : locked ? "LOCK" : "SCAN";
    oled.drawStr(128 - oled.getStrWidth(st), 9, st);
  }
  oled.drawHLine(0, 12, 128);

  if (cfg.screen == SCR_ADDR) {
    oled.setFont(u8g2_font_logisoso28_tn);
    snprintf(buf, sizeof(buf), "%03u", cfg.addr);
    oled.drawStr(0, 52, buf);
    oled.setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "R G B W = %u-%u", cfg.addr, cfg.addr + 3);
    oled.drawStr(0, 63, buf);
  } else if (cfg.screen == SCR_CHAN) {
    oled.setFont(u8g2_font_helvB18_tf);
    oled.drawStr(0, 40, ID_NAMES[cfg.id]);
    oled.setFont(u8g2_font_6x10_tf);
    if (locked) snprintf(buf, sizeof(buf), "RF ch %u  %s", scanCh, ID_NAMES[scanId]);
    else        snprintf(buf, sizeof(buf), "Scanning...");
    oled.drawStr(0, 63, buf);
  } else {
    oled.setFont(u8g2_font_helvB14_tf);
    oled.drawStr(0, 40, programName(cfg.program));
    oled.setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "%u / %u", cfg.program + 1, NUM_PROGRAMS);
    oled.drawStr(0, 63, buf);
    snprintf(buf, sizeof(buf), "Bright %u%%", cfg.bright);
    oled.drawStr(128 - oled.getStrWidth(buf), 63, buf);
  }
}

// Send the buffer one 128-byte page per call so I2C never starves the radio.
void serviceDisplay(uint32_t now, bool force) {
  static uint64_t lastKey = ~0ULL;
  static int8_t page = -1;
  if (!displayOn) return;
  uint64_t key = cfg.screen | ((uint64_t)cfg.addr << 4) | ((uint64_t)cfg.id << 14) | ((uint64_t)cfg.program << 17) |
                 ((uint64_t)locked << 23) | ((uint64_t)radioOk << 24) |
                 ((uint64_t)(locked ? scanCh : 0) << 25) |  // don't redraw on every scan step
                 ((uint64_t)cfg.bright << 33);
  if (force || (key != lastKey && page < 0)) {
    lastKey = key;
    drawUI();
    page = 0;
  }
  if (page >= 0) {
    oled.updateDisplayArea(0, page, 16, 1);
    if (++page >= 8) page = -1;
  }
}

void setDisplay(bool on) {
  displayOn = on;
  oled.setPowerSave(on ? 0 : 1);
}

// ================================================================ buttons
struct Button {
  uint8_t pin;
  bool down;
  uint32_t downAt, lastRepeat, lastEdge;
  bool longFired;
  explicit Button(uint8_t p) : pin(p), down(false), downAt(0), lastRepeat(0), lastEdge(0), longFired(false) {}
};
Button btnUp{PIN_BTN_UP}, btnDown{PIN_BTN_DOWN}, btnMode{PIN_BTN_MODE};

// Returns number of "steps" this poll: 1 on press, then repeats while held
// (faster after 1.5s hold).
int pollButton(Button& b, uint32_t now, bool repeat) {
  bool pressed = digitalRead(b.pin) == LOW;
  if (pressed != b.down && now - b.lastEdge > 20) {  // debounce
    b.down = pressed;
    b.lastEdge = now;
    if (pressed) { b.downAt = b.lastRepeat = now; return 1; }
  }
  if (b.down && repeat && now - b.downAt > 500) {
    uint32_t interval = (now - b.downAt > 1500) ? 40 : 120;
    if (now - b.lastRepeat >= interval) { b.lastRepeat = now; return 1; }
  }
  return 0;
}

// Color-screen up/down: a tap changes program (on release); holding past
// 500ms instead steps brightness, repeating (faster after 1.5s).
// Returns +1 for a tap, 0 otherwise; brightness steps are returned in *bri.
int pollTapOrHold(Button& b, uint32_t now, int* bri) {
  *bri = 0;
  bool pressed = digitalRead(b.pin) == LOW;
  if (pressed != b.down && now - b.lastEdge > 20) {  // debounce
    b.down = pressed;
    b.lastEdge = now;
    if (pressed) { b.downAt = b.lastRepeat = now; b.longFired = false; return 0; }
    if (!b.longFired) return 1;  // released without long press = tap
    return 0;
  }
  if (b.down && now - b.downAt > 500) {
    uint32_t interval = (now - b.downAt > 1500) ? 40 : 120;
    if (!b.longFired || now - b.lastRepeat >= interval) {
      b.longFired = true;
      b.lastRepeat = now;
      *bri = 1;
    }
  }
  return 0;
}

void changeValue(int dir) {
  switch (cfg.screen) {
    case SCR_ADDR:
      cfg.addr = (cfg.addr - 1 + dir + 509) % 509 + 1;
      break;
    case SCR_CHAN:
      cfg.id = (cfg.id + dir + 8) % 8;
      memset(dmx, 0, sizeof(dmx));  // stale data from another group is meaningless
      lastPkt = 0;
      startScan();
      break;
    case SCR_COLOR:
      cfg.program = (cfg.program + dir + NUM_PROGRAMS) % NUM_PROGRAMS;
      break;
  }
  markDirty();
}

void serviceButtons(uint32_t now) {
  int up, dn, upBri = 0, dnBri = 0;
  bool colorScr = cfg.screen == SCR_COLOR;
  if (colorScr) {
    up = pollTapOrHold(btnUp, now, &upBri);
    dn = pollTapOrHold(btnDown, now, &dnBri);
  } else {
    up = pollButton(btnUp, now, true);
    dn = pollButton(btnDown, now, true);
  }
  int md = pollButton(btnMode, now, false);
  if (!(up || dn || md || upBri || dnBri)) return;
  lastActivity = now;
  if (!displayOn) {  // first press only wakes the screen
    setDisplay(true);
    serviceDisplay(now, true);
    return;
  }
  if (upBri || dnBri) {
    int v = (int)cfg.bright + (upBri - dnBri) * 5;
    cfg.bright = constrain(v, 5, 100);
    markDirty();
    return;
  }
  if (md) { cfg.screen = (cfg.screen + 1) % SCR_COUNT; markDirty(); }
  if (up) changeValue(+1);
  if (dn) changeValue(-1);
}

// ================================================================ setup / loop
void setup() {
  setCpuFrequencyMhz(CPU_MHZ);  // WiFi/BT are never initialised -> radios stay off
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_NRF_IRQ, INPUT);

  loadSettings();

  strip.begin();
  strip.show();

  oled.setBusClock(400000);
  oled.begin();
  oled.setContrast(OLED_CONTRAST);

  radioOk = radio.begin();
  if (radioOk) {
    radio.setDataRate(RF24_250KBPS);
    radio.setCRCLength(RF24_CRC_16);
    radio.setAutoAck(false);
    radio.disableDynamicPayloads();
    radio.setPayloadSize(sizeof(WdmxPkt));
    radio.maskIRQ(1, 1, 0);  // IRQ only on RX data ready
    startScan();
  }

  lastActivity = millis();
  serviceDisplay(millis(), true);
}

void loop() {
  uint32_t now = millis();
  serviceRadio();
  serviceButtons(now);
  updateLeds(now);
  serviceRadio();  // LED show() blocks ~3ms; drain the FIFO right after
  serviceDisplay(now, false);

  if (displayOn && now - lastActivity > OLED_TIMEOUT_MS) setDisplay(false);
  if (dirty && now - dirtyAt > SAVE_DELAY_MS) { dirty = false; saveSettings(); }
  delay(1);
}
