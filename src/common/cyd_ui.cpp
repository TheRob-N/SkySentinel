#ifdef SS_CYD
#include "cyd_ui.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#include "cyd_art.h"
#include "cyd_sd.h"
#include "ss_version.h"
#include "ss_gps.h"
#include "ss_follow.h"
#include "ss_touch.h"

// Both boards (ESP32-2432S028 and LCDWiki ESP32-32E) wire the screen, touch and SD
// card the same way. They differ in the RGB LED's red pin, GPIO 34 (light sensor vs
// battery) and the speaker amp's enable pin.
#define TFT_SCK 14
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_CS 15
#define TFT_DC 2
#define TFT_BL 21
#define T_CLK 25
#define T_MOSI 32
#define T_MISO 39
#define T_CS 33
#define T_IRQ 36
#define LED_G 16
#define LED_B 17
#define SPEAKER 26
#define BOOT_BTN 0
#define BATT_ADC 34
#define AMP_EN_32E 4      // ESP32-32E: speaker amp enable, low = on
#define LED_R_2432 4
#define LED_R_32E 22

namespace ss {
namespace cyd {

static constexpr int16_t W = 320, H = 240;
static const TouchTargets kTargets{24, 296, 24, 216};

enum Board : uint8_t { BOARD_UNSET = 0, BOARD_2432 = 1, BOARD_32E = 2 };
enum Chip : uint8_t { CHIP_AUTO = 0, CHIP_ILI9341 = 1, CHIP_ST7789 = 2 };

enum TouchBus : uint8_t { TB_UNKNOWN = 0, TB_OWN = 1 /* CYD: own pins 25/32/39 */, TB_SHARED = 2 /* on the display's SPI */ };

struct Settings {
  uint8_t magic = 0x5F;
  uint8_t board = BOARD_UNSET;
  uint8_t chip = CHIP_AUTO;   // forced chip, or auto-detect
  int8_t invert = -1;         // -1 = the chip's usual setting
  uint8_t swap_rb = 0;
  uint8_t rot180 = 0;
  uint8_t beep = 1;
  uint8_t done = 0;           // first-run setup finished
  uint8_t cal_rot = 0;        // rot180 when the touch was calibrated
  TouchCal cal;
  uint8_t touch_bus = TB_UNKNOWN;
  uint8_t night = 0;          // red-on-black, dimmed backlight
  uint8_t bright_day = 0;     // backlight %, 0 = default (100)
  uint8_t bright_night = 0;   // backlight %, 0 = default (25)
  uint8_t clear_s = 0;        // alert banner clears after: 0 = default (60 s), 30/60, 5 = 5 min, 255 = never
};

static Preferences* prefs_ = nullptr;
static Settings set_;
static uint8_t chip_ = CHIP_ST7789;   // what's actually driving the screen
static uint32_t chipId_ = 0;
static SPIClass tftSpi(HSPI);
static Adafruit_SPITFT* tft = nullptr;

// ---------------------------------------------------------------------------
// Colors (RGB565), rebuilt when "swap red/blue" changes

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (set_.swap_rb) { const uint8_t t = r; r = b; b = t; }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint16_t BG, PANEL, LINE, TEXT, MUTED, DIM, ACCENT, WARN, CRIT, OK, WHITE;
struct Rgb { uint8_t r, g, b; };
static Rgb bgRgb_, panelRgb_, accentRgb_, textRgb_;   // for blending artwork onto them
static uint16_t catColor[CAT_COUNT];
static void buildColors() {
  if (set_.night) {
    // Night: everything red on black (keeps night vision), like the web page's NIGHT theme.
    BG = rgb(0, 0, 0); PANEL = rgb(28, 4, 4); LINE = rgb(80, 14, 12); TEXT = rgb(255, 80, 60);
    MUTED = rgb(170, 45, 35); DIM = rgb(90, 20, 16); ACCENT = rgb(255, 60, 40); WARN = rgb(255, 120, 40);
    CRIT = rgb(255, 30, 20); OK = rgb(220, 70, 50); WHITE = rgb(255, 150, 130);
    bgRgb_ = {0, 0, 0}; panelRgb_ = {28, 4, 4}; accentRgb_ = {255, 60, 40}; textRgb_ = {255, 80, 60};
    for (uint8_t i = 0; i < CAT_COUNT; ++i)   // surveillance a little brighter than everyday
      catColor[i] = (kSurveillanceMask & catBit(i)) ? rgb(255, 70, 50) : rgb(170, 45, 35);
    return;
  }
  BG = rgb(6, 16, 28); PANEL = rgb(16, 32, 50); LINE = rgb(40, 64, 90); TEXT = rgb(220, 233, 243);
  MUTED = rgb(138, 164, 186); DIM = rgb(70, 90, 110); ACCENT = rgb(72, 201, 232); WARN = rgb(242, 169, 59);
  CRIT = rgb(255, 90, 80); OK = rgb(61, 220, 151); WHITE = rgb(255, 255, 255);
  bgRgb_ = {6, 16, 28}; panelRgb_ = {16, 32, 50}; accentRgb_ = {72, 201, 232}; textRgb_ = {220, 233, 243};
  static const uint8_t c[CAT_COUNT][3] = {
    {128, 128, 128}, {255, 107, 94}, {255, 149, 64}, {242, 169, 59}, {199, 125, 255}, {72, 201, 232},
    {255, 214, 10}, {61, 220, 151}, {138, 164, 186}, {150, 180, 255}, {120, 200, 200}, {120, 140, 160},
    {120, 140, 160}, {180, 220, 120},
  };
  for (uint8_t i = 0; i < CAT_COUNT; ++i) catColor[i] = rgb(c[i][0], c[i][1], c[i][2]);
}
static const char* const kCatShort[CAT_COUNT] = {
  "", "ALPR", "BODY CAM", "TRACKER", "GLASSES", "DRONE", "SKIMMER", "PENTEST",
  "PHONES", "AUDIO", "WEARABLE", "OTHER BT", "WI-FI", "LORA",
};
static const uint8_t kTileCats[8] = {CAT_FLOCK, CAT_BODYCAM, CAT_TRACKER, CAT_GLASSES,
                                     CAT_DRONE, CAT_SKIMMER, CAT_PENTEST, CAT_PHONE};

// ---------------------------------------------------------------------------
// Settings

static void saveSettings() {
  if (prefs_) prefs_->putBytes("cyd", &set_, sizeof(set_));
}
static void loadSettings() {
  Settings s;
  if (!prefs_ || !prefs_->isKey("cyd")) return;
  // Newer firmware adds fields at the end: an older, shorter record still loads.
  const size_t len = prefs_->getBytesLength("cyd");
  if (len && len <= sizeof(s) && prefs_->getBytes("cyd", &s, len) == len && s.magic == 0x5F) set_ = s;
}

// ---------------------------------------------------------------------------
// Display: find out which chip, then drive it through Adafruit's common base

static uint32_t rawRead(uint8_t cmd) {
  tftSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);
  tftSpi.transfer(cmd);
  digitalWrite(TFT_DC, HIGH);
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | tftSpi.transfer(0);
  digitalWrite(TFT_CS, HIGH);
  tftSpi.endTransaction();
  return v;
}

static uint8_t detectChip() {
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  tftSpi.begin(TFT_SCK, TFT_MISO, TFT_MOSI, -1);
  // Software reset first: the panel's reset line is tied to the ESP32's.
  tftSpi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);
  tftSpi.transfer(0x01);
  digitalWrite(TFT_CS, HIGH);
  tftSpi.endTransaction();
  delay(150);
  // ILI9341 answers "Read ID4" (D3h) with 00 93 41 after a dummy byte.
  const uint32_t d3 = rawRead(0xD3);
  chipId_ = d3;
  if ((d3 & 0xFFFF) == 0x9341) return CHIP_ILI9341;
  // ST7789: RDDID (04h) gives 85 85 52 (or 85 xx xx) after one dummy bit.
  chipId_ = (rawRead(0x04) >> 7) & 0xFFFFFF;
  if ((chipId_ >> 16) == 0x85) return CHIP_ST7789;
  // No answer: many of these panels don't drive their data-out line. The setup asks.
  return CHIP_AUTO;
}

static bool chipGuessed_ = false;  // detection got no answer; the first setup screen asks

static bool invertOn() { return set_.invert >= 0 ? set_.invert != 0 : chip_ == CHIP_ST7789; }

static void initPanel() {
  if (chip_ == CHIP_ILI9341) {
    Adafruit_ILI9341* t = new Adafruit_ILI9341(&tftSpi, TFT_DC, TFT_CS, -1);
    t->begin(40000000);
    tft = t;
  } else {
    Adafruit_ST7789* t = new Adafruit_ST7789(&tftSpi, TFT_CS, TFT_DC, -1);
    t->init(240, 320);
    t->setSPISpeed(40000000);
    tft = t;
  }
  tft->setRotation(set_.rot180 ? 3 : 1);
  tft->invertDisplay(invertOn());
  tft->setTextWrap(false);
  tft->cp437(true);
}

static bool blPwm_ = false;
static void backlight(bool on) {
  if (!blPwm_) blPwm_ = ledcAttach(TFT_BL, 5000, 8);
  const uint8_t pct = set_.night ? (set_.bright_night ? set_.bright_night : 25) : (set_.bright_day ? set_.bright_day : 100);
  const uint8_t level = !on ? 0 : (uint8_t)(pct * 255 / 100);
  if (blPwm_) ledcWrite(TFT_BL, level);
  else { pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, on ? HIGH : LOW); }
}

static void startDisplay() {
  if (set_.chip == CHIP_AUTO) {
    chip_ = detectChip();
    if (chip_ == CHIP_AUTO) { chip_ = CHIP_ILI9341; chipGuessed_ = true; }  // the original CYD's chip
  } else {
    chip_ = set_.chip;
    tftSpi.begin(TFT_SCK, TFT_MISO, TFT_MOSI, -1);
  }
  initPanel();
  backlight(true);
}

// Try the other chip without restarting (setup's "press BOOT" step). The old driver
// object is left alone: it's a few hundred bytes, once.
static void switchChip() {
  chip_ = chip_ == CHIP_ILI9341 ? CHIP_ST7789 : CHIP_ILI9341;
  set_.invert = -1;
  initPanel();
  Serial.printf("[cyd] trying display chip %s\n", chip_ == CHIP_ILI9341 ? "ILI9341" : "ST7789");
}

// ---------------------------------------------------------------------------
// Drawing helpers

// The built-in font is plain ASCII. Names from the air can be UTF-8 (or cut in the
// middle of a character): one '?' per character it can't draw, stray bytes dropped.
// Writes forward only, so it's safe on its own buffer.
static const char* plain(const char* s) {
  static char buf[96];
  size_t o = 0;
  for (const uint8_t* p = (const uint8_t*)s; *p && o < sizeof(buf) - 1;) {
    const uint8_t c = *p;
    if (c < 0x80) { buf[o++] = (c >= 0x20 && c != 0x7F) ? (char)c : ' '; ++p; continue; }
    if (c >= 0xC0 && c <= 0xF7) buf[o++] = '?';   // start of a multi-byte character
    ++p;
    while ((*p & 0xC0) == 0x80) ++p;               // its continuation bytes (or strays)
  }
  buf[o] = '\0';
  return buf;
}

static void text(int16_t x, int16_t y, uint8_t size, uint16_t fg, uint16_t bg, const char* s) {
  tft->setTextSize(size);
  tft->setTextColor(fg, bg);
  tft->setCursor(x, y);
  tft->print(plain(s));
}
static void textRight(int16_t xr, int16_t y, uint8_t size, uint16_t fg, uint16_t bg, const char* s) {
  s = plain(s);
  text(xr - (int16_t)strlen(s) * 6 * size, y, size, fg, bg, s);
}
static void textCenter(int16_t cx, int16_t y, uint8_t size, uint16_t fg, uint16_t bg, const char* s) {
  s = plain(s);
  text(cx - (int16_t)strlen(s) * 3 * size, y, size, fg, bg, s);
}
// Copy at most n characters (keeps the screen tidy)
static const char* fit(const char* s, size_t n) {
  static char buf[64];
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  size_t l = strlen(s);
  if (l <= n) return s;
  memcpy(buf, s, n);
  buf[n] = '\0';
  if (n > 2) buf[n - 1] = '.';
  return buf;
}


// ---------------------------------------------------------------------------
// Artwork: the logo, text images and category icons (all in flash, drawn a row at a time)

