#ifdef SS_DISPLAY_TEMBED
// T-Embed CC1101 screen: 320 x 170 landscape, three pages turned with the jog wheel.
//   HOME     cluster link, what this node scans, battery
//   SUB-GHZ  each band: level now vs its noise floor, bursts heard, JAMMED
//   RECENT   the latest bursts: when, which band, how strong
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#include "node_display.h"
#include "subghz_node.h"
#include "tembed_hw.h"

namespace ss {
namespace display {

static Adafruit_ST7789 tft(&SPI, tembed::TFT_CS, tembed::TFT_DC, tembed::TFT_RST);
static bool ok_ = false, on_ = true;
static uint8_t page_ = 0, shown_ = 255;
static constexpr uint8_t kPages = 3;
static Info info_;
static bool haveInfo_ = false;
static volatile int16_t encCount_ = 0;
static volatile uint8_t encState_ = 0;

static constexpr uint16_t BG = 0x0883, PANEL = 0x10E6, TEXT = 0xDF5E, MUTED = 0x8D37, ACCENT = 0x4E5D, WARN = 0xF547,
                          CRIT = 0xFB4B, OK = 0x4E8B;
static constexpr int W = 320, H = 170;

static void IRAM_ATTR onEncoder() {
  // Quadrature: the table gives -1 / 0 / +1 per transition.
  static const int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t s = (uint8_t)((digitalRead(tembed::ENC_A) << 1) | digitalRead(tembed::ENC_B));
  encState_ = (uint8_t)(((encState_ << 2) | s) & 0x0F);
  encCount_ = (int16_t)(encCount_ + kStep[encState_]);
}

bool begin() {
  tembed::begin();
  pinMode(tembed::TFT_BL, OUTPUT);
  digitalWrite(tembed::TFT_BL, HIGH);
  {
    tembed::SpiLock l;
    tft.init(170, 320);
    tft.setRotation(3);
    tft.invertDisplay(true);
    tft.fillScreen(BG);
  }
  pinMode(tembed::ENC_A, INPUT_PULLUP);
  pinMode(tembed::ENC_B, INPUT_PULLUP);
  pinMode(tembed::ENC_KEY, INPUT_PULLUP);
  pinMode(tembed::BACK_KEY, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(tembed::ENC_A), onEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(tembed::ENC_B), onEncoder, CHANGE);
  ok_ = true;
  return true;
}

void setOn(bool on) {
  on_ = on;
  digitalWrite(tembed::TFT_BL, on ? HIGH : LOW);
  if (on) shown_ = 255;   // redraw everything
}
bool isOn() { return on_; }
uint8_t batteryPercent() { return tembed::batteryPercent(); }

// --- drawing helpers (caller holds the SPI lock) ---
static void text(int x, int y, uint8_t size, uint16_t fg, const char* s) {
  tft.setTextSize(size);
  tft.setTextColor(fg, BG);
  tft.setCursor(x, y);
  tft.print(s);
}
static void row(int y, int h, uint8_t size, uint16_t fg, const char* s) {
  tft.fillRect(0, y, W, h, BG);
  text(8, y + 2, size, fg, s);
}

static void header() {
  const Info& i = info_;
  tft.fillRect(0, 0, W, 22, PANEL);
  tft.setTextSize(2);
  tft.setTextColor(ACCENT, PANEL);
  tft.setCursor(6, 4);
  tft.print(i.label && i.label[0] ? i.label : "SKYSENTINEL");
  char b[24];
  if (i.battery_pct <= 100) snprintf(b, sizeof(b), "%u%%", i.battery_pct);
  else snprintf(b, sizeof(b), "USB");
  tft.setTextColor(i.battery_pct <= 15 ? CRIT : TEXT, PANEL);
  tft.setCursor(W - 6 - (int)strlen(b) * 12, 4);
  tft.print(b);
  // page dots
  for (uint8_t p = 0; p < kPages; ++p) tft.fillCircle(W / 2 - 12 + p * 12, 11, 3, p == page_ ? ACCENT : MUTED);
}

static void pageHome() {
  const Info& i = info_;
  char b[64];
  const uint32_t now = millis();
  if (i.role && strcmp(i.role, "NODE") == 0) {
    if (i.paired) snprintf(b, sizeof(b), "Master %04X  %d dBm", i.master_id, i.link_rssi);
    else snprintf(b, sizeof(b), "Looking for master...");
    row(30, 22, 2, i.paired ? OK : WARN, b);
  } else {
    snprintf(b, sizeof(b), "%s  %u nodes", i.role, i.nodes_online);
    row(30, 22, 2, ACCENT, b);
  }
  snprintf(b, sizeof(b), "BLE %u%%  Wi-Fi %u ch  sent %lu", i.ble_duty, i.wifi_channels, (unsigned long)i.sent);
  row(56, 14, 1, TEXT, b);
  const SubGhzWatch& w = subghz::watch();
  uint32_t total = 0;
  bool jam = false;
  for (uint8_t k = 0; k < kSubGhzBands; ++k) { total += w.bursts(k); jam = jam || w.jammed(k, now); }
  snprintf(b, sizeof(b), "SUB-GHZ  %s", !subghz::ready() ? "radio not found" : !subghz::running() ? "off (category)" : jam ? "JAMMING" : "watching");
  row(78, 22, 2, !subghz::ready() ? CRIT : jam ? CRIT : subghz::running() ? ACCENT : MUTED, b);
  snprintf(b, sizeof(b), "%lu bursts  %lu sweeps", (unsigned long)total, (unsigned long)subghz::sweeps());
  row(104, 14, 1, TEXT, b);
  const uint32_t up = i.uptime_s;
  snprintf(b, sizeof(b), "up %luh%02lum   turn the wheel for bands", (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  row(150, 14, 1, MUTED, b);
}

static void pageBands() {
  const SubGhzWatch& w = subghz::watch();
  const uint32_t now = millis();
  char b[48];
  row(28, 12, 1, MUTED, "BAND      LEVEL (floor . now . peak)        BURSTS");
  for (uint8_t k = 0; k < kSubGhzBands; ++k) {
    const int y = 44 + k * 25;
    tft.fillRect(0, y, W, 24, BG);
    const bool jam = w.jammed(k, now), cur = subghz::currentBand() == k;
    text(8, y + 4, 2, jam ? CRIT : cur ? ACCENT : TEXT, kSubGhzBandList[k].name);
    // bar from -120 to -30 dBm
    const int bx = 112, bw = 140;
    auto px = [&](int dbm) { int v = (dbm + 120) * bw / 90; return bx + (v < 0 ? 0 : v > bw ? bw : v); };
    tft.drawRect(bx, y + 6, bw, 12, MUTED);
    const int8_t fl = w.floorDbm(k), now_ = w.lastDbm(k), pk = w.peakDbm(k);
    if (now_ > -127) tft.fillRect(bx + 1, y + 7, px(now_) - bx, 10, jam ? CRIT : ACCENT);
    if (fl > -127) tft.drawFastVLine(px(fl), y + 3, 18, OK);
    if (pk > -127) tft.drawFastVLine(px(pk), y + 3, 18, WARN);
    snprintf(b, sizeof(b), jam ? "JAM" : "%lu", (unsigned long)w.bursts(k));
    text(262, y + 4, 2, jam ? CRIT : TEXT, b);
  }
}

static void pageRecent() {
  const SubGhzWatch& w = subghz::watch();
  SubGhzBurst r[6];
  const uint8_t n = w.recent(r, 6);
  const uint32_t now = millis();
  row(28, 12, 1, MUTED, "LATEST BURSTS (fob presses, sensors, remotes)");
  char b[64];
  for (uint8_t k = 0; k < 6; ++k) {
    const int y = 44 + k * 20;
    if (k >= n) { tft.fillRect(0, y, W, 20, BG); continue; }
    const uint32_t ago = (now - r[k].ms) / 1000;
    snprintf(b, sizeof(b), "%4lus  %-7s %4d dBm  %s", (unsigned long)ago, kSubGhzBandList[r[k].band].name, r[k].peak,
             kSubGhzBandList[r[k].band].what);
    row(y, 20, 1, k == 0 && ago < 5 ? WARN : TEXT, b);
  }
  if (!n) row(60, 20, 1, MUTED, "Nothing yet. Press a car fob or garage remote nearby.");
}

static void drawAll(bool full) {
  if (!ok_ || !on_ || !haveInfo_) return;
  tembed::SpiLock l;
  const Info& i = info_;
  if (i.identify) {
    const bool flip = (millis() / 250) % 2;
    tft.fillScreen(flip ? ACCENT : BG);
    tft.setTextSize(4);
    tft.setTextColor(flip ? BG : ACCENT);
    tft.setCursor(60, 60);
    tft.print("THIS ONE");
    shown_ = 255;
    return;
  }
  if (i.pair) {
    char b[48];
    tft.fillScreen(BG);
    if (i.pair == 2) {
      snprintf(b, sizeof(b), "%06lu", (unsigned long)i.pair_code);
      text(20, 20, 2, TEXT, "Pair with master?");
      text(40, 60, 5, ACCENT, b);
      text(20, 130, 1, MUTED, "Check the code matches, then ACCEPT on the master.");
    } else {
      text(20, 60, 2, WARN, i.pair == 3 ? "Pairing open" : "Waiting to be paired");
      text(20, 100, 1, MUTED, "Press PAIR A NODE on the master and hold this close.");
    }
    shown_ = 255;
    return;
  }
  if (full || shown_ != page_) { tft.fillScreen(BG); shown_ = page_; }
  header();
  if (page_ == 0) pageHome();
  else if (page_ == 1) pageBands();
  else pageRecent();
}

void update(const Info& i) {
  info_ = i;
  haveInfo_ = true;
  // LED: red while a band is jammed
  bool jam = false;
  for (uint8_t k = 0; k < kSubGhzBands; ++k) jam = jam || subghz::watch().jammed(k, millis());
  rgbLedWrite(tembed::LED_PIN, jam ? 40 : 0, 0, 0);
  drawAll(false);
}

void tick(uint32_t now) {
  static int16_t last = 0;
  static uint32_t lastDraw = 0, keyAt = 0, backAt = 0;
  static bool keyWas = false, backWas = false;
  const int16_t c = encCount_;
  const int16_t steps = (int16_t)(c - last) / 4;   // one detent = 4 transitions
  if (steps) {
    last = (int16_t)(last + steps * 4);
    page_ = (uint8_t)((page_ + kPages + (steps % kPages)) % kPages);
    if (on_) drawAll(true);
    lastDraw = now;
  }
  const bool key = digitalRead(tembed::ENC_KEY) == LOW, back = digitalRead(tembed::BACK_KEY) == LOW;
  if (key && !keyWas && now - keyAt > 200) { keyAt = now; page_ = (uint8_t)((page_ + 1) % kPages); if (on_) drawAll(true); }
  if (back && !backWas && now - backAt > 300) { backAt = now; setOn(!on_); if (on_) drawAll(true); }
  keyWas = key;
  backWas = back;
  // live pages (bands) refresh 4x a second
  if (on_ && page_ == 1 && now - lastDraw > 250) { lastDraw = now; drawAll(false); }
}

}  // namespace display
}  // namespace ss
#endif