// The logo is light text on black: lay it over `bg` by treating brightness as coverage.
// Night theme turns it red like everything else.
static void drawLogo(int16_t x, int16_t y, bool big, Rgb bg) {
  const uint16_t* src = big ? SKYLOGO_BIG : SKYLOGO_SMALL;
  const int16_t w = big ? SKYLOGO_BIG_W : SKYLOGO_SMALL_W, h = big ? SKYLOGO_BIG_H : SKYLOGO_SMALL_H;
  uint16_t line[SKYLOGO_BIG_W];
  for (int16_t yy = 0; yy < h; ++yy) {
    for (int16_t xx = 0; xx < w; ++xx) {
      const uint16_t v = src[yy * w + xx];
      uint8_t r = (uint8_t)(((v >> 11) & 31) * 255 / 31), g = (uint8_t)(((v >> 5) & 63) * 255 / 63), b = (uint8_t)((v & 31) * 255 / 31);
      if (set_.night) { const uint8_t l = r > g ? (r > b ? r : b) : (g > b ? g : b); r = l; g = (uint8_t)(l * 3 / 10); b = (uint8_t)(l / 5); }
      const uint8_t a = r > g ? (r > b ? r : b) : (g > b ? g : b);   // coverage
      const uint16_t k = 255 - a;
      line[xx] = rgb((uint8_t)(r + bg.r * k / 255), (uint8_t)(g + bg.g * k / 255), (uint8_t)(b + bg.b * k / 255));
    }
    tft->drawRGBBitmap(x, y + yy, line, w, 1);
  }
}

// 8-bit alpha text image in one colour over `bg`
static void drawAlphaImage(int16_t x, int16_t y, const uint8_t* src, int16_t w, int16_t h, Rgb fg, Rgb bg) {
  uint16_t line[200];
  if (w > 200) return;
  for (int16_t yy = 0; yy < h; ++yy) {
    for (int16_t xx = 0; xx < w; ++xx) {
      const uint16_t a = src[yy * w + xx], k = 255 - a;
      line[xx] = rgb((uint8_t)((fg.r * a + bg.r * k) / 255), (uint8_t)((fg.g * a + bg.g * k) / 255), (uint8_t)((fg.b * a + bg.b * k) / 255));
    }
    tft->drawRGBBitmap(x, y + yy, line, w, 1);
  }
}

// 12 x 12 category icons for the dashboard tiles (same order as kTileCats), one row per word
static const uint16_t kTileIcons[8][12] = {
  {0x000,0xFFC,0x807,0x985,0x987,0x804,0xFFC,0x0C0,0x0C0,0x0C0,0x3F0,0x000},  // FLOCK
  {0x1F8,0x204,0x4F2,0x50A,0x56A,0x56A,0x50A,0x4F2,0x402,0x59A,0x204,0x1F8},  // BODYCAM
  {0x010,0x008,0x1E4,0x212,0x40A,0x4CA,0x4CA,0x40A,0x212,0x1E4,0x008,0x010},  // TRACKER
  {0x000,0x000,0x000,0x000,0xFFF,0xF9F,0xF9F,0x79E,0x30C,0x000,0x000,0x000},  // GLASSES (sunglasses, front)
  {0x000,0xE07,0x402,0x606,0x108,0x0F0,0x090,0x0F0,0x108,0x606,0x402,0xE07},  // DRONE
  {0x000,0x000,0xFFF,0x801,0xFFF,0xFFF,0x801,0xB81,0x801,0xFFF,0x000,0x000},  // SKIMMER
  {0xFFF,0x801,0xFFF,0x801,0xA01,0x901,0x881,0x901,0xA79,0x801,0xFFF,0x000},  // PENTEST
  {0x3FC,0x204,0x2F4,0x204,0x204,0x204,0x204,0x204,0x204,0x204,0x264,0x3FC},  // PHONE
};
static void drawIcon(int16_t x, int16_t y, uint8_t tile, uint16_t col) {
  for (int16_t yy = 0; yy < 12; ++yy) {
    const uint16_t row = kTileIcons[tile][yy];
    for (int16_t xx = 0; xx < 12;) {   // runs of lit pixels as short lines
      if (!(row & (0x800 >> xx))) { ++xx; continue; }
      int16_t n = 1;
      while (xx + n < 12 && (row & (0x800 >> (xx + n)))) ++n;
      tft->drawFastHLine(x + xx, y + yy, n, col);
      xx += n;
    }
  }
}

// ---------------------------------------------------------------------------
// Boot screen: logo, what's starting, version. Stays up for a few seconds, or until the
// master answers, then the dashboard takes over.
static bool splash_ = false;
static uint32_t splashUntil_ = 0;
static uint8_t splashLine_ = 0;
static void drawSplash() {
  tft->fillScreen(BG);
  drawLogo((W - SKYLOGO_BIG_W) / 2, 30, true, bgRgb_);
  drawAlphaImage((W - TXT_SUBTITLE_W) / 2, 70, TXT_SUBTITLE, TXT_SUBTITLE_W, TXT_SUBTITLE_H, accentRgb_, bgRgb_);
  tft->drawFastHLine(60, 98, W - 120, LINE);
  textCenter(W / 2, 222, 1, DIM, BG, "SkySentinel " SS_RELEASE "  -  CYD node + dashboard");
  splash_ = true;
  splashLine_ = 0;
  splashUntil_ = millis() + 2500;
}

struct Button { int16_t x, y, w, h; };
static void drawButton(const Button& b, const char* label, uint16_t fg, uint16_t fill, uint16_t border) {
  tft->fillRoundRect(b.x, b.y, b.w, b.h, 6, fill);
  tft->drawRoundRect(b.x, b.y, b.w, b.h, 6, border);
  // Up to two lines, split on '\n'
  const char* nl = strchr(label, '\n');
  if (!nl) {
    textCenter(b.x + b.w / 2, b.y + b.h / 2 - 4, 1, fg, fill, label);
  } else {
    char a[40];
    const size_t l = (size_t)(nl - label) < sizeof(a) - 1 ? (size_t)(nl - label) : sizeof(a) - 1;
    memcpy(a, label, l);
    a[l] = '\0';
    textCenter(b.x + b.w / 2, b.y + b.h / 2 - 10, 1, fg, fill, a);
    textCenter(b.x + b.w / 2, b.y + b.h / 2 + 3, 1, MUTED, fill, nl + 1);
  }
}
static bool hit(const Button& b, int16_t x, int16_t y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// Redraw a part only when what it shows changed.
enum Part : uint8_t { P_HEADER, P_BANNER, P_TILE0, P_ROW0 = P_TILE0 + 8, P_LISTHEAD = P_ROW0 + kDashThreats, P_FOOTER, P_BODY,
                      P_TABS, P_TITLE, P_PAGER, P_LINE0, P_COUNT = P_LINE0 + 12 };
static uint32_t partHash_[P_COUNT];
static bool changed(Part p, const char* what, uint32_t extra = 0) {
  uint32_t h = 2166136261u ^ extra;
  for (const char* c = what; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
  if (!h) h = 1;
  if (partHash_[p] == h) return false;
  partHash_[p] = h;
  return true;
}
static void forgetParts() { memset(partHash_, 0, sizeof(partHash_)); }
static constexpr int16_t kTabY = 216;   // the tab bar along the bottom
static void drawTabs();

// ---------------------------------------------------------------------------
// Touch (XPT2046): its own pins on these boards (SPI on 25/32/39, CS 33), or the display bus



struct TouchRead { int z; uint16_t xs[3], ys[3]; };
// The XPT2046 read sequence of PaulStoffregen's XPT2046_Touchscreen (what SquachWatch
// uses on these boards): pipelined 16-bit transfers at 2 MHz, first position sample
// thrown away, power down at the end so the pen interrupt works again.
// The original CYD's own touch pins (25/32/39/33): bit-banged, because the ESP32's
// second SPI bus (VSPI) is the SD card's. Same sequence as xptRead below, ~250 kHz.
static uint16_t bbXfer(uint16_t out, int bits) {
  uint16_t in = 0;
  for (int i = bits - 1; i >= 0; --i) {
    digitalWrite(T_MOSI, (out >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(1);
    in = (uint16_t)((in << 1) | (digitalRead(T_MISO) ? 1 : 0));
    digitalWrite(T_CLK, LOW);
  }
  return in;
}
static void xptReadBitbang(TouchRead& r) {
  digitalWrite(T_CLK, LOW);
  digitalWrite(T_CS, LOW);
  bbXfer(0xB1, 8);
  const int z1 = bbXfer(0xC1, 16) >> 3;
  const int z2 = bbXfer(0x91, 16) >> 3;
  r.z = z1 + 4095 - z2;
  bbXfer(0x91, 16);
  r.ys[0] = bbXfer(0xD1, 16) >> 3;
  r.xs[0] = bbXfer(0x91, 16) >> 3;
  r.ys[1] = bbXfer(0xD1, 16) >> 3;
  r.xs[1] = bbXfer(0x91, 16) >> 3;
  r.ys[2] = bbXfer(0xD0, 16) >> 3;
  r.xs[2] = bbXfer(0, 16) >> 3;
  digitalWrite(T_CS, HIGH);
}
static void xptRead(SPIClass& spi, TouchRead& r) {
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(T_CS, LOW);
  spi.transfer(0xB1);
  const int z1 = spi.transfer16(0xC1) >> 3;
  const int z2 = spi.transfer16(0x91) >> 3;
  r.z = z1 + 4095 - z2;
  spi.transfer16(0x91);                       // noisy first sample
  r.ys[0] = spi.transfer16(0xD1) >> 3;        // answer to 0x91
  r.xs[0] = spi.transfer16(0x91) >> 3;        // answer to 0xD1
  r.ys[1] = spi.transfer16(0xD1) >> 3;
  r.xs[1] = spi.transfer16(0x91) >> 3;
  r.ys[2] = spi.transfer16(0xD0) >> 3;        // and power down after
  r.xs[2] = spi.transfer16(0) >> 3;
  digitalWrite(T_CS, HIGH);
  spi.endTransaction();
}
static void readTouch(uint8_t bus, TouchRead& r) {
  if (bus == TB_SHARED) xptRead(tftSpi, r);
  else xptReadBitbang(r);
}
// A line that isn't connected reads a constant: all 0s, all 1s, or the same value for
// x and y. A real press gives different x and y somewhere inside the range.
static bool plausible(const TouchRead& r) {
  const uint16_t x = r.xs[1], y = r.ys[1];
  if (x == y) return false;
  return x > 20 && x < 4080 && y > 20 && y < 4080;
}

static uint32_t lastTouchLog_ = 0;
// Raw reading; false when nobody's touching.
// ESP32-32E boards put the XPT2046 on the display's SPI bus (CS 33); the original CYD
// gives it pins of its own. Try the display bus first: on the original CYD it reads
// nothing (that panel doesn't drive its data-out line), so it can't be mistaken.
static bool touchRaw(int16_t& rx, int16_t& ry) {
  TouchRead r;
  if (set_.touch_bus == TB_UNKNOWN) {
    if (digitalRead(T_IRQ) == HIGH) return false;
    TouchRead own;
    readTouch(TB_SHARED, r);
    readTouch(TB_OWN, own);
    if (millis() - lastTouchLog_ > 300) {
      lastTouchLog_ = millis();
      Serial.printf("[cyd] touch: display bus x=%u y=%u z=%d | own pins x=%u y=%u z=%d\n", r.xs[1], r.ys[1], r.z,
                    own.xs[1], own.ys[1], own.z);
    }
    if (plausible(r)) {
      set_.touch_bus = TB_SHARED;
    } else if (plausible(own)) {
      set_.touch_bus = TB_OWN;
      r = own;
    } else {
      return false;
    }
    saveSettings();
    Serial.printf("[cyd] touch is on %s\n", set_.touch_bus == TB_SHARED ? "the display's SPI bus" : "its own pins");
  } else {
    // The pen line is wired on both boards; on the display bus trust pressure alone too.
    if (set_.touch_bus == TB_OWN && digitalRead(T_IRQ) == HIGH) return false;
    readTouch(set_.touch_bus, r);
  }
  if (r.z < 400) return false;
  if (!plausible(r)) {
    // Pressed hard enough but nonsense coordinates, again and again: wrong wiring saved. Look again.
    static uint16_t bad = 0;
    if (++bad > 60) { bad = 0; set_.touch_bus = TB_UNKNOWN; Serial.println("[cyd] touch: readings make no sense, detecting again"); }
    return false;
  }
  uint16_t* xs = r.xs;
  uint16_t* ys = r.ys;
  auto med = [](uint16_t* a) -> int16_t {
    if (a[0] > a[1]) { uint16_t t = a[0]; a[0] = a[1]; a[1] = t; }
    if (a[1] > a[2]) { uint16_t t = a[1]; a[1] = a[2]; a[2] = t; }
    if (a[0] > a[1]) { uint16_t t = a[0]; a[0] = a[1]; a[1] = t; }
    return (int16_t)a[1];
  };
  rx = med(xs);
  ry = med(ys);
  return true;
}

static bool touchScreen(int16_t& x, int16_t& y) {
  int16_t rx, ry;
  if (!touchRaw(rx, ry) || !set_.cal.valid()) return false;
  touchMap(set_.cal, kTargets, rx, ry, W, H, x, y);
  if (set_.rot180 != set_.cal_rot) { x = W - 1 - x; y = H - 1 - y; }
  return true;
}

// ---------------------------------------------------------------------------
// LED and beeper

static uint8_t ledR() { return set_.board == BOARD_32E ? LED_R_32E : LED_R_2432; }
static void led(bool r, bool g, bool b) {  // common anode: low = lit
  digitalWrite(ledR(), r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}
static uint32_t ledUntil_ = 0;
static uint8_t ledLevel_ = 0;
static uint32_t beepAt_[4] = {0};  // start times of the beeps still to play
static bool beeping_ = false;
static bool beeperReady_ = false;

static uint32_t ampUntil_ = 0;
// Three short beeps. The speaker amp (ESP32-32E) is switched on 100 ms early: it needs a
// moment to wake up, and a beep sent before that is lost.
static void playBeeps(uint32_t now) {
  beepAt_[0] = now + 100;
  beepAt_[1] = now + 320;
  beepAt_[2] = now + 540;
  ampUntil_ = now + 900;
}
static void alertSignal(uint8_t level, uint32_t now) {
  ledLevel_ = level;
  ledUntil_ = now + (level >= 2 ? 4000 : 1500);
  if (level >= 2 && set_.beep) playBeeps(now);
}

static void runSignals(uint32_t now) {
  // LED: red blink for alerts, amber blip for notices
  if (ledLevel_ && (int32_t)(ledUntil_ - now) > 0) {
    const bool on = (now / 200) % 2 == 0;
    if (ledLevel_ >= 2) led(on, false, false); else led(on, on, false);
  } else if (ledLevel_) {
    ledLevel_ = 0;
    led(false, false, false);
  }
  // Beeps: 2.4 kHz, 120 ms each
  bool want = false;
  for (uint32_t& t : beepAt_) {
    if (!t) continue;
    const int32_t d = (int32_t)(now - t);
    if (d >= 0 && d < 120) want = true;
    else if (d >= 120) t = 0;
  }
  // Amp (ESP32-32E, low = on) stays on for the whole sequence
  static bool ampOn = false;
  const bool amp = ampUntil_ && (int32_t)(ampUntil_ - now) > 0;
  if (amp != ampOn) {
    ampOn = amp;
    if (set_.board == BOARD_32E) digitalWrite(AMP_EN_32E, amp ? LOW : HIGH);
    if (!amp) ampUntil_ = 0;
  }
  if (want != beeping_) {
    beeping_ = want;
    if (!beeperReady_) beeperReady_ = ledcAttach(SPEAKER, 2400, 8);
    if (beeperReady_) ledcWriteTone(SPEAKER, want ? 2400 : 0);
  }
}

// ---------------------------------------------------------------------------
// State

enum Screen : uint8_t { S_CAL, S_BOARD, S_DISPLAY, S_DASH, S_SETTINGS, S_CHIP, S_DEVICES, S_DETAIL, S_EVENTS, S_NODES, S_SD, S_ALERTS };
static Screen screen_ = S_DASH;
static bool fullSetup_ = false;      // walking through all setup steps
static bool redraw_ = true;
static uint8_t calStep_ = 0;
static int16_t calRaw_[3][2];
static bool calFailed_ = false;

static Dash dash_;
static bool haveDash_ = false;
static uint32_t dashMs_ = 0;
static uint16_t ackedSeq_ = 0;       // alert the user tapped away
static uint16_t seenSeq_ = 0;
static bool firstDash_ = true;
static uint16_t pendingCats_ = 0;    // tile taps not yet confirmed by the master (xor mask)
static uint32_t pendingCatsMs_ = 0;

static DashCommand cmdQ_[4];
static uint8_t cmdN_ = 0;
static void queueCmd(uint8_t cmd, uint8_t arg, uint16_t offset = 0, uint32_t id = 0) {
  if (cmd == DC_VIEW)  // only the latest view request matters
    for (uint8_t i = 0; i < cmdN_; ++i)
      if (cmdQ_[i].cmd == DC_VIEW) { cmdQ_[i] = DashCommand{cmd, arg, offset, id}; return; }
  if (cmdN_ < 4) cmdQ_[cmdN_++] = DashCommand{cmd, arg, offset, id};
}

// The extra screens: what we asked the master for, and what came back.
static uint8_t viewWant_ = DV_NONE;
static uint16_t viewOffset_ = 0;
static uint32_t viewId_ = 0;
static uint32_t viewAskedMs_ = 0;
static uint32_t viewMs_ = 0;          // when matching data last arrived
static DashDevices devs_;
static DashDetail detail_;
static DashEvents events_;
static DashNodes nodes_;
static Screen lastTab_ = S_DASH;      // where BACK from settings goes
static void askView(uint8_t view, uint16_t offset, uint32_t id, uint32_t now) {
  if (view != viewWant_ || offset != viewOffset_ || id != viewId_) viewMs_ = 0;  // old data no longer fits
  viewWant_ = view;
  viewOffset_ = offset;
  viewId_ = id;
  viewAskedMs_ = now;
  queueCmd(DC_VIEW, view, offset, id);
}

static uint32_t lastActivity_ = 0;   // last tap or screen change: an idle screen goes home
static void go(Screen s) {
  screen_ = s;
  redraw_ = true;
  lastActivity_ = millis();
  const uint8_t view = s == S_DEVICES ? DV_DEVICES : s == S_DETAIL ? DV_DETAIL : s == S_EVENTS ? DV_EVENTS
                       : s == S_NODES ? DV_NODES : DV_NONE;
  if (s == S_DASH || s == S_DEVICES || s == S_DETAIL || s == S_EVENTS || s == S_NODES) lastTab_ = s == S_DETAIL ? S_DEVICES : s;
  if (view != viewWant_ || view == DV_DETAIL) {
    const uint16_t off = view == viewWant_ ? viewOffset_ : 0;
    askView(view, view == DV_DETAIL ? 0 : off, view == DV_DETAIL ? viewId_ : 0, millis());
  }
}

static bool pressed_ = false;
static uint32_t lastDownMs_ = 0;
static uint32_t calQuietUntil_ = 0;  // ignore touches until then (between crosses)

static void startCalibration() {
  calStep_ = 0;
  calFailed_ = false;
  go(S_CAL);
  pressed_ = false;
  calQuietUntil_ = millis() + 800;  // the tap that got us here isn't cross 1
}

// ---------------------------------------------------------------------------
// Setup screens

// First of all, when the display chip couldn't be read: is the picture right?
static void drawChip() {
  tft->fillScreen(BG);
  textCenter(W / 2, 40, 3, ACCENT, BG, "SKYSENTINEL");
  textCenter(W / 2, 92, 2, TEXT, BG, "Can you read this?");
  textCenter(W / 2, 128, 1, OK, BG, "Yes, dark background: tap the screen.");
  textCenter(W / 2, 150, 1, WARN, BG, "Mirrored, light, garbled or blank:");
  textCenter(W / 2, 164, 1, WARN, BG, "press BOOT once to try the other display.");
  char b[40];
  snprintf(b, sizeof(b), "now trying: %s", chip_ == CHIP_ILI9341 ? "ILI9341" : "ST7789");
  textCenter(W / 2, 200, 1, MUTED, BG, b);
}

static const Button kBoard2432{12, 70, 296, 60};
static const Button kBoard32E{12, 142, 296, 60};

static void drawCal() {
  tft->fillScreen(BG);
  textCenter(W / 2, 92, 2, ACCENT, BG, "TOUCH SETUP");
  char b[48];
  snprintf(b, sizeof(b), "Tap the cross with a fingernail (%u of 3)", calStep_ + 1);
  textCenter(W / 2, 118, 1, TEXT, BG, b);
  if (calFailed_) textCenter(W / 2, 134, 1, WARN, BG, "Those taps didn't line up. Once more.");
  const int16_t px = calStep_ == 1 ? kTargets.right : kTargets.left;
  const int16_t py = calStep_ == 2 ? kTargets.bottom : kTargets.top;
  tft->drawFastHLine(px - 12, py, 25, WHITE);
  tft->drawFastVLine(px, py - 12, 25, WHITE);
  tft->drawCircle(px, py, 6, CRIT);
}

static void drawBoard() {
  tft->fillScreen(BG);
  textCenter(W / 2, 22, 2, ACCENT, BG, "WHICH BOARD?");
  textCenter(W / 2, 46, 1, MUTED, BG, "Look at the USB ports on its edge");
  drawButton(kBoard2432, "Two USB ports (micro-USB + USB-C)\nESP32-2432S028, the classic CYD", TEXT,
             set_.board == BOARD_2432 ? LINE : PANEL, ACCENT);
  drawButton(kBoard32E, "One USB-C, battery plug, BOOT + RESET\nESP32-32E (LCDWiki 2.8\")", TEXT,
             set_.board == BOARD_32E ? LINE : PANEL, ACCENT);
}

static const Button kDispInvert{8, 150, 98, 36};
static const Button kDispSwap{111, 150, 98, 36};
static const Button kDispRotate{214, 150, 98, 36};
static const Button kDispOk{8, 194, 304, 38};

static void drawDisplayCheck() {
  tft->fillScreen(BG);
  textCenter(W / 2, 8, 2, ACCENT, BG, "DISPLAY CHECK");
  const char* names[4] = {"RED", "GREEN", "BLUE", "WHITE"};
  const uint16_t cols[4] = {rgb(255, 0, 0), rgb(0, 255, 0), rgb(0, 0, 255), WHITE};
  for (int i = 0; i < 4; ++i) {
    const int16_t x = 12 + i * 76;
    tft->fillRect(x, 34, 68, 50, cols[i]);
    textCenter(x + 34, 90, 1, TEXT, BG, names[i]);
  }
  textCenter(W / 2, 108, 1, TEXT, BG, "Background dark, colors match their names,");
  textCenter(W / 2, 120, 1, TEXT, BG, "text right way up?");
  char b[48];
  snprintf(b, sizeof(b), "Display chip: %s (%s)", chip_ == CHIP_ILI9341 ? "ILI9341" : "ST7789",
           set_.chip == CHIP_AUTO ? "found" : "set");
  textCenter(W / 2, 134, 1, MUTED, BG, b);
  drawButton(kDispInvert, "Flip dark/light", TEXT, PANEL, LINE);
  drawButton(kDispSwap, "Swap red/blue", TEXT, PANEL, LINE);
  drawButton(kDispRotate, "Upside down", TEXT, PANEL, LINE);
  drawButton(kDispOk, "LOOKS GOOD", BG, OK, OK);
}

// ---------------------------------------------------------------------------
// Dashboard

static const Button kHeader{0, 0, W, 22};
static const Button kBanner{0, 24, W, 32};
static Button tileBtn(uint8_t i) { return Button{(int16_t)(4 + (i % 4) * 79), (int16_t)(60 + (i / 4) * 40), 75, 37}; }

// Signed: the dashboard can arrive (stamped with millis()) a moment after this loop's
// `now` was taken; unsigned, that reads as 49 days old and the screen blinks "no data".
// The summary comes about once a second. Keep showing the last one through a short gap
// (the master busy, a channel change) instead of flashing to "waiting for data".
static bool dashFresh(uint32_t now) { return haveDash_ && (int32_t)(now - dashMs_) < 20000; }
static bool catOn(uint8_t c) { return ((dash_.cats ^ pendingCats_) & catBit(c)) != 0; }

static void drawHeader(const NodeState& s, uint32_t now) {
  char right[40];
  const char* dep = !dashFresh(now) ? "" : dash_.deploy == DEPLOY_MOBILE ? "MOBILE" : dash_.deploy == DEPLOY_PORTABLE ? "PORTABLE" : "STATIONARY";
  const uint8_t batt = batteryPercent();
  char bs[8] = "";
  if (batt <= 100) snprintf(bs, sizeof(bs), "%u%%", batt);
  else if (batt == 254) snprintf(bs, sizeof(bs), "USB");
  // This CYD's GPS, when it has one: "GPS 12" (locked) or "GPS --" (searching / no data)
  char gps[12] = "";
  const bool locked = s.gps >= GPS_FIX_2D;
  if (s.gps) snprintf(gps, sizeof(gps), locked ? "GPS %u" : "GPS --", s.gps_sats);
  if (dashFresh(now))
    // Boards in the cluster, the master included (the master counts only its nodes)
    snprintf(right, sizeof(right), s.gps ? "%s  %u/%u  " : "%s  %u/%u boards  ", dep, dash_.nodes_online + 1u, dash_.nodes_total + 1u);
  else right[0] = '\0';
  char key[64];
  snprintf(key, sizeof(key), "%s|%s|%s", right, gps, bs);
  if (!changed(P_HEADER, key)) return;
  tft->fillRect(0, 0, W, 22, PANEL);
  drawLogo(4, 2, false, panelRgb_);
  // Right to left: power, GPS (green when locked), deployment and nodes
  int16_t x = W - 6 - (int16_t)strlen(bs) * 6;
  text(x, 8, 1, MUTED, PANEL, bs);
  if (gps[0]) {
    x -= (int16_t)(strlen(gps) + 2) * 6;
    text(x, 8, 1, locked ? OK : WARN, PANEL, gps);
  }
  x -= (int16_t)strlen(right) * 6;
  text(x, 8, 1, MUTED, PANEL, right);
}

static void drawBanner(uint32_t now) {
  char key[80];
  const DashAlert& a = dash_.alert;
  // Clears itself after the time set in SETTINGS > Alerts (default 1 minute), like the web page
  const uint16_t clearAfter = set_.clear_s == 255 ? 0xFFFF : set_.clear_s == 5 ? 300 : set_.clear_s ? set_.clear_s : 60;
  const bool show = dashFresh(now) && a.seq && a.seq != ackedSeq_ && a.age_s < clearAfter;
  char age[16];
  if (a.age_s < 60) snprintf(age, sizeof(age), "%us ago", a.age_s);
  else snprintf(age, sizeof(age), "%um ago", a.age_s / 60);
  if (show) snprintf(key, sizeof(key), "A%u|%u|%s|%s", a.seq, a.level, a.text, age);
  else if (dashFresh(now)) snprintf(key, sizeof(key), "C%u", dash_.devices_now);
  else snprintf(key, sizeof(key), "W");
  if (!changed(P_BANNER, key)) return;
  if (show) {
    const uint16_t fill = a.level >= 2 ? CRIT : PANEL, fg = a.level >= 2 ? WHITE : WARN;
    tft->fillRect(0, 24, W, 32, fill);
    if (a.level < 2) tft->drawRect(0, 24, W, 32, WARN);
    char top[40];
    snprintf(top, sizeof(top), "%s  %s  %s", a.level >= 2 ? "ALERT" : "NOTICE", kCatShort[a.category < CAT_COUNT ? a.category : 0], age);
    text(6, 27, 1, fg, fill, top);
    textRight(W - 6, 27, 1, fg, fill, "tap to clear");
    text(6, 38, 2, fg, fill, fit(a.text, 25));
  } else if (dashFresh(now)) {
    tft->fillRect(0, 24, W, 32, BG);
    text(6, 32, 2, OK, BG, "Scanning");
    char b[32];
    snprintf(b, sizeof(b), "%u devices now", dash_.devices_now);
    textRight(W - 6, 36, 1, MUTED, BG, b);
  } else {
    tft->fillRect(0, 24, W, 32, BG);
  }
}

static void drawTiles(uint32_t now) {
  for (uint8_t i = 0; i < 8; ++i) {
    const uint8_t c = kTileCats[i];
    const bool on = catOn(c);
    const uint8_t n = dash_.now[c];
    char key[24];
    snprintf(key, sizeof(key), "%u|%u|%u", on, n, dashFresh(now));
    if (!changed((Part)(P_TILE0 + i), key)) continue;
    const Button b = tileBtn(i);
    const uint16_t fill = on ? PANEL : BG;
    tft->fillRoundRect(b.x, b.y, b.w, b.h, 5, fill);
    tft->drawRoundRect(b.x, b.y, b.w, b.h, 5, on ? (n ? catColor[c] : LINE) : DIM);
    text(b.x + 5, b.y + 5, 1, on ? catColor[c] : DIM, fill, kCatShort[c]);
    drawIcon(b.x + b.w - 17, b.y + b.h - 17, i, on ? (n ? catColor[c] : MUTED) : DIM);
    char v[8];
    if (!on) snprintf(v, sizeof(v), "OFF");
    else snprintf(v, sizeof(v), "%u", n);
    text(b.x + 5, b.y + 17, 2, !on ? DIM : n ? TEXT : MUTED, fill, v);
  }
}

static void drawList(uint32_t now) {
  if (changed(P_LISTHEAD, dash_.n_threats ? "L" : "E")) {
    tft->fillRect(0, 141, W, 10, BG);
    text(6, 142, 1, MUTED, BG, "CLOSEST");
    textRight(W - 6, 142, 1, MUTED, BG, "dBm");
  }
  for (uint8_t i = 0; i < 4; ++i) {   // 4 fit above the tab bar
    const int16_t y = 153 + i * 15;
    char key[64];
    if (i < dash_.n_threats) {
      const DashThreat& t = dash_.threats[i];
      snprintf(key, sizeof(key), "%u|%d|%u|%s", t.category, t.rssi, t.flags, t.text);
    } else {
      snprintf(key, sizeof(key), "-%u", i == 0 && dash_.n_threats == 0);
    }
    if (!changed((Part)(P_ROW0 + i), key)) continue;
    tft->fillRect(0, y, W, 14, BG);
    if (i >= dash_.n_threats) {
      if (i == 0) text(12, y + 3, 1, MUTED, BG, "Scanning: nothing flagged nearby");
      continue;
    }
    const DashThreat& t = dash_.threats[i];
    const uint8_t c = t.category < CAT_COUNT ? t.category : 0;
    const bool quiet = t.flags & DT_QUIET;
    tft->fillRect(4, y + 2, 3, 10, catColor[c]);
    text(12, y + 3, 1, quiet ? DIM : catColor[c], BG, kCatShort[c]);
    const bool fol = t.flags & DT_FOLLOWING;
    text(66, y + 3, 1, quiet ? MUTED : TEXT, BG, fit(t.text, fol ? 23 : 34));
    if (fol) {
      tft->fillRoundRect(206, y + 1, 44, 12, 3, CRIT);
      text(210, y + 3, 1, WHITE, CRIT, "FOLLOW");
    }
    // signal: number + 4-step bar
    char r[8];
    snprintf(r, sizeof(r), "%d", t.rssi);
    textRight(W - 22, y + 3, 1, quiet ? DIM : MUTED, BG, r);
    const int bars = t.rssi >= -55 ? 4 : t.rssi >= -67 ? 3 : t.rssi >= -79 ? 2 : 1;
    for (int k = 0; k < 4; ++k)
      tft->fillRect(W - 18 + k * 4, y + 11 - k * 3, 3, 2 + k * 3, k < bars ? (quiet ? DIM : ACCENT) : LINE);
  }
}


// Instead of tiles + list when there's no master data to show.
static void drawBody(const NodeState& s, uint32_t now) {
  char key[48];
  const uint8_t state = s.unpaired ? (s.pair_offer ? 4 : 3) : !s.paired ? 0 : !dashFresh(now) ? 1 : 2;
  snprintf(key, sizeof(key), "%u|%04X|%u|%lu", state, s.master_id, state >= 3 ? 0u : (unsigned)((now / 1000) % 4),
           (unsigned long)(state == 4 ? s.pair_code : 0));
  if (state == 2) { partHash_[P_BODY] = 0; return; }
  if (!changed(P_BODY, key)) return;
  tft->fillRect(0, 24, W, kTabY - 24, BG);
  static const char* dots[4] = {"", ".", "..", "..."};
  char b[48];
  if (state == 3) {
    textCenter(W / 2, 40, 2, WARN, BG, "NOT PAIRED YET");
    text(14, 72, 1, TEXT, BG, "1. On the master's web page, open CLUSTER");
    text(14, 86, 1, TEXT, BG, "   and press PAIR A NODE.");
    text(14, 104, 1, TEXT, BG, "2. Hold this board within arm's reach");
    text(14, 118, 1, TEXT, BG, "   of the master.");
    text(14, 136, 1, TEXT, BG, "3. A code appears here and on the page.");
    text(14, 150, 1, TEXT, BG, "   Same code? Press ACCEPT on the page.");
    textCenter(W / 2, 182, 1, MUTED, BG, "This board waits here until it's paired.");
    return;
  }
  if (state == 4) {
    snprintf(b, sizeof(b), "Master %04X wants to pair. Its code:", s.pair_master);
    textCenter(W / 2, 44, 1, TEXT, BG, b);
    char code[8];
    snprintf(code, sizeof(code), "%03lu %03lu", (unsigned long)(s.pair_code / 1000 % 1000), (unsigned long)(s.pair_code % 1000));
    textCenter(W / 2, 74, 5, ACCENT, BG, code);
    textCenter(W / 2, 130, 1, TEXT, BG, "Does the master's page show the same code?");
    textCenter(W / 2, 146, 1, OK, BG, "Yes: press ACCEPT there.");
    textCenter(W / 2, 162, 1, CRIT, BG, "No: press IGNORE. Someone else may be pairing.");
    return;
  }
  if (state == 0) {
    snprintf(b, sizeof(b), "Looking for master%-3s", dots[(now / 1000) % 4]);
    textCenter(W / 2, 100, 2, WARN, BG, b);
    textCenter(W / 2, 130, 1, MUTED, BG, "Power up the master nearby (same cluster key).");
    textCenter(W / 2, 144, 1, MUTED, BG, "This board scans Classic Bluetooth meanwhile.");
  } else {
    snprintf(b, sizeof(b), "Connected to master %04X", s.master_id);
    textCenter(W / 2, 100, 2, TEXT, BG, b);
    textCenter(W / 2, 130, 1, MUTED, BG, "Waiting for dashboard data. If this stays,");
    textCenter(W / 2, 144, 1, MUTED, BG, "update the master's firmware.");
  }
}

static void drawDash(const NodeState& s, uint32_t now) {
  if (redraw_) {
    tft->fillScreen(BG);
    forgetParts();
    redraw_ = false;
  }
  drawHeader(s, now);
  if (!s.unpaired) drawBanner(now);
  if (s.paired && dashFresh(now) && !s.unpaired) {
    if (partHash_[P_BODY]) {  // coming back from the "no data" body: clear it
      tft->fillRect(0, 24, W, kTabY - 24, BG);
      partHash_[P_BODY] = 0;
      partHash_[P_BANNER] = 0;
      for (uint8_t p = P_TILE0; p < P_FOOTER; ++p) partHash_[p] = 0;
    }
    drawTiles(now);
    drawList(now);
  } else {
    if (!partHash_[P_BODY]) for (uint8_t p = P_TILE0; p < P_FOOTER; ++p) partHash_[p] = 0;
    drawBody(s, now);
  }
  drawTabs();
}

// ---------------------------------------------------------------------------
// Tabs and the other screens (DEVICES, detail, EVENTS, CLUSTER)

static const char* const kTabNames[4] = {"DASH", "DEVICES", "EVENTS", "CLUSTER"};
static const Screen kTabScreens[4] = {S_DASH, S_DEVICES, S_EVENTS, S_NODES};
static Button tabBtn(uint8_t i) { return Button{(int16_t)(i * 80), kTabY, 80, (int16_t)(H - kTabY)}; }

static void drawTabs() {
  const Screen cur = screen_ == S_DETAIL ? S_DEVICES : screen_;
  char key[8];
  snprintf(key, sizeof(key), "%u", (unsigned)cur);
  if (!changed(P_TABS, key)) return;
  tft->fillRect(0, kTabY, W, H - kTabY, PANEL);
  tft->drawFastHLine(0, kTabY, W, LINE);
  for (uint8_t i = 0; i < 4; ++i) {
    const Button b = tabBtn(i);
    const bool on = kTabScreens[i] == cur;
    if (on) tft->fillRect(b.x + 2, b.y + 2, b.w - 4, b.h - 3, LINE);
    textCenter(b.x + b.w / 2, b.y + 9, 1, on ? ACCENT : MUTED, on ? LINE : PANEL, kTabNames[i]);
    if (i) tft->drawFastVLine(b.x, b.y + 4, b.h - 8, LINE);
  }
}

// Paging column on the right of DEVICES / EVENTS / CLUSTER
static const Button kPgUp{280, 24, 40, 94};
static const Button kPgDown{280, 120, 40, 94};
static constexpr int16_t kListW = 276;
static void drawPager(uint16_t offset, uint16_t per, uint16_t total) {
  char key[24];
  snprintf(key, sizeof(key), "%u|%u|%u", offset, per, total);
  if (!changed(P_PAGER, key)) return;
  const bool up = offset > 0, down = offset + per < total;
  for (int k = 0; k < 2; ++k) {
    const Button& b = k ? kPgDown : kPgUp;
    const bool on = k ? down : up;
    tft->fillRoundRect(b.x + 2, b.y + 2, b.w - 4, b.h - 4, 6, on ? PANEL : BG);
    tft->drawRoundRect(b.x + 2, b.y + 2, b.w - 4, b.h - 4, 6, on ? LINE : DIM);
    const int16_t cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (k) tft->fillTriangle(cx - 9, cy - 5, cx + 9, cy - 5, cx, cy + 7, on ? ACCENT : DIM);
    else tft->fillTriangle(cx - 9, cy + 5, cx + 9, cy + 5, cx, cy - 7, on ? ACCENT : DIM);
  }
}

static void drawTitle(const char* left, const char* right) {
  char key[80];
  snprintf(key, sizeof(key), "%s|%s", left, right);
  if (!changed(P_TITLE, key)) return;
  tft->fillRect(0, 24, kListW, 13, BG);
  text(6, 26, 1, ACCENT, BG, left);
  textRight(kListW - 4, 26, 1, MUTED, BG, right);
}

// Status line for screens waiting on the master
static bool viewReady(uint32_t now, const NodeState& s) {
  if (viewMs_ && (int32_t)(now - viewMs_) < 8000) return true;
  char key[32];
  snprintf(key, sizeof(key), "wait%u%u", s.paired, (unsigned)((now / 1000) % 4));
  if (changed(P_LINE0, key)) {
    tft->fillRect(0, 38, kListW, kTabY - 38, BG);
    static const char* dots[4] = {"", ".", "..", "..."};
    char b[48];
    snprintf(b, sizeof(b), s.paired ? "Asking the master%s" : "No master yet%s", dots[(now / 1000) % 4]);
    textCenter(kListW / 2, 110, 1, MUTED, BG, b);
    for (uint8_t i = 1; i < 12; ++i) partHash_[P_LINE0 + i] = 0;
  }
  return false;
}

static const char* radioName(uint8_t r) {
  switch (r) { case 1: return "Wi-Fi"; case 2: return "BLE"; case 3: return "Classic"; case 4: return "LoRa"; default: return "?"; }
}
static void fmtAge(char* out, size_t n, uint32_t s) {
  if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
  else snprintf(out, n, "%lud", (unsigned long)(s / 86400));
}
static void signalBars(int16_t x, int16_t y, int8_t rssi, bool dim) {
  const int bars = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -79 ? 2 : 1;
  for (int k = 0; k < 4; ++k) tft->fillRect(x + k * 4, y + 9 - k * 3, 3, 2 + k * 3, k < bars ? (dim ? DIM : ACCENT) : LINE);
}

// DEVICES: 6 rows, two lines each; tap one for details
static constexpr int16_t kRowY = 38, kRowH = 29;
static void drawDevices(const NodeState& s, uint32_t now) {
  char l[40], r[24];
  snprintf(l, sizeof(l), "DEVICES");
  if (viewMs_ && devs_.total) snprintf(r, sizeof(r), "%u-%u of %u", devs_.offset + 1, devs_.offset + devs_.n, devs_.total);
  else r[0] = '\0';
  drawTitle(l, r);
  drawPager(devs_.offset, kDashRows, viewMs_ ? devs_.total : 0);
  if (!viewReady(now, s)) return;
  for (uint8_t i = 0; i < kDashRows; ++i) {
    const int16_t y = kRowY + i * kRowH;
    char key[72];
    if (i < devs_.n) {
      const DashDevRow& d = devs_.rows[i];
      snprintf(key, sizeof(key), "%lu|%u|%d|%u|%u|%u|%s", (unsigned long)d.id, d.category, d.rssi, d.conf, d.flags,
               d.age_s / 5, d.text);
    } else {
      snprintf(key, sizeof(key), "-%u", i == 0 && devs_.n == 0);
    }
    if (!changed((Part)(P_LINE0 + i), key)) continue;
    tft->fillRect(0, y, kListW, kRowH, BG);
    if (i >= devs_.n) {
      if (i == 0) textCenter(kListW / 2, y + 40, 1, MUTED, BG, "Nothing heard in the categories that are on");
      continue;
    }
    const DashDevRow& d = devs_.rows[i];
    const uint8_t c = d.category < CAT_COUNT ? d.category : 0;
    const bool quiet = d.age_s > 10;
    tft->fillRect(2, y + 3, 3, kRowH - 6, catColor[c]);
    text(9, y + 4, 1, quiet ? DIM : catColor[c], BG, kCatShort[c]);
    text(63, y + 4, 1, quiet ? MUTED : TEXT, BG, fit(d.text, 35));
    char b[48], age[12];
    fmtAge(age, sizeof(age), d.age_s);
    snprintf(b, sizeof(b), "%d dBm  %s ago  %u%%  %s", d.rssi, d.age_s >= 255 ? ">4m" : age, d.conf, radioName(d.radio));
    text(63, y + 16, 1, quiet ? DIM : MUTED, BG, b);
    signalBars(9, y + 14, d.rssi, quiet);
    if (d.flags & DR_FOLLOWING) {
      tft->fillRoundRect(kListW - 48, y + 14, 44, 12, 3, CRIT);
      text(kListW - 44, y + 16, 1, WHITE, CRIT, "FOLLOW");
    }
    if (i + 1 < kDashRows) tft->drawFastHLine(9, y + kRowH - 1, kListW - 12, PANEL);
  }
}

// A few makers worth naming on a small screen (Bluetooth SIG company ids)
static const char* makerName(uint16_t co) {
  switch (co) {
    case 0x004C: return "Apple"; case 0x0006: return "Microsoft"; case 0x0075: return "Samsung";
    case 0x00E0: return "Google"; case 0x0087: return "Garmin"; case 0x0059: return "Nordic Semi";
    case 0x02E5: return "Espressif"; case 0x0171: return "Amazon"; case 0x009E: return "Bose";
    case 0x012D: return "Sony"; case 0x08C3: return "Chipolo";
    default: return nullptr;
  }
}

static const Button kBack{0, 24, 70, 26};
static void drawDetail(const NodeState& s, uint32_t now) {
  if (changed(P_TITLE, "detail")) {
    tft->fillRect(0, 24, W, 28, BG);
    drawButton(kBack, "< BACK", TEXT, PANEL, LINE);
  }
  if (!viewMs_ || (int32_t)(now - viewMs_) > 8000) {
    if (changed(P_LINE0, "wait")) {
      tft->fillRect(0, 54, W, kTabY - 54, BG);
      textCenter(W / 2, 110, 1, MUTED, BG, "Asking the master...");
    }
    return;
  }
  const DashDetail& d = detail_;
  char key[96];
  snprintf(key, sizeof(key), "%lu|%u|%d|%d|%lu|%lu|%u|%u", (unsigned long)d.id, d.found, d.rssi, d.rssi_max,
           (unsigned long)(d.age_s / 2), (unsigned long)d.hits, d.conf, d.places);
  if (!changed(P_LINE0, key)) return;
  tft->fillRect(0, 54, W, kTabY - 54, BG);
  if (!d.found) {
    textCenter(W / 2, 110, 1, MUTED, BG, "This device has dropped off the list.");
    return;
  }
  const uint8_t c = d.category < CAT_COUNT ? d.category : 0;
  textRight(W - 6, 31, 1, catColor[c], BG, kCatShort[c]);
  text(6, 56, 2, TEXT, BG, fit(d.name[0] ? d.name : d.sig, 26));
  int16_t y = 78;
  char b[64], a[18];
  auto row = [&](const char* label, const char* value, uint16_t color) {
    text(6, y, 1, MUTED, BG, label);
    text(84, y, 1, color, BG, fit(value, 39));
    y += 13;
  };
  if (d.name[0]) row("What", d.sig, TEXT);
  formatMac(d.addr, a);
  snprintf(b, sizeof(b), "%s  %s", a, (d.flags & DR_RANDOM) ? "random" : "fixed");
  row("Address", b, TEXT);
  if (d.company != 0xFFFF) {
    const char* m = makerName(d.company);
    if (m) snprintf(b, sizeof(b), "%s", m); else snprintf(b, sizeof(b), "company 0x%04X", d.company);
    row("Maker", b, TEXT);
  }
  snprintf(b, sizeof(b), "%d dBm now, %d peak", d.rssi, d.rssi_max);
  row("Signal", b, TEXT);
  snprintf(b, sizeof(b), "%u%%%s", d.conf, (d.flags & DR_STRONG) ? ", strong match" : "");
  row("Confidence", b, d.conf >= 75 ? WARN : TEXT);
  char a1[12], a2[12];
  fmtAge(a1, sizeof(a1), d.age_s);
  fmtAge(a2, sizeof(a2), d.first_s);
  snprintf(b, sizeof(b), "%s ago, first %s ago, %lu hits", a1, a2, (unsigned long)d.hits);
  row("Seen", b, TEXT);
  if (d.channel) snprintf(b, sizeof(b), "%s, channel %u", radioName(d.radio), d.channel);
  else snprintf(b, sizeof(b), "%s", radioName(d.radio));
  row("Radio", b, TEXT);
  if (d.rotations) { snprintf(b, sizeof(b), "%u (stitched by fingerprint)", d.rotations); row("Addr changes", b, TEXT); }
  if (d.places > 1 || (d.flags & DR_FOLLOWING)) {
    snprintf(b, sizeof(b), "%u places%s", d.places, (d.flags & DR_FOLLOWING) ? ", FOLLOWING" : "");
    row("With you", b, (d.flags & DR_FOLLOWING) ? CRIT : TEXT);
  }
  if (d.flags & DR_SPOOFED) row("Note", "part of a BLE spam burst", WARN);
}

// EVENTS: newest first, 5 per page
static constexpr int16_t kEvH = 35;
static void drawEvents(const NodeState& s, uint32_t now) {
  char r[24];
  if (viewMs_ && events_.total) snprintf(r, sizeof(r), "%u-%u of %u", events_.offset + 1, events_.offset + events_.n, events_.total);
  else r[0] = '\0';
  drawTitle("EVENTS", r);
  drawPager(events_.offset, kDashEvents, viewMs_ ? events_.total : 0);
  if (!viewReady(now, s)) return;
  for (uint8_t i = 0; i < kDashEvents; ++i) {
    const int16_t y = kRowY + i * kEvH;
    char key[72];
    if (i < events_.n) {
      const DashEvent& e = events_.ev[i];
      snprintf(key, sizeof(key), "%u|%lu|%s", e.level, (unsigned long)(e.age_s < 60 ? e.age_s / 5 : e.age_s / 60), e.text);
    } else {
      snprintf(key, sizeof(key), "-%u", i == 0 && events_.n == 0);
    }
    if (!changed((Part)(P_LINE0 + i), key)) continue;
    tft->fillRect(0, y, kListW, kEvH, BG);
    if (i >= events_.n) {
      if (i == 0) textCenter(kListW / 2, y + 40, 1, MUTED, BG, "Nothing yet");
      continue;
    }
    const DashEvent& e = events_.ev[i];
    const uint16_t col = e.level >= 2 ? CRIT : e.level == 1 ? WARN : MUTED;
    const char* lv = e.level >= 2 ? "ALERT" : e.level == 1 ? "NOTICE" : "INFO";
    tft->fillRoundRect(6, y + 3, 46, 12, 3, col);
    textCenter(29, y + 5, 1, BG, col, lv);
    char age[12], b[32];
    fmtAge(age, sizeof(age), e.age_s);
    snprintf(b, sizeof(b), "%s ago", age);
    if (e.category && e.category < CAT_COUNT) text(58, y + 5, 1, catColor[e.category], BG, kCatShort[e.category]);
    textRight(kListW - 4, y + 5, 1, MUTED, BG, b);
    text(6, y + 19, 1, e.level >= 1 ? TEXT : MUTED, BG, fit(e.text, 45));
    if (i + 1 < kDashEvents) tft->drawFastHLine(6, y + kEvH - 1, kListW - 10, PANEL);
  }
}

// CLUSTER: the master, then every node
static const char* hwShort(uint8_t hw) {
  switch (hw) { case 1: return "C5"; case 2: return "Marauder V8"; case 3: return "CYD"; case 4: return "SBC";
    case 5: return "Heltec"; case 6: return "ESP32"; default: return "board"; }
}
static void drawNodes(const NodeState& s, uint32_t now) {
  char r[24];
  if (viewMs_) {
    uint8_t on = 0;
    for (uint8_t i = 0; i < nodes_.n; ++i) on += (nodes_.nodes[i].flags & DN_ONLINE) ? 1 : 0;
    snprintf(r, sizeof(r), "%u boards", nodes_.total);
  } else {
    r[0] = '\0';
  }
  drawTitle("CLUSTER", r);
  drawPager(nodes_.offset, kDashNodes, viewMs_ ? nodes_.total : 0);
  if (!viewReady(now, s)) return;
  for (uint8_t i = 0; i < kDashNodes; ++i) {
    const int16_t y = kRowY + i * kRowH;
    char key[96];
    if (i < nodes_.n) {
      const DashNode& n = nodes_.nodes[i];
      snprintf(key, sizeof(key), "%04X|%u|%d|%u|%u|%u|%u|%u|%lu|%s", n.id, n.flags, n.link, n.loss_pct, n.batt, n.gps,
               n.sats, n.losses, (unsigned long)(n.up_s / 60), n.label);
    } else {
      snprintf(key, sizeof(key), "-");
    }
    if (!changed((Part)(P_LINE0 + i), key)) continue;
    tft->fillRect(0, y, kListW, kRowH, BG);
    if (i >= nodes_.n) continue;
    const DashNode& n = nodes_.nodes[i];
    const bool me = n.id == s.my_id;
    const bool online = n.flags & DN_ONLINE;
    char b[64];
    if (n.label[0]) snprintf(b, sizeof(b), "%04X %s", n.id, n.label);
    else snprintf(b, sizeof(b), "%04X %s", n.id, hwShort(n.hw));
    text(6, y + 4, 1, me ? ACCENT : TEXT, BG, fit(b, 28));
    const char* st = (n.flags & DN_MASTER) ? "MASTER" : !online ? "OFFLINE" : (n.flags & DN_ACK) ? (me ? "THIS ONE" : "ONLINE") : "GETTING ORDERS";
    const uint16_t sc = !online ? CRIT : (n.flags & DN_MASTER) ? ACCENT : (n.flags & DN_ACK) ? OK : WARN;
    textRight(kListW - 4, y + 4, 1, sc, BG, st);
    char up[12], bt[10] = "", gp[14] = "", rs[14] = "";
    fmtAge(up, sizeof(up), n.up_s);
    if (n.batt <= 100) snprintf(bt, sizeof(bt), " %u%%", n.batt); else if (n.batt == 254) snprintf(bt, sizeof(bt), " USB");
    if (n.gps >= 3) snprintf(gp, sizeof(gp), " GPS %u", n.sats); else if (n.gps == 2) snprintf(gp, sizeof(gp), " GPS --");
    if (n.reset == RW_CRASH) snprintf(rs, sizeof(rs), " CRASHED");
    else if (n.reset == RW_WATCHDOG) snprintf(rs, sizeof(rs), " HUNG");
    else if (n.reset == RW_BROWNOUT) snprintf(rs, sizeof(rs), " BROWNOUT");
    if (n.flags & DN_MASTER) snprintf(b, sizeof(b), "%s  up %s%s%s", hwShort(n.hw), up, bt, gp);
    else snprintf(b, sizeof(b), "%s %d dBm %u.%u%% loss up %s%s%s%s", hwShort(n.hw), n.link, n.loss_pct / 10,
                  n.loss_pct % 10, up, bt, gp, rs);
    text(6, y + 16, 1, rs[0] ? WARN : MUTED, BG, fit(b, 44));
    if (i + 1 < kDashNodes) tft->drawFastHLine(6, y + kRowH - 1, kListW - 10, PANEL);
  }
}

static void drawView(const NodeState& s, uint32_t now) {
  if (redraw_) {
    tft->fillScreen(BG);
    forgetParts();
    redraw_ = false;
  }
  drawHeader(s, now);
  if (screen_ == S_DEVICES) drawDevices(s, now);
  else if (screen_ == S_DETAIL) drawDetail(s, now);
  else if (screen_ == S_EVENTS) drawEvents(s, now);
  else drawNodes(s, now);
  drawTabs();
}

static void pageBy(int dir, uint32_t now) {
  const uint16_t per = screen_ == S_EVENTS ? kDashEvents : screen_ == S_NODES ? kDashNodes : kDashRows;
  const uint16_t total = screen_ == S_EVENTS ? events_.total : screen_ == S_NODES ? nodes_.total : devs_.total;
  int32_t off = (int32_t)viewOffset_ + dir * per;
  if (off >= (int32_t)total) return;
  if (off < 0) off = 0;
  if (off == viewOffset_) return;
  askView(viewWant_, (uint16_t)off, 0, now);
  for (uint8_t i = 0; i < 12; ++i) partHash_[P_LINE0 + i] = 0;
}

// ---------------------------------------------------------------------------
// Settings screen

static const Button kSetBack{240, 2, 76, 22};
static const Button kSetDeploy{8, 30, 148, 40};
static const Button kSetNight{164, 30, 148, 40};
static const Button kSetBeep{8, 74, 148, 40};
static const Button kSetTouch{164, 74, 148, 40};
static const Button kSetDisplay{8, 118, 148, 40};
static const Button kSetPair{164, 118, 148, 40};
static uint32_t pairConfirmUntil_ = 0;   // "tap again to confirm"
static bool wantUnpair_ = false;
static const Button kSetBoard{8, 162, 148, 40};
static const Button kSetBright{164, 162, 148, 40};
static const uint8_t kBrightSteps[5] = {10, 25, 50, 75, 100};

static const Button kSetSd{0, 218, 320, 22};   // the SD line at the bottom
static uint8_t sdShown_ = 255;
static uint32_t sdSavedShown_ = 0, sdDrawMs_ = 0;
static void drawSettings(const NodeState& s, uint32_t now) {
  tft->fillScreen(BG);
  text(8, 6, 2, ACCENT, BG, "SETTINGS");
  drawButton(kSetBack, "BACK", BG, ACCENT, ACCENT);
  char b[48];
  const char* dep = dash_.deploy == DEPLOY_MOBILE ? "Mobile" : dash_.deploy == DEPLOY_PORTABLE ? "Portable" : "Stationary";
  if (dashFresh(now)) snprintf(b, sizeof(b), "Deployment: %s\ntap: whole cluster", dep);
  else snprintf(b, sizeof(b), "Deployment\nneeds the master");
  drawButton(kSetDeploy, b, TEXT, PANEL, LINE);
  snprintf(b, sizeof(b), "Theme: %s\n%s", set_.night ? "NIGHT (red)" : "DAY (blue)", set_.night ? "tap for day" : "tap for night");
  drawButton(kSetNight, b, TEXT, PANEL, LINE);
  snprintf(b, sizeof(b), "Alerts: beep %s\nclear, sound test", set_.beep ? "ON" : "OFF");
  drawButton(kSetBeep, b, TEXT, PANEL, LINE);
  drawButton(kSetTouch, "Touch setup\ntap three crosses again", TEXT, PANEL, LINE);
  drawButton(kSetDisplay, "Display check\ncolors and orientation", TEXT, PANEL, LINE);
  snprintf(b, sizeof(b), "Board: %s\ntap to change", set_.board == BOARD_32E ? "ESP32-32E" : "ESP32-2432S028");
  drawButton(kSetBoard, b, TEXT, PANEL, LINE);
  if ((int32_t)(pairConfirmUntil_ - now) > 0) drawButton(kSetPair, "TAP AGAIN TO CONFIRM\nforgets its master", BG, WARN, WARN);
  else drawButton(kSetPair, "Pair with a master\nnew or different one", TEXT, PANEL, LINE);
  const uint8_t pct = set_.night ? (set_.bright_night ? set_.bright_night : 25) : (set_.bright_day ? set_.bright_day : 100);
  snprintf(b, sizeof(b), "Brightness: %u%%\n%s, tap to change", pct, set_.night ? "night" : "day");
  drawButton(kSetBright, b, TEXT, PANEL, LINE);
  // Who this is: the name given on the master (CLUSTER > EDIT on the web page) and its id
  if (s.label && s.label[0]) snprintf(b, sizeof(b), "This node: %s (%04X)", s.label, s.my_id);
  else snprintf(b, sizeof(b), "This node: %04X (name it in the web page's CLUSTER)", s.my_id);
  text(8, 210, 1, TEXT, BG, fit(b, 52));
  char sd[28];
  switch (s.sd) {
    case SD_LOGGING: snprintf(sd, sizeof(sd), "SD: logging, %lu saved", (unsigned long)s.sd_saved); break;
    case SD_ERROR: snprintf(sd, sizeof(sd), "SD: CAN'T WRITE"); break;
    case SD_FULL: snprintf(sd, sizeof(sd), "SD: FULL"); break;
    case SD_UNFORMATTED: snprintf(sd, sizeof(sd), "SD: needs FORMAT"); break;
    default: snprintf(sd, sizeof(sd), "SD: no card"); break;
  }
  snprintf(b, sizeof(b), "%s  >  SD CARD", sd);
  text(8, 225, 1, s.sd == SD_ERROR || s.sd == SD_FULL || s.sd == SD_UNFORMATTED ? WARN : ACCENT, BG, b);
  sdShown_ = s.sd;
  sdSavedShown_ = s.sd_saved;
}

// ---------------------------------------------------------------------------
// Alerts: beep on/off, test the speaker, how long the banner stays

static const Button kAlBack{240, 2, 76, 22};
static const Button kAlBeep{8, 30, 148, 40};
static const Button kAlTest{164, 30, 148, 40};
static const Button kAlClear[4] = {{8, 100, 72, 36}, {86, 100, 72, 36}, {164, 100, 72, 36}, {242, 100, 70, 36}};
static const uint8_t kClearVals[4] = {30, 60, 5, 255};
static const char* const kClearText[4] = {"30 S", "1 MIN", "5 MIN", "NEVER"};
static void drawAlerts() {
  tft->fillScreen(BG);
  text(8, 6, 2, ACCENT, BG, "ALERTS");
  drawButton(kAlBack, "BACK", BG, ACCENT, ACCENT);
  drawButton(kAlBeep, set_.beep ? "Beep on alerts: ON\ntap to turn off" : "Beep on alerts: OFF\ntap to turn on", TEXT, PANEL, LINE);
  drawButton(kAlTest, "TEST BEEP\nplays the alert sound", TEXT, PANEL, LINE);
  text(8, 84, 1, TEXT, BG, "Alert banner clears itself after:");
  const uint8_t cur = set_.clear_s ? set_.clear_s : 60;
  for (int i = 0; i < 4; ++i) {
    const bool sel = kClearVals[i] == cur;
    drawButton(kAlClear[i], kClearText[i], sel ? BG : TEXT, sel ? ACCENT : PANEL, sel ? ACCENT : LINE);
  }
  text(8, 150, 1, MUTED, BG, "Tap the banner to clear it sooner. EVENTS keeps");
  text(8, 162, 1, MUTED, BG, "every alert either way.");
  text(8, 186, 1, MUTED, BG, "Speaker: the 2-pin SPK plug (8 ohm, 0.5-3 W).");
  text(8, 198, 1, MUTED, BG, "No sound on TEST BEEP? Check the plug, and that");
  text(8, 210, 1, MUTED, BG, "SETTINGS > Board matches your board.");
}

// ---------------------------------------------------------------------------
// SD card: status, and FORMAT (erases everything on the card) behind a second tap

static const Button kSdBack{240, 2, 76, 22};
static const Button kSdFormat{60, 160, 200, 44};
static uint32_t sdConfirmUntil_ = 0;
static uint8_t sdScrShown_ = 255;
static uint32_t sdScrSaved_ = 0;
static void drawSd(const NodeState& s, uint32_t now) {
  tft->fillScreen(BG);
  text(8, 6, 2, ACCENT, BG, "SD CARD");
  char b[64];
  if (s.sd == SD_FORMATTING) {
    textCenter(W / 2, 80, 2, WARN, BG, "FORMATTING...");
    textCenter(W / 2, 112, 1, TEXT, BG, "Don't remove the card or power off.");
    textCenter(W / 2, 128, 1, MUTED, BG, "Takes a few seconds to a minute.");
    sdScrShown_ = s.sd;
    return;
  }
  drawButton(kSdBack, "BACK", BG, ACCENT, ACCENT);
  const uint32_t mb = cydsd::cardMB();
  switch (s.sd) {
    case SD_LOGGING: snprintf(b, sizeof(b), "Logging: %lu records since power on", (unsigned long)s.sd_saved); break;
    case SD_ERROR: snprintf(b, sizeof(b), "Card is in but can't be written"); break;
    case SD_FULL: snprintf(b, sizeof(b), "Card is full"); break;
    case SD_UNFORMATTED: snprintf(b, sizeof(b), "Card found, but not FAT32 (exFAT?): FORMAT it"); break;
    default: snprintf(b, sizeof(b), "No card found: check it's pushed all the way in"); break;
  }
  text(8, 36, 1, s.sd == SD_LOGGING ? TEXT : WARN, BG, b);
  if (mb) snprintf(b, sizeof(b), "Card: %lu MB", (unsigned long)mb);
  else snprintf(b, sizeof(b), "Card: -");
  text(8, 52, 1, MUTED, BG, b);
  const char* f = cydsd::fileName();
  snprintf(b, sizeof(b), "File: %s", f[0] ? f : "(nothing written yet)");
  text(8, 68, 1, MUTED, BG, fit(b, 52));
  if (cydsd::formatResult()[0]) {
    snprintf(b, sizeof(b), "Last format: %s", cydsd::formatResult());
    text(8, 84, 1, cydsd::formatOk() ? TEXT : WARN, BG, fit(b, 52));
  }
  text(8, 108, 1, MUTED, BG, "FORMAT erases EVERYTHING on the card, logs");
  text(8, 120, 1, MUTED, BG, "included, and sets it up as FAT32. Works for a");
  text(8, 132, 1, MUTED, BG, "card from a camera or phone, or one that fails.");
  if ((int32_t)(sdConfirmUntil_ - now) > 0) drawButton(kSdFormat, "TAP AGAIN TO ERASE ALL DATA", BG, WARN, WARN);
  else drawButton(kSdFormat, "FORMAT SD CARD\nerases all data", WARN, PANEL, WARN);
  sdScrShown_ = s.sd;
  sdScrSaved_ = s.sd_saved;
}

// ---------------------------------------------------------------------------
// Identify: "which one is this?"

static bool identifying_ = false;
static bool wasDark_ = false;
static void drawIdentify(const NodeState& s, uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 250) return;
  last = now;
  const bool flip = (now / 250) % 2;
  tft->fillScreen(flip ? ACCENT : BG);
  textCenter(W / 2, 90, 4, flip ? BG : ACCENT, flip ? ACCENT : BG, "THIS ONE");
  textCenter(W / 2, 140, 2, flip ? BG : TEXT, flip ? ACCENT : BG, s.label && s.label[0] ? s.label : "SkySentinel node");
}

// ---------------------------------------------------------------------------
// Touch handling per screen

static int16_t pressRaw_[2] = {0, 0};
static uint32_t pressMs_ = 0;
static uint32_t lastTap_ = 0;
static uint32_t wakeUntil_ = 0;

static void onTap(int16_t x, int16_t y, uint32_t now) {
  switch (screen_) {
    case S_BOARD:
      if (hit(kBoard2432, x, y) || hit(kBoard32E, x, y)) {
        set_.board = hit(kBoard32E, x, y) ? BOARD_32E : BOARD_2432;
        saveSettings();
        go(S_DISPLAY);
      }
      break;
    case S_DISPLAY:
      if (hit(kDispInvert, x, y)) {
        set_.invert = invertOn() ? 0 : 1;
        tft->invertDisplay(invertOn());
        saveSettings();
      } else if (hit(kDispSwap, x, y)) {
        set_.swap_rb ^= 1;
        buildColors();
        saveSettings();
        redraw_ = true;
      } else if (hit(kDispRotate, x, y)) {
        set_.rot180 ^= 1;
        tft->setRotation(set_.rot180 ? 3 : 1);
        saveSettings();
        redraw_ = true;
      } else if (hit(kDispOk, x, y)) {
        set_.done = 1;
        fullSetup_ = false;
        saveSettings();
        go(S_DASH);
      }
      break;
    case S_ALERTS:
      if (hit(kAlBack, x, y)) go(S_SETTINGS);
      else if (hit(kAlBeep, x, y)) { set_.beep ^= 1; saveSettings(); redraw_ = true; if (set_.beep) playBeeps(now); }
      else if (hit(kAlTest, x, y)) playBeeps(now);
      else
        for (int i = 0; i < 4; ++i)
          if (hit(kAlClear[i], x, y)) { set_.clear_s = kClearVals[i]; saveSettings(); redraw_ = true; forgetParts(); }
      break;
    case S_SD:
      if (cydsd::formatPending()) break;
      if (hit(kSdBack, x, y)) { sdConfirmUntil_ = 0; go(S_SETTINGS); }
      else if (hit(kSdFormat, x, y)) {
        if ((int32_t)(sdConfirmUntil_ - now) > 0) { sdConfirmUntil_ = 0; cydsd::requestFormat(); }
        else sdConfirmUntil_ = now + 4000;
        redraw_ = true;
      }
      break;
    case S_SETTINGS:
      if (hit(kSetBack, x, y)) go(lastTab_);
      else if (hit(kSetSd, x, y)) go(S_SD);
      else if (hit(kSetBright, x, y)) {
        uint8_t& v = set_.night ? set_.bright_night : set_.bright_day;
        const uint8_t cur = v ? v : (set_.night ? 25 : 100);
        uint8_t next = kBrightSteps[0];
        for (uint8_t i = 0; i < 5; ++i) if (kBrightSteps[i] > cur) { next = kBrightSteps[i]; break; }
        v = next;  // past 100 wraps to the dimmest
        saveSettings();
        backlight(true);
        redraw_ = true;
      } else if (hit(kSetNight, x, y)) {
        set_.night ^= 1;
        saveSettings();
        buildColors();
        backlight(true);
        redraw_ = true;
      }
      else if (hit(kSetDeploy, x, y) && dashFresh(now)) {
        dash_.deploy = (uint8_t)((dash_.deploy + 1) % 3);
        queueCmd(DC_DEPLOY, dash_.deploy);
        redraw_ = true;
      } else if (hit(kSetBeep, x, y)) go(S_ALERTS);
      else if (hit(kSetTouch, x, y)) startCalibration();
      else if (hit(kSetDisplay, x, y)) go(S_DISPLAY);
      else if (hit(kSetBoard, x, y)) {
        led(false, false, false);
        set_.board = set_.board == BOARD_32E ? BOARD_2432 : BOARD_32E;
        saveSettings();
        redraw_ = true;
      } else if (hit(kSetPair, x, y)) {
        if ((int32_t)(pairConfirmUntil_ - now) > 0) wantUnpair_ = true;
        else pairConfirmUntil_ = now + 4000;
        redraw_ = true;
      }
      break;
    case S_DEVICES:
    case S_DETAIL:
    case S_EVENTS:
    case S_NODES:
      if (hit(kHeader, x, y)) { go(S_SETTINGS); break; }
      if (y >= kTabY) { go(kTabScreens[x / 80 < 4 ? x / 80 : 3]); break; }
      if (screen_ == S_DETAIL) { if (hit(kBack, x, y)) go(S_DEVICES); break; }
      if (hit(kPgUp, x, y)) { pageBy(-1, now); break; }
      if (hit(kPgDown, x, y)) { pageBy(1, now); break; }
      if (screen_ == S_DEVICES && viewMs_ && x < kListW && y >= kRowY) {
        const int row = (y - kRowY) / kRowH;
        if (row >= 0 && row < devs_.n) {
          viewId_ = devs_.rows[row].id;
          go(S_DETAIL);
        }
      }
      break;
    case S_DASH:
      if (hit(kHeader, x, y)) { go(S_SETTINGS); break; }
      if (y >= kTabY) { go(kTabScreens[x / 80 < 4 ? x / 80 : 3]); break; }
      if (hit(kBanner, x, y)) { ackedSeq_ = dash_.alert.seq; partHash_[P_BANNER] = 0; break; }
      if (dashFresh(now)) {
        for (uint8_t i = 0; i < 8; ++i)
          if (hit(tileBtn(i), x, y)) {
            pendingCats_ ^= catBit(kTileCats[i]);
            pendingCatsMs_ = now;
            queueCmd(DC_TOGGLE_CAT, kTileCats[i]);
          }
      }
      break;
    default:
      break;
  }
}

static void pollTouch(const NodeState& s, uint32_t now) {
  int16_t rx, ry;
  const bool down = touchRaw(rx, ry);
  if (screen_ == S_CHIP) {  // any press = "yes, it looks right" (touch isn't calibrated yet)
    if (down && !pressed_) {
      pressed_ = true;
      set_.chip = chip_;
      chipGuessed_ = false;
      saveSettings();
      startCalibration();
    } else if (!down) {
      pressed_ = false;
    }
    return;
  }
  // A resistive panel lets go for a few ms in the middle of one tap: only call it
  // released after 150 ms with nothing.
  if (down) lastDownMs_ = now;
  const bool held = down || (pressed_ && now - lastDownMs_ < 150);
  if (screen_ == S_CAL) {
    // Calibration uses raw readings: average the press, record on release.
    if ((int32_t)(calQuietUntil_ - now) > 0) { if (!held) pressed_ = false; return; }
    if (down) {
      if (!pressed_) { pressed_ = true; pressRaw_[0] = rx; pressRaw_[1] = ry; pressMs_ = now; }
      else { pressRaw_[0] = (int16_t)((pressRaw_[0] * 3 + rx) / 4); pressRaw_[1] = (int16_t)((pressRaw_[1] * 3 + ry) / 4); }
    } else if (pressed_ && !held) {
      pressed_ = false;
      if (lastDownMs_ - pressMs_ < 40) return;  // a blip, not a tap
      calRaw_[calStep_][0] = pressRaw_[0];
      calRaw_[calStep_][1] = pressRaw_[1];
      Serial.printf("[cyd] touch setup tap %u: raw x=%d y=%d (%s)\n", calStep_ + 1, pressRaw_[0], pressRaw_[1],
                    set_.touch_bus == TB_SHARED ? "display bus" : "own pins");
      calQuietUntil_ = now + 500;  // the next cross needs a new tap
      if (++calStep_ < 3) { redraw_ = true; return; }
      TouchCal c;
      if (!touchCalibrate(calRaw_, c)) {
        Serial.println("[cyd] touch setup: taps too close together, starting over");
        // Three taps that all read the same: we're listening on the wrong wiring.
        if (abs(calRaw_[0][0] - calRaw_[1][0]) < 40 && abs(calRaw_[0][1] - calRaw_[2][1]) < 40 &&
            abs(calRaw_[0][1] - calRaw_[1][1]) < 40) {
          set_.touch_bus = set_.touch_bus == TB_SHARED ? TB_OWN : TB_SHARED;
          saveSettings();
          Serial.printf("[cyd] touch: switching to %s\n", set_.touch_bus == TB_SHARED ? "the display's SPI bus" : "its own pins");
        }
        calStep_ = 0; calFailed_ = true; redraw_ = true; return;
      }
      set_.cal = c;
      set_.cal_rot = set_.rot180;
      saveSettings();
      go(fullSetup_ || set_.board == BOARD_UNSET ? S_BOARD : (set_.done ? S_SETTINGS : S_DISPLAY));
    }
    return;
  }
  if (!held) { pressed_ = false; return; }
  if (pressed_) return;  // act once per press
  pressed_ = true;
  lastActivity_ = now;
  if (now - lastTap_ < 250) return;
  lastTap_ = now;
  if (s.screen_off && (int32_t)(wakeUntil_ - now) <= 0) {  // first tap only wakes the screen
    wakeUntil_ = now + 30000;
    return;
  }
  if (s.screen_off) wakeUntil_ = now + 30000;
  int16_t x, y;
  if (!set_.cal.valid()) return;
  touchMap(set_.cal, kTargets, rx, ry, W, H, x, y);
  if (set_.rot180 != set_.cal_rot) { x = W - 1 - x; y = H - 1 - y; }
  onTap(x, y, now);
}

// ---------------------------------------------------------------------------
// BOOT button: hold 3 s to switch display chip (escape hatch for a blank screen)

static void pollBoot(uint32_t now) {
  static uint32_t downSince = 0;
  if (digitalRead(BOOT_BTN) == LOW) {
    if (!downSince) downSince = now ? now : 1;
    if (now - downSince > 3000 && screen_ != S_CHIP) {
      set_.chip = chip_ == CHIP_ILI9341 ? CHIP_ST7789 : CHIP_ILI9341;
      set_.invert = -1;
      saveSettings();
      Serial.printf("[cyd] display chip set to %s, rebooting\n", set_.chip == CHIP_ILI9341 ? "ILI9341" : "ST7789");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
  } else {
    // A short press on the "can you read this?" screen tries the other chip right away.
    if (downSince && screen_ == S_CHIP && now - downSince > 50) {
      switchChip();
      redraw_ = true;
    }
    downSince = 0;
  }
}

// ---------------------------------------------------------------------------
// Public

void begin(Preferences* prefs) {
  prefs_ = prefs;
  loadSettings();
  buildColors();
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(T_CS, OUTPUT);
  pinMode(T_IRQ, INPUT);
  digitalWrite(T_CS, HIGH);
  pinMode(T_CLK, OUTPUT);
  pinMode(T_MOSI, OUTPUT);
  pinMode(T_MISO, INPUT);
  digitalWrite(T_CLK, LOW);
  // RGB LED off (both boards' red pins, until we know which board this is)
  for (uint8_t p : {(uint8_t)LED_R_2432, (uint8_t)LED_R_32E, (uint8_t)LED_G, (uint8_t)LED_B}) {
    if (p == LED_R_2432 && set_.board == BOARD_32E) continue;  // GPIO 4 is the amp enable there
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  if (set_.board == BOARD_32E) { pinMode(AMP_EN_32E, OUTPUT); digitalWrite(AMP_EN_32E, HIGH); }
  startDisplay();
  Serial.printf("[cyd] display %s (id %06lX, %s), board %s, setup %s\n", chip_ == CHIP_ILI9341 ? "ILI9341" : "ST7789",
                (unsigned long)chipId_, set_.chip != CHIP_AUTO ? "set" : chipGuessed_ ? "no answer, asking" : "detected",
                set_.board == BOARD_32E ? "ESP32-32E" : set_.board == BOARD_2432 ? "ESP32-2432S028" : "not chosen",
                set_.done ? "done" : "needed");
  if (!set_.done || !set_.cal.valid()) {
    fullSetup_ = true;
    if (chipGuessed_) go(S_CHIP);
    else startCalibration();
  } else {
    go(S_DASH);
    drawSplash();
    bootStep("Display", chip_ == CHIP_ILI9341 ? "ILI9341" : "ST7789", 1);
  }
}

void bootStep(const char* what, const char* result, uint8_t ok) {
  if (!splash_ || splashLine_ >= 7) return;
  const int16_t y = 108 + splashLine_ * 14;
  ++splashLine_;
  text(70, y, 1, MUTED, BG, what);
  // dotted leader, then the result: green = ok, amber = not there / attention, grey = info
  for (int16_t x = 70 + (int16_t)strlen(what) * 6 + 4; x < 176; x += 4) tft->drawPixel(x, y + 6, DIM);
  text(182, y, 1, ok == 1 ? OK : ok == 0 ? WARN : TEXT, BG, fit(result, 22));
}

void onDash(const Dash& d, uint32_t now) {
  // Tile taps stay shown as tapped until the master's mask agrees (or 5 s pass).
  if (pendingCats_) {
    const uint16_t settled = (uint16_t)~(dash_.cats ^ d.cats);   // bits the master didn't change
    if (now - pendingCatsMs_ > 5000) pendingCats_ = 0;
    else pendingCats_ &= settled;
  }
  dash_ = d;
  haveDash_ = true;
  dashMs_ = now;
  if (d.alert.seq && d.alert.seq != seenSeq_) {
    seenSeq_ = d.alert.seq;
    if (firstDash_ && d.alert.age_s > 60) ackedSeq_ = d.alert.seq;  // old news at boot
    else alertSignal(d.alert.level, now);
  }
  firstDash_ = false;
}

void onView(const uint8_t* p, size_t n, uint32_t now) {
  const uint8_t v = dashViewOf(p, n);
  if (v != viewWant_) return;  // a screen we've left
  bool ok = false;
  if (v == DV_DEVICES) { DashDevices d; ok = decodeDashDevices(p, n, d) && d.offset == viewOffset_; if (ok) devs_ = d; }
  else if (v == DV_DETAIL) { DashDetail d; ok = decodeDashDetail(p, n, d) && d.id == viewId_; if (ok) detail_ = d; }
  else if (v == DV_EVENTS) { DashEvents d; ok = decodeDashEvents(p, n, d) && d.offset == viewOffset_; if (ok) events_ = d; }
  else if (v == DV_NODES) { DashNodes d; ok = decodeDashNodes(p, n, d) && d.offset == viewOffset_; if (ok) nodes_ = d; }
  if (ok) viewMs_ = now ? now : 1;
}

bool pendingCommand() { return cmdN_ > 0; }
bool wantsUnpair() { return wantUnpair_; }

bool takeCommand(DashCommand& out) {
  if (!cmdN_) return false;
  out = cmdQ_[0];
  for (uint8_t i = 1; i < cmdN_; ++i) cmdQ_[i - 1] = cmdQ_[i];
  --cmdN_;
  return true;
}

void loop(const NodeState& s, uint32_t now) {
  pollBoot(now);
  runSignals(now);
  // Boot screen: up until it has been shown a moment and the master answered (or 6 s)
  if (splash_) {
    static bool said = false;
    if (s.paired && !said) { said = true; char b[24]; snprintf(b, sizeof(b), "joined %04X", s.master_id); bootStep("Cluster", b, 1); splashUntil_ = now + 700; }
    if ((int32_t)(now - splashUntil_) < 0 || (!s.paired && !s.unpaired && now < 6000)) return;
    if (!said && !s.unpaired) bootStep("Cluster", "still looking", 0);
    splash_ = false;
    redraw_ = true;
    forgetParts();
  }
  static uint32_t lastTouch = 0;
  if (now - lastTouch >= 15) { lastTouch = now; pollTouch(s, now); }

  // Identify takes over the screen (even when screens are off)
  if (s.identify) {
    backlight(true);
    identifying_ = true;
    drawIdentify(s, now);
    return;
  }
  if (identifying_) { identifying_ = false; redraw_ = true; forgetParts(); wasDark_ = false; }

  const bool mainScreen = screen_ == S_DASH || screen_ == S_DEVICES || screen_ == S_DETAIL || screen_ == S_EVENTS || screen_ == S_NODES;
  // Keep asking for the open screen's data (a lost request, or the master restarted)
  if (viewWant_ != DV_NONE && s.paired && now - viewAskedMs_ > 4000) askView(viewWant_, viewOffset_, viewId_, now);
  const bool dark = s.screen_off && (int32_t)(wakeUntil_ - now) <= 0 && mainScreen;
  if (dark != wasDark_) {
    wasDark_ = dark;
    backlight(!dark);
    if (!dark) { redraw_ = true; forgetParts(); }
  }
  if (dark) return;

  // A format started from the master's page: show it here too (the card is busy)
  if (s.sd == SD_FORMATTING && screen_ != S_SD && screen_ != S_CAL && screen_ != S_CHIP) go(S_SD);
  // A minute untouched on any other screen (settings, a list, the SD page after a format
  // from the master): back to the dashboard. Not during first-time setup or a format.
  const bool setupScreen = screen_ == S_CAL || screen_ == S_BOARD || screen_ == S_DISPLAY || screen_ == S_CHIP;
  if (screen_ != S_DASH && !setupScreen && s.sd != SD_FORMATTING && (int32_t)(now - lastActivity_) > 60000) go(S_DASH);
  static uint32_t lastDraw = 0;
  if (!redraw_ && now - lastDraw < 200) return;
  lastDraw = now;
  switch (screen_) {
    case S_CHIP: if (redraw_) { redraw_ = false; drawChip(); } break;
    case S_CAL: if (redraw_) { redraw_ = false; drawCal(); } break;
    case S_BOARD: if (redraw_) { redraw_ = false; drawBoard(); } break;
    case S_DISPLAY: if (redraw_) { redraw_ = false; drawDisplayCheck(); } break;
    case S_SETTINGS:
      if (pairConfirmUntil_ && (int32_t)(pairConfirmUntil_ - now) <= 0) { pairConfirmUntil_ = 0; redraw_ = true; }
      // The SD line: a new state right away, the count every few seconds
      if (s.sd != sdShown_ || (s.sd_saved != sdSavedShown_ && now - sdDrawMs_ > 3000)) {
        redraw_ = true;
        sdDrawMs_ = now;
      }
      if (redraw_) { redraw_ = false; drawSettings(s, now); }
      break;
    case S_ALERTS: if (redraw_) { redraw_ = false; drawAlerts(); } break;
    case S_SD:
      if (sdConfirmUntil_ && (int32_t)(sdConfirmUntil_ - now) <= 0) { sdConfirmUntil_ = 0; redraw_ = true; }
      if (s.sd != sdScrShown_ || (s.sd_saved != sdScrSaved_ && now - sdDrawMs_ > 3000)) { redraw_ = true; sdDrawMs_ = now; }
      if (redraw_) { redraw_ = false; drawSd(s, now); }
      break;
    case S_DASH: drawDash(s, now); break;
    case S_DEVICES: case S_DETAIL: case S_EVENTS: case S_NODES: drawView(s, now); break;
  }
}

bool handleCommand(const String& line) {
  if (line == "cyd setup") {
    set_.done = 0;
    fullSetup_ = true;
    saveSettings();
    startCalibration();
    Serial.println("[cyd] setup started on the screen");
    return true;
  }
  if (line.startsWith("cyd driver ")) {
    const String v = line.substring(11);
    set_.chip = v == "ili9341" ? CHIP_ILI9341 : v == "st7789" ? CHIP_ST7789 : CHIP_AUTO;
    set_.invert = -1;
    saveSettings();
    Serial.println("{\"t\":\"ack\",\"cmd\":\"cyd driver\",\"reboot\":true}");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  if (line == "cyd beep") {  // speaker test
    playBeeps(millis());
    Serial.printf("[cyd] beep (board %s, amp pin %s)\n", set_.board == BOARD_32E ? "ESP32-32E" : "ESP32-2432S028",
                  set_.board == BOARD_32E ? "GPIO 4" : "none");
    return true;
  }
  if (line.startsWith("cyd invert ")) {
    const String v = line.substring(11);
    set_.invert = v == "on" ? 1 : v == "off" ? 0 : -1;
    tft->invertDisplay(invertOn());
    saveSettings();
    return true;
  }
  return false;
}

uint8_t batteryPercent() {
  if (set_.board != BOARD_32E) return 255;
  static uint32_t last = 0;
  static uint8_t pct = 255;
  const uint32_t now = millis();
  if (last && now - last < 10000) return pct;
  last = now;
  // Average a few reads; note the spread (a charger with no cell attached wobbles).
  uint32_t sum = 0, lo = 0xFFFFFFFF, hi = 0;
  for (int i = 0; i < 8; ++i) {
    const uint32_t mv = analogReadMilliVolts(BATT_ADC);
    sum += mv; lo = mv < lo ? mv : lo; hi = mv > hi ? mv : hi;
  }
  const float v = sum / 8.0f * 2.0f / 1000.0f;  // 1:2 divider
  const bool wobbly = (hi - lo) * 2 > 150;
  if (v < 2.5f) pct = 255;                       // nothing on the battery pin
  else if (v > 4.08f || wobbly) pct = 254;       // charger output (USB, with or without a cell)
  else {
    const float p = (v - 3.3f) / (4.08f - 3.3f) * 100.0f;
    pct = (uint8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
  }
  return pct;
}

void describe(Print& out) {
  out.printf(",\"cyd\":{\"chip\":\"%s\",\"id\":\"%06lX\",\"board\":\"%s\",\"setup\":%s}",
             chip_ == CHIP_ILI9341 ? "ili9341" : "st7789", (unsigned long)chipId_,
             set_.board == BOARD_32E ? "32e" : set_.board == BOARD_2432 ? "2432s028" : "", set_.done ? "true" : "false");
  out.printf(",\"sd\":\"%s\",\"sd_mb\":%lu,\"sd_saved\":%lu", sdStateName(cydsd::state()), (unsigned long)cydsd::cardMB(),
             (unsigned long)cydsd::saved());
  if (cydsd::formatResult()[0]) {
    out.print(",\"sd_fmt\":\"");
    out.print(cydsd::formatResult());
    out.printf("\",\"sd_fmt_ok\":%s", cydsd::formatOk() ? "true" : "false");
  }
}

#ifdef SS_CYD_PREVIEW
// Host preview (renders the screens to images without a board)
namespace preview {
void setup(uint8_t board, bool done) {
  set_.board = board;
  set_.done = done;
  const int16_t raw[3][2] = {{400, 500}, {3600, 520}, {410, 3500}};
  touchCalibrate(raw, set_.cal);
  saveSettings();
}
void go(int s) { go((Screen)s); forgetParts(); }
void sdConfirm(uint32_t until) { sdConfirmUntil_ = until; redraw_ = true; }
Adafruit_SPITFT* display() { return tft; }
void tap(int16_t x, int16_t y, uint32_t now) { onTap(x, y, now); }
void calStep(uint8_t step, bool failed) { calStep_ = step; calFailed_ = failed; redraw_ = true; }
void night(bool on) { set_.night = on; buildColors(); redraw_ = true; forgetParts(); }
}  // namespace preview
#endif

}  // namespace cyd
}  // namespace ss
#endif  // SS_CYD
