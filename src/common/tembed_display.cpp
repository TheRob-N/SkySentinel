#ifdef SS_DISPLAY_TEMBED
// T-Embed CC1101 screen: 320 x 170 landscape, six pages turned with the jog wheel.
//   HOME     cluster link, what this node scans, battery
//   ALERTS   the cluster's latest alert, what's of concern nearby, recent events (from the master)
//   BANDS    each sub-GHz band: level now vs its noise floor, bursts heard, JAMMED
//   HISTORY  the last 10 minutes of each band (10 s per bar; burst slots in color)
//   RECENT   the latest bursts: when, which band, how strong
//   FOXHUNT  pick a sub-GHz band or a Bluetooth device this board hears, then walk toward it
// Controls:
//   turn the wheel     next / previous page (in a menu or list: move)
//   press the wheel    next page (FOXHUNT page: pick a target; in a menu: change / do it)
//   hold the wheel 1 s SETTINGS menu
//   press BACK         screen off / on (in a menu, a list or a hunt: back out)
//   hold BACK 2 s      POWER menu
// The LED ring around the wheel is the alert light (a color per alert, set in SETTINGS),
// and the signal meter while hunting. A NIGHT theme turns everything red on black and
// dims the screen and the ring.
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ST7789.h>
#include <Preferences.h>
#include <SPI.h>

#include "cyd_art.h"   // the SkySentinel logo, as on the CYD and the web console
#include "node_display.h"
#include "ss_classify.h"
#include "subghz_node.h"
#include "tembed_hw.h"

namespace ss {
namespace display {

static Adafruit_ST7789 tft(&SPI, tembed::TFT_CS, tembed::TFT_DC, tembed::TFT_RST);
static Adafruit_NeoPixel ring(8, tembed::LED_PIN, NEO_GRB + NEO_KHZ800);
static bool ok_ = false, on_ = true;
static uint8_t page_ = 0, shown_ = 255;
enum : uint8_t { P_HOME, P_ALERTS, P_BANDS, P_HISTORY, P_RECENT, P_HUNT, kPages };
static Info info_;
static bool haveInfo_ = false;
static volatile int16_t encCount_ = 0;
static volatile uint8_t encState_ = 0;

static constexpr int W = 320, H = 170;

// ---------------------------------------------------------------------------
// Settings, saved on the board (Preferences "ssdisp"). New ones go at the end: an older,
// shorter save keeps their defaults.
struct Color { const char* name; uint8_t r, g, b; };
static const Color kColors[] = {
    {"Red", 255, 0, 0},    {"Orange", 255, 70, 0}, {"Yellow", 255, 170, 0}, {"Green", 0, 255, 0},  {"Cyan", 0, 200, 255},
    {"Blue", 0, 0, 255},   {"Purple", 150, 0, 255}, {"White", 255, 255, 255}, {"Off", 0, 0, 0},
};
static constexpr uint8_t kColorCount = sizeof(kColors) / sizeof(kColors[0]);
static constexpr uint8_t kColorOff = kColorCount - 1;

struct Settings {
  uint8_t rot = 3;          // 3: wheel on the left, 1: turned around
  uint8_t bright = 3;       // day backlight 25 / 50 / 75 / 100 %
  uint8_t timeout = 0;      // screen off after: never / 1 / 5 / 15 min
  uint8_t start = 0;        // page at start (P_*)
  uint8_t led = 1;          // LED ring on
  uint8_t ledBright = 1;    // low / medium / high
  uint8_t colJam = 0;       // Red
  uint8_t colBurst = 5;     // Blue (a short flash)
  uint8_t colLost = 2;      // Yellow (a slow blink while looking for the master)
  uint8_t sens = 4;         // burst louder than 10 / 15 / 20 / 25 / 30 dB over the floor (30: quiet on a desk, Nov 10)
  uint8_t bands = 0x3F;     // bit per band (kSubGhzBandList numbers)
  uint8_t jamAlert = 1;     // send jamming to the master
  uint8_t len = 2;          // a burst lasts at least: 1 / 3 / 6 / 8 / 12 samples (2.5 ms each); 15 ms
  uint8_t ver = 3;          // 2: six bands (314.35 added); 3: six pages, theme, cluster alerts
  uint8_t night = 0;        // NIGHT theme: red on black, dimmer screen and ring
  uint8_t brightNight = 2;  // night backlight 5 / 10 / 25 / 50 %
  uint8_t colAlert = 6;     // Purple: a new alert from the cluster
  uint8_t alertFlash = 1;   // how long that pulses: 10 s / 30 s / 1 min / 5 min / until a button
} static set_;

static const uint8_t kBrightPwm[] = {64, 128, 192, 255};
static const uint8_t kNightPct[] = {5, 10, 25, 50};
static const uint8_t kNightPwm[] = {13, 26, 64, 128};
static const uint32_t kTimeoutMs[] = {0, 60000, 300000, 900000};
static const char* const kTimeoutNames[] = {"Never", "1 min", "5 min", "15 min"};
static const char* const kPageNames[kPages] = {"HOME", "ALERTS", "BANDS", "HISTORY", "RECENT", "FOXHUNT"};
static const uint8_t kLedLevel[] = {12, 40, 110};   // out of 255
// How long the ring pulses for a new cluster alert; 0 = until a button is pressed. Any
// button stops it sooner (the alert is seen).
static const uint32_t kAlertFlashMs[] = {10000, 30000, 60000, 300000, 0};
static const char* const kAlertFlashNames[] = {"10 s", "30 s", "1 min", "5 min", "Until a button"};
static const char* const kLevelNames[] = {"Low", "Medium", "High"};
// How loud a burst must be (dB over the band's quiet level) and how long (loud samples
// in one 100 ms look of 40). Louder and longer = fewer false bursts, but weaker or
// shorter real ones are missed.
static const int8_t kSensDb[] = {10, 15, 20, 25, 30};
static const char* const kSensNames[] = {"10 dB (most)", "15 dB", "20 dB", "25 dB", "30 dB (fewest)"};
static const uint8_t kLenSamples[] = {1, 3, 6, 8, 12};
static const char* const kLenNames[] = {"2.5 ms (most)", "7.5 ms", "15 ms", "20 ms", "30 ms (fewest)"};

static void saveSettings() {
  Preferences p;
  p.begin("ssdisp", false);
  p.putBytes("set", &set_, sizeof(set_));
  p.putUChar("rot", set_.rot);
  p.end();
}
static void loadSettings() {
  Preferences p;
  p.begin("ssdisp", true);
  Settings s;
  const size_t n = p.getBytesLength("set");
  if (n > 0 && n <= sizeof(s)) {
    s.ver = 0;   // a save too short to hold it is from before the version byte
    p.getBytes("set", &s, n);
    if (s.ver < 2) s.bands |= 1u << 5;   // saved before the 314.35 band: watch it too
    if (s.ver < 3) {                      // three pages then (HOME, BANDS, RECENT), six now
      static const uint8_t kOld[] = {P_HOME, P_BANDS, P_RECENT};
      s.start = s.start < 3 ? kOld[s.start] : (uint8_t)P_HOME;
    }
  } else {
    s.rot = p.getUChar("rot", 3);   // only the orientation was saved before
  }
  p.end();
  // Keep everything in range, whatever was stored.
  s.rot = s.rot == 1 ? 1 : 3;
  if (s.bright > 3) s.bright = 3;
  if (s.timeout > 3) s.timeout = 0;
  if (s.start >= kPages) s.start = P_HOME;
  s.led = s.led ? 1 : 0;
  if (s.ledBright > 2) s.ledBright = 1;
  if (s.colJam >= kColorCount) s.colJam = 0;
  if (s.colBurst >= kColorCount) s.colBurst = 5;
  if (s.colLost >= kColorCount) s.colLost = 2;
  if (s.colAlert >= kColorCount) s.colAlert = 6;
  if (s.sens > 4) s.sens = 4;
  if (s.len > 4) s.len = 2;
  s.bands &= (uint8_t)((1u << kSubGhzBands) - 1);
  s.jamAlert = s.jamAlert ? 1 : 0;
  s.night = s.night ? 1 : 0;
  if (s.brightNight > 3) s.brightNight = 2;
  if (s.alertFlash > 4) s.alertFlash = 1;
  s.ver = 3;
  set_ = s;
}

// ---------------------------------------------------------------------------
// Colors: DAY (blue, as the web console) or NIGHT (red on black, keeps night vision; the
// same as the CYD's and the web page's NIGHT themes).
static uint16_t BG, PANEL, TEXT, MUTED, ACCENT, WARN, CRIT, OK;
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static void applyTheme() {
  if (set_.night) {
    BG = rgb(0, 0, 0); PANEL = rgb(28, 4, 4); TEXT = rgb(255, 80, 60); MUTED = rgb(170, 45, 35);
    ACCENT = rgb(255, 60, 40); WARN = rgb(255, 120, 40); CRIT = rgb(255, 30, 20); OK = rgb(220, 70, 50);
  } else {
    BG = 0x0883; PANEL = 0x10E6; TEXT = 0xDF5E; MUTED = 0x8D37; ACCENT = 0x4E5D; WARN = 0xF547; CRIT = 0xFB4B; OK = 0x4E8B;
  }
}

// ---------------------------------------------------------------------------
// Flicker-free drawing: each strip of the screen (a header, a text row, a band) is drawn
// in memory first and sent to the screen in one go, so the screen never shows a strip
// cleared to the background before its text arrives. One strip buffer (320 x 26 x 2 bytes,
// about 16 KB) is reused for every strip.
static constexpr int kStripH = 26;
static GFXcanvas16* strip_ = nullptr;
static int stripY_ = 0, stripH_ = 0;
static bool inStrip_ = false;
// The canvas being drawn into: the strip while one is open, else the screen directly
// (full-screen pages: pairing, IDENTIFY).
static Adafruit_GFX& g() { return inStrip_ ? (Adafruit_GFX&)*strip_ : (Adafruit_GFX&)tft; }
static int oy() { return inStrip_ ? stripY_ : 0; }   // strip coordinates: subtract its top
static void stripBegin(int y, int h, uint16_t bg) {
  stripY_ = y; stripH_ = h > kStripH ? kStripH : h;
  inStrip_ = strip_ != nullptr;
  if (inStrip_) strip_->fillRect(0, 0, W, kStripH, bg);
  else tft.fillRect(0, y, W, h, bg);
}
static void stripEnd() {
  if (inStrip_) tft.drawRGBBitmap(0, stripY_, strip_->getBuffer(), W, stripH_);
  inStrip_ = false;
}

// Settings the sub-GHz watch uses (it may start after the screen).
static void applyRadioSettings() {
  subghz::setBandMask(set_.bands);
  subghz::setBurstDb(kSensDb[set_.sens]);
  subghz::setBurstSamples(kLenSamples[set_.len]);
  subghz::setJamAlerts(set_.jamAlert);
}
static void backlight(bool on) {
  analogWrite(tembed::TFT_BL, !on ? 0 : set_.night ? kNightPwm[set_.brightNight] : kBrightPwm[set_.bright]);
}

static void IRAM_ATTR onEncoder() {
  // Quadrature: the table gives -1 / 0 / +1 per transition.
  static const int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t s = (uint8_t)((digitalRead(tembed::ENC_A) << 1) | digitalRead(tembed::ENC_B));
  encState_ = (uint8_t)(((encState_ << 2) | s) & 0x0F);
  encCount_ = (int16_t)(encCount_ + kStep[encState_]);
}

bool begin() {
  tembed::begin();
  loadSettings();
  applyTheme();
  applyRadioSettings();
  page_ = set_.start;
  pinMode(tembed::TFT_BL, OUTPUT);
  backlight(true);
  {
    tembed::SpiLock l;
    tft.init(170, 320);
    tft.setRotation(set_.rot);
    tft.invertDisplay(true);
    tft.fillScreen(BG);
  }
  pinMode(tembed::ENC_A, INPUT_PULLUP);
  pinMode(tembed::ENC_B, INPUT_PULLUP);
  pinMode(tembed::ENC_KEY, INPUT_PULLUP);
  pinMode(tembed::BACK_KEY, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(tembed::ENC_A), onEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(tembed::ENC_B), onEncoder, CHANGE);
  strip_ = new (std::nothrow) GFXcanvas16(W, kStripH);
  if (strip_ && !strip_->getBuffer()) { delete strip_; strip_ = nullptr; }   // no memory: draw directly
  ring.begin();
  ring.clear();
  ring.show();
  ok_ = true;
  return true;
}

void flip() {
  set_.rot = set_.rot == 3 ? 1 : 3;
  saveSettings();
  if (!ok_) return;
  {
    tembed::SpiLock l;
    tft.setRotation(set_.rot);
    tft.fillScreen(BG);
  }
  shown_ = 255;
}

void setOn(bool on) {
  on_ = on;
  backlight(on);
  if (ok_) { tembed::SpiLock l; tft.enableSleep(!on); }   // panel asleep too (less power), as Bruce does
  if (on) shown_ = 255;   // redraw everything
}
bool isOn() { return on_; }
uint8_t batteryPercent() { return tembed::batteryPercent(); }

// ---------------------------------------------------------------------------
// ALERTS: what the master sends (HBW_ALERTS in the heartbeat asks for it).
static Dash dash_;
static DashEvents events_;
static uint32_t dashMs_ = 0, eventsMs_ = 0;
static uint16_t alertSeqSeen_ = 0;
static bool alertSeqKnown_ = false;
static uint32_t alertLedUntil_ = 0;   // the LED pulses the cluster-alert color until then
static bool alertLedHeld_ = false;    // ... or until a button is pressed (Alert flash: Until a button)

void onDash(const Dash& d) {
  const uint32_t now = millis();
  // A new alert (not one that was already showing when this board joined) lights the ring.
  if (alertSeqKnown_ && d.alert.seq && d.alert.seq != alertSeqSeen_ && d.alert.level >= 2 && d.alert.age_s < 60) {
    const uint32_t ms = kAlertFlashMs[set_.alertFlash];
    alertLedHeld_ = ms == 0;
    alertLedUntil_ = now + (ms ? ms : 1);
  }
  alertSeqSeen_ = d.alert.seq;
  alertSeqKnown_ = true;
  dash_ = d;
  dashMs_ = now ? now : 1;
}
void onEvents(const DashEvents& e) {
  events_ = e;
  eventsMs_ = millis() | 1;
}

// ---------------------------------------------------------------------------
// FOXHUNT. Targets: the sub-GHz bands, and Bluetooth devices this board hears itself (the
// signal strength that matters for walking toward something is the one in your hand).
struct Seen {
  uint8_t mac[6];
  uint8_t cat;
  uint16_t sig;
  int8_t rssi;
  uint32_t ms;
  char name[21];
};
static constexpr uint8_t kSeen = 16;
static Seen seen_[kSeen];
static portMUX_TYPE seenMux_ = portMUX_INITIALIZER_UNLOCKED;
static constexpr uint32_t kSeenMs = 60000;   // listed while heard in the last minute

struct Target {
  uint8_t kind;   // 0 sub-GHz band, 1 Bluetooth device
  uint8_t band;
  uint8_t mac[6];
  uint8_t cat;
  int8_t rssi;
  char text[25];
};
static constexpr uint8_t kTargets = kSubGhzBands + kSeen;
static Target targets_[kTargets];
static uint8_t nTargets_ = 0;
static Target hunt_;                    // what's being hunted
static volatile int8_t huntRssi_ = -127;   // Bluetooth: the latest advert's strength
static volatile uint32_t huntMs_ = 0;
static volatile bool huntBleOn_ = false;
static int8_t huntHold_ = -127;         // the strongest in the last 3 s (peak hold)
static uint32_t huntHoldMs_ = 0;

void onSighting(const Sighting& s, const char* name) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&seenMux_);
  if (huntBleOn_ && memcmp(s.addr, hunt_.mac, 6) == 0) { huntRssi_ = s.rssi_last; huntMs_ = now ? now : 1; }
  int slot = -1, oldest = 0;
  for (int i = 0; i < kSeen; ++i) {
    if (seen_[i].ms && memcmp(seen_[i].mac, s.addr, 6) == 0) { slot = i; break; }
    if (seen_[i].ms < seen_[oldest].ms) oldest = i;
  }
  if (slot < 0) { slot = oldest; memcpy(seen_[slot].mac, s.addr, 6); seen_[slot].name[0] = 0; }
  Seen& e = seen_[slot];
  e.cat = s.category;
  e.sig = s.sig_id;
  e.rssi = s.rssi_last;
  e.ms = now ? now : 1;
  if (name && name[0]) { strncpy(e.name, name, sizeof(e.name) - 1); e.name[sizeof(e.name) - 1] = 0; }
  portEXIT_CRITICAL(&seenMux_);
}
bool huntBle() { return huntBleOn_; }

// What to call a Bluetooth device: its name, else what it was recognized as, plus the end
// of its address (two of the same kind stay apart).
static void seenText(const Seen& e, char* out, size_t n) {
  const char* what = e.name[0] ? e.name : (e.sig ? sigName(e.sig) : categoryLabel(e.cat));
  snprintf(out, n, "%.18s %02X%02X", what, e.mac[4], e.mac[5]);
}

// The list to pick from: the bands (lowest frequency first), then the Bluetooth devices
// heard in the last minute, surveillance and trackers first, then the strongest.
static void buildTargets() {
  nTargets_ = 0;
  for (uint8_t i = 0; i < kSubGhzBands; ++i) {
    Target& t = targets_[nTargets_++];
    t.kind = 0;
    t.band = kSubGhzBandOrder[i];
    t.cat = CAT_SUBGHZ;
    t.rssi = subghz::watch().peakDbm(t.band);
    snprintf(t.text, sizeof(t.text), "%s sub-GHz", kSubGhzBandList[t.band].name);
  }
  Seen copy[kSeen];
  portENTER_CRITICAL(&seenMux_);
  memcpy(copy, seen_, sizeof(copy));
  portEXIT_CRITICAL(&seenMux_);
  const uint32_t now = millis();
  auto rank = [](const Seen& e) { return (kSurveillanceMask & catBit(e.cat)) ? 1000 + e.rssi : e.rssi; };
  bool used[kSeen] = {false};
  for (;;) {
    int best = -1;
    for (int i = 0; i < kSeen; ++i)
      if (!used[i] && copy[i].ms && now - copy[i].ms < kSeenMs && (best < 0 || rank(copy[i]) > rank(copy[best]))) best = i;
    if (best < 0 || nTargets_ >= kTargets) break;
    used[best] = true;
    Target& t = targets_[nTargets_++];
    t.kind = 1;
    t.band = 0;
    memcpy(t.mac, copy[best].mac, 6);
    t.cat = copy[best].cat;
    t.rssi = copy[best].rssi;
    seenText(copy[best], t.text, sizeof(t.text));
  }
}

static void huntStart(const Target& t) {
  hunt_ = t;
  huntHold_ = -127;
  huntHoldMs_ = 0;
  huntRssi_ = -127;
  huntMs_ = 0;
  if (t.kind == 0) subghz::setHunt(t.band);
  else huntBleOn_ = true;   // main raises Bluetooth to full-time (huntBle())
}
static void huntStop() {
  subghz::setHunt(0xFF);
  huntBleOn_ = false;
}
// The live reading: dBm, or -127 when nothing recent (sub-GHz: a look in the last second;
// Bluetooth: an advert in the last 5 s).
static int8_t huntNow(uint32_t now, uint32_t* ageMs) {
  uint32_t at = 0;
  int8_t v = -127;
  if (hunt_.kind == 0) v = subghz::huntPeak(&at);
  else { v = huntRssi_; at = huntMs_; }
  *ageMs = at ? now - at : 0xFFFFFFFF;
  if (!at || *ageMs > (hunt_.kind == 0 ? 1000u : 5000u)) return -127;
  return v;
}

// ---------------------------------------------------------------------------
// Menus. POWER: hold BACK 2 s (Bruce's choices). SETTINGS: hold the wheel 1 s. FOXHUNT
// list: press the wheel on the FOXHUNT page. Turn the wheel to choose, press it to change /
// do it, BACK backs out.
enum : uint8_t { M_NONE, M_POWER, M_SETTINGS, M_HUNTPICK, M_HUNTING };
enum : uint8_t { PM_SCREEN_OFF, PM_DEEP_SLEEP, PM_POWER_OFF, PM_RESTART, PM_CANCEL, PM_COUNT };
static const char* const kPowerItems[PM_COUNT] = {"Screen off", "Deep sleep", "Power off", "Restart", "Cancel"};
static const char* const kPowerHelp[PM_COUNT] = {"Keeps scanning. BACK: on", "Radios off. BACK wakes it",
                                                 "USB turns it on again", "", ""};
enum : uint8_t {
  SI_THEME, SI_BRIGHT, SI_TIMEOUT, SI_START, SI_FLIP, SI_LED, SI_LED_BRIGHT, SI_COL_JAM, SI_COL_BURST, SI_COL_LOST,
  SI_COL_ALERT, SI_ALERT_FLASH, SI_SENS, SI_LEN, SI_BAND0, SI_JAM_ALERT = SI_BAND0 + kSubGhzBands, SI_DONE, SI_COUNT
};
static uint8_t menu_ = M_NONE;
static uint8_t menuSel_ = 0, menuTop_ = 0;
static uint8_t confirm_ = 0;   // power menu: 0 list, 1 "power off?", 2 "can't: on USB"
static uint32_t ledTestUntil_ = 0;
static uint8_t ledTestColor_ = 0;

static uint8_t menuCount() {
  switch (menu_) {
    case M_POWER: return PM_COUNT;
    case M_SETTINGS: return SI_COUNT;
    case M_HUNTPICK: return nTargets_ ? nTargets_ : 1;
    default: return 1;
  }
}

// One settings line: its label and current value (a color line also gets a swatch).
static void settingText(uint8_t k, char* label, size_t ln, char* value, size_t vn, int& swatch) {
  swatch = -1;
  auto pct = [](uint8_t v) { return (unsigned)(25 * (v + 1)); };
  switch (k) {
    case SI_THEME: snprintf(label, ln, "Theme"); snprintf(value, vn, "%s", set_.night ? "Night (red)" : "Day"); break;
    case SI_BRIGHT:
      snprintf(label, ln, "Brightness");
      if (set_.night) snprintf(value, vn, "%u%% night", kNightPct[set_.brightNight]);
      else snprintf(value, vn, "%u%%", pct(set_.bright));
      break;
    case SI_TIMEOUT: snprintf(label, ln, "Screen off after"); snprintf(value, vn, "%s", kTimeoutNames[set_.timeout]); break;
    case SI_START: snprintf(label, ln, "Start page"); snprintf(value, vn, "%s", kPageNames[set_.start]); break;
    case SI_FLIP: snprintf(label, ln, "Turn screen around"); snprintf(value, vn, "wheel %s", set_.rot == 3 ? "left" : "right"); break;
    case SI_LED: snprintf(label, ln, "LED ring alerts"); snprintf(value, vn, "%s", set_.led ? "On" : "Off"); break;
    case SI_LED_BRIGHT: snprintf(label, ln, "LED brightness"); snprintf(value, vn, "%s", kLevelNames[set_.ledBright]); break;
    case SI_COL_JAM: snprintf(label, ln, "Jamming"); snprintf(value, vn, "%s", kColors[set_.colJam].name); swatch = set_.colJam; break;
    case SI_COL_BURST: snprintf(label, ln, "Sub-GHz burst"); snprintf(value, vn, "%s", kColors[set_.colBurst].name); swatch = set_.colBurst; break;
    case SI_COL_LOST: snprintf(label, ln, "Lost master"); snprintf(value, vn, "%s", kColors[set_.colLost].name); swatch = set_.colLost; break;
    case SI_COL_ALERT: snprintf(label, ln, "Cluster alert"); snprintf(value, vn, "%s", kColors[set_.colAlert].name); swatch = set_.colAlert; break;
    case SI_ALERT_FLASH: snprintf(label, ln, "Alert flash"); snprintf(value, vn, "%s", kAlertFlashNames[set_.alertFlash]); break;
    case SI_SENS: snprintf(label, ln, "Burst louder than"); snprintf(value, vn, "%s", kSensNames[set_.sens]); break;
    case SI_LEN: snprintf(label, ln, "Burst longer than"); snprintf(value, vn, "%s", kLenNames[set_.len]); break;
    case SI_JAM_ALERT: snprintf(label, ln, "Jamming to master"); snprintf(value, vn, "%s", set_.jamAlert ? "On" : "Off"); break;
    case SI_DONE: snprintf(label, ln, "Done"); value[0] = 0; break;
    default:
      if (k >= SI_BAND0 && k < SI_BAND0 + kSubGhzBands) {
        const uint8_t bn = kSubGhzBandOrder[k - SI_BAND0];
        snprintf(label, ln, "Watch %s", kSubGhzBandList[bn].name);
        snprintf(value, vn, "%s", set_.bands & (1u << bn) ? "On" : "Off");
      } else {
        label[0] = value[0] = 0;
      }
  }
}

// Press on a settings line: step to the next value, save, apply.
static void settingNext(uint8_t k) {
  switch (k) {
    case SI_THEME:
      set_.night ^= 1;
      applyTheme();
      backlight(true);
      shown_ = 255;   // every color changes: draw it all again
      break;
    case SI_BRIGHT:
      if (set_.night) set_.brightNight = (uint8_t)((set_.brightNight + 1) % 4);
      else set_.bright = (uint8_t)((set_.bright + 1) % 4);
      backlight(true);
      break;
    case SI_TIMEOUT: set_.timeout = (uint8_t)((set_.timeout + 1) % 4); break;
    case SI_START: set_.start = (uint8_t)((set_.start + 1) % kPages); break;
    case SI_FLIP: flip(); return;   // saves itself
    case SI_LED: set_.led ^= 1; break;
    case SI_LED_BRIGHT: set_.ledBright = (uint8_t)((set_.ledBright + 1) % 3); break;
    case SI_COL_JAM: set_.colJam = (uint8_t)((set_.colJam + 1) % kColorCount); break;
    case SI_COL_BURST: set_.colBurst = (uint8_t)((set_.colBurst + 1) % kColorCount); break;
    case SI_COL_LOST: set_.colLost = (uint8_t)((set_.colLost + 1) % kColorCount); break;
    case SI_COL_ALERT: set_.colAlert = (uint8_t)((set_.colAlert + 1) % kColorCount); break;
    case SI_ALERT_FLASH: set_.alertFlash = (uint8_t)((set_.alertFlash + 1) % 5); break;
    case SI_SENS: set_.sens = (uint8_t)((set_.sens + 1) % 5); break;
    case SI_LEN: set_.len = (uint8_t)((set_.len + 1) % 5); break;
    case SI_JAM_ALERT: set_.jamAlert ^= 1; break;
    case SI_DONE: menu_ = M_NONE; shown_ = 255; return;
    default:
      if (k >= SI_BAND0 && k < SI_BAND0 + kSubGhzBands) set_.bands ^= (uint8_t)(1u << kSubGhzBandOrder[k - SI_BAND0]);
      else return;
  }
  // Show the chosen LED color / brightness on the ring for a moment.
  if (k == SI_COL_JAM || k == SI_COL_BURST || k == SI_COL_LOST || k == SI_COL_ALERT || k == SI_LED_BRIGHT ||
      k == SI_LED || k == SI_THEME) {
    ledTestColor_ = k == SI_COL_JAM ? set_.colJam : k == SI_COL_BURST ? set_.colBurst : k == SI_COL_LOST ? set_.colLost
                  : k == SI_COL_ALERT ? set_.colAlert : 7;
    ledTestUntil_ = millis() + 1200;
  }
  applyRadioSettings();
  saveSettings();
}

static void row(int y, int h, uint8_t size, uint16_t fg, const char* s);
static void text(int x, int y, uint8_t size, uint16_t fg, const char* s);

static void menuHeader(const char* title, const char* help) {
  stripBegin(0, 22, PANEL);
  Adafruit_GFX& d = g();
  d.setTextSize(2);
  d.setTextColor(ACCENT, PANEL);
  d.setCursor(6, 4);
  d.print(title);
  d.setTextSize(1);
  d.setTextColor(MUTED, PANEL);
  d.setCursor(6 + (int)strlen(title) * 12 + 12, 8);
  d.print(help);
  stripEnd();
}

static void drawPowerMenu() {
  menuHeader("POWER", "wheel: choose  press: do it  BACK: cancel");
  if (confirm_) {
    if (confirm_ == 1) {
      row(34, 24, 2, CRIT, "Power off?");
      row(64, 16, 1, TEXT, "The battery is disconnected. Plug in USB to turn it on.");
      row(82, 16, 1, TEXT, "The master will show this node as lost until then.");
      row(110, 24, 2, ACCENT, "Press the wheel: power off");
      row(140, 24, 2, MUTED, "BACK: cancel");
    } else {
      row(34, 24, 2, WARN, "On USB power");
      row(64, 16, 1, TEXT, "It can't switch off while plugged in: USB keeps it running.");
      row(82, 16, 1, TEXT, "Unplug it first, or choose Deep sleep.");
      row(110, 24, 2, MUTED, "");
      row(140, 24, 2, MUTED, "BACK: back to the menu");
    }
    return;
  }
  const int rowH = 26, top = 26;
  for (uint8_t k = 0; k < PM_COUNT; ++k) {
    const int y = top + k * rowH;
    const bool sel = k == menuSel_;
    stripBegin(y, rowH, sel ? PANEL : BG);
    Adafruit_GFX& d = g();
    const int ly = y - oy();
    if (sel) d.fillRect(0, ly, 4, rowH, ACCENT);
    d.setTextSize(2);
    d.setTextColor(k == PM_POWER_OFF ? CRIT : sel ? ACCENT : TEXT, sel ? PANEL : BG);
    d.setCursor(12, ly + 5);
    d.print(kPowerItems[k]);
    if (sel && kPowerHelp[k][0]) {
      d.setTextSize(1);
      d.setTextColor(MUTED, PANEL);
      const int tx = 12 + (int)strlen(kPowerItems[k]) * 12 + 10;
      if (tx + (int)strlen(kPowerHelp[k]) * 6 <= W - 4) { d.setCursor(tx, ly + 9); d.print(kPowerHelp[k]); }
    }
    stripEnd();
  }
}

static uint16_t rgb565(const Color& c) { return rgb(c.r, c.g, c.b); }

// A scrolling list (SETTINGS, the FOXHUNT list): 7 rows of 21 px under the header, the
// selected one highlighted, and a scroll bar.
template <typename RowFn>
static void drawList(uint8_t count, RowFn drawRow) {
  constexpr uint8_t kRows = 7;
  constexpr int rowH = 21, top = 24;
  if (menuSel_ < menuTop_) menuTop_ = menuSel_;
  if (menuSel_ >= menuTop_ + kRows) menuTop_ = (uint8_t)(menuSel_ - kRows + 1);
  for (uint8_t r = 0; r < kRows; ++r) {
    const uint8_t k = (uint8_t)(menuTop_ + r);
    const int y = top + r * rowH;
    const bool sel = k == menuSel_;
    stripBegin(y, rowH, sel ? PANEL : BG);
    if (k < count) {
      Adafruit_GFX& d = g();
      const int ly = y - oy();
      if (sel) d.fillRect(0, ly, 4, rowH, ACCENT);
      drawRow(d, k, ly, sel, sel ? PANEL : BG);
    }
    stripEnd();
  }
  stripBegin(top + kRows * rowH, H - (top + kRows * rowH), BG);
  if (count > kRows) {
    Adafruit_GFX& d = g();
    const int ly = top + kRows * rowH - oy();
    const int span = W - 16, bar = span * kRows / count;
    const int bx = 8 + (span - bar) * menuTop_ / (count - kRows);
    d.fillRect(8, ly + 2, span, 2, PANEL);
    d.fillRect(bx, ly + 2, bar, 2, MUTED);
  }
  stripEnd();
}

static void drawSettingsMenu() {
  menuHeader("SETTINGS", "press: change  BACK: done");
  drawList(SI_COUNT, [](Adafruit_GFX& d, uint8_t k, int ly, bool sel, uint16_t bg) {
    char label[32], value[24];
    int swatch;
    settingText(k, label, sizeof(label), value, sizeof(value), swatch);
    d.setTextSize(2);
    d.setTextColor(sel ? ACCENT : TEXT, bg);
    d.setCursor(12, ly + 3);
    d.print(label);
    d.setTextColor(sel ? TEXT : MUTED, bg);
    int vx = W - 8 - (int)strlen(value) * 12;
    if (strlen(value) > 8) { d.setTextSize(1); vx = W - 8 - (int)strlen(value) * 6; d.setCursor(vx, ly + 7); }
    else d.setCursor(vx, ly + 3);
    d.print(value);
    if (swatch >= 0) {
      if (swatch == kColorOff) d.drawRect(vx - 22, ly + 3, 16, 14, MUTED);
      else d.fillRect(vx - 22, ly + 3, 16, 14, rgb565(kColors[swatch]));
    }
  });
}

static void drawHuntPick() {
  menuHeader("FOXHUNT", "press: start  BACK: cancel");
  if (!nTargets_) { row(60, 20, 1, MUTED, "Nothing to hunt yet."); return; }
  drawList(nTargets_, [](Adafruit_GFX& d, uint8_t k, int ly, bool sel, uint16_t bg) {
    const Target& t = targets_[k];
    const bool sur = t.kind == 1 && (kSurveillanceMask & catBit(t.cat));
    d.setTextSize(t.kind == 0 ? 2 : 1);
    d.setTextColor(sur ? WARN : sel ? ACCENT : TEXT, bg);
    d.setCursor(12, t.kind == 0 ? ly + 3 : ly + 7);
    d.print(t.text);
    char b[12];
    if (t.rssi > -127) snprintf(b, sizeof(b), "%d", t.rssi);
    else snprintf(b, sizeof(b), "--");
    d.setTextSize(2);
    d.setTextColor(sel ? TEXT : MUTED, bg);
    d.setCursor(W - 8 - (int)strlen(b) * 12, ly + 3);
    d.print(b);
  });
}

// FOXHUNT screen: one big number, a bar with a 3 s peak hold, and how fresh the reading is.
static void drawHunting(bool full) {
  const uint32_t now = millis();
  uint32_t age = 0;
  const int8_t v = huntNow(now, &age);
  if (v > -127 && (v >= huntHold_ || now - huntHoldMs_ > 3000)) { huntHold_ = v; huntHoldMs_ = now; }
  if (full) {
    stripBegin(0, 22, PANEL);
    Adafruit_GFX& d = g();
    d.setTextSize(2);
    d.setTextColor(ACCENT, PANEL);
    d.setCursor(6, 4);
    d.print("FOXHUNT");
    d.setTextSize(1);
    d.setTextColor(TEXT, PANEL);
    d.setCursor(100, 8);
    d.print(hunt_.text);
    stripEnd();
  }
  // the big number: two strips (it is 56 px tall)
  char b[16];
  if (v > -127) snprintf(b, sizeof(b), "%d", v);
  else snprintf(b, sizeof(b), "--");
  const uint16_t col = v <= -127 ? MUTED : v > -55 ? CRIT : v > -70 ? WARN : v > -85 ? OK : ACCENT;
  for (int y = 30; y < 86; y += kStripH) {
    const int h = 86 - y < kStripH ? 86 - y : kStripH;
    stripBegin(y, h, BG);
    Adafruit_GFX& d = g();
    d.setTextSize(7);
    d.setTextColor(col, BG);
    const int tw = (int)strlen(b) * 42;
    d.setCursor((W - tw - 40) / 2, 30 - oy());
    d.print(b);
    d.setTextSize(2);
    d.setCursor((W - tw - 40) / 2 + tw + 6, 30 + 36 - oy());
    d.print("dBm");
    stripEnd();
  }
  // bar from -110 to -30 dBm, with the 3 s peak hold
  stripBegin(96, 22, BG);
  {
    Adafruit_GFX& d = g();
    const int bx = 10, bw = W - 20, ly = 96 - oy();
    auto px = [&](int dbm) { int q = (dbm + 110) * bw / 80; return q < 0 ? 0 : q > bw ? bw : q; };
    d.drawRect(bx, ly + 2, bw, 18, MUTED);
    if (v > -127) d.fillRect(bx + 1, ly + 3, px(v), 16, col);
    if (huntHold_ > -127 && now - huntHoldMs_ <= 3000) d.drawFastVLine(bx + px(huntHold_), ly, 22, TEXT);
  }
  stripEnd();
  char line[64];
  if (hunt_.kind == 0)
    snprintf(line, sizeof(line), "%s MHz, live: the loudest sample 10x a second", kSubGhzBandList[hunt_.band].name);
  else if (v > -127)
    snprintf(line, sizeof(line), "heard %lu.%lu s ago (Bluetooth full-time in a foxhunt)", (unsigned long)(age / 1000),
             (unsigned long)(age % 1000 / 100));
  else if (age != 0xFFFFFFFF)
    snprintf(line, sizeof(line), "not heard for %lu s: walk around, or it went quiet", (unsigned long)(age / 1000));
  else
    snprintf(line, sizeof(line), "waiting for its next advert...");
  row(126, 16, 1, v > -127 ? TEXT : WARN, line);
  row(146, 16, 1, MUTED, "Louder = closer. BACK: stop the foxhunt");
}

// Last screen before going dark, then everything off.
static void goDown(bool off) {
  huntStop();
  {
    tembed::SpiLock l;
    tft.fillScreen(BG);
    tft.setTextSize(2);
    tft.setTextColor(off ? CRIT : ACCENT, BG);
    tft.setCursor(20, 60);
    tft.print(off ? "Powering off..." : "Deep sleep...");
    tft.setTextSize(1);
    tft.setTextColor(MUTED, BG);
    tft.setCursor(20, 100);
    tft.print(off ? "Plug in USB to turn it on." : "Press BACK to wake it.");
  }
  Serial.printf("[display] %s\n", off ? "power off (battery disconnected)" : "deep sleep, BACK wakes it");
  ring.clear();
  ring.show();
  delay(1200);
  {
    tembed::SpiLock l;
    tft.enableSleep(true);
  }
  backlight(false);
  subghz::shutdown();   // CC1101 asleep; keeps the SPI bus
  if (off) {
    Serial.flush();
    if (tembed::batteryOff()) delay(2000);   // power is gone here, unless USB came back
    Serial.println("[display] still running (charger didn't answer, or on USB): deep sleep instead");
  }
  pinMode(tembed::TFT_BL, OUTPUT);   // off the PWM, so it can be held low
  tembed::deepSleep();
}

static void powerDo() {
  switch (menuSel_) {
    case PM_SCREEN_OFF: menu_ = M_NONE; setOn(false); break;
    case PM_DEEP_SLEEP: goDown(false); break;
    case PM_POWER_OFF: confirm_ = tembed::usbPowered() ? 2 : 1; break;
    case PM_RESTART:
      Serial.println("[display] restart from the power menu");
      Serial.flush();
      ESP.restart();
      break;
    default: menu_ = M_NONE; shown_ = 255; break;
  }
}

// ---------------------------------------------------------------------------
// Pages (caller holds the SPI lock).
static void text(int x, int y, uint8_t size, uint16_t fg, const char* s) {
  Adafruit_GFX& d = g();
  d.setTextSize(size);
  d.setTextColor(fg, BG);
  d.setCursor(x, y - oy());
  d.print(s);
}
static void row(int y, int h, uint8_t size, uint16_t fg, const char* s) {
  stripBegin(y, h, BG);
  text(8, y + 2, size, fg, s);
  stripEnd();
}

// "now", "45s", "12m", "3h", "2d"
static void ago(char* out, size_t n, uint32_t s) {
  if (s < 5) snprintf(out, n, "now");
  else if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(out, n, "%luh", (unsigned long)(s / 3600));
  else snprintf(out, n, "%lud", (unsigned long)(s / 86400));
}

// The logo (bold italic, SKY in blue): light text on black in the art, so brightness is
// taken as coverage and blended onto `bg`, as the CYD does. Red at night, as the CYD's.
static void drawLogo(int x, int y, uint16_t bg) {
  const uint8_t br = (uint8_t)(((bg >> 11) & 31) * 255 / 31), bgc = (uint8_t)(((bg >> 5) & 63) * 255 / 63),
                bb = (uint8_t)((bg & 31) * 255 / 31);
  uint16_t line[SKYLOGO_SMALL_W];
  for (int yy = 0; yy < SKYLOGO_SMALL_H; ++yy) {
    for (int xx = 0; xx < SKYLOGO_SMALL_W; ++xx) {
      const uint16_t v = SKYLOGO_SMALL[yy * SKYLOGO_SMALL_W + xx];
      uint8_t r = (uint8_t)(((v >> 11) & 31) * 255 / 31), gg = (uint8_t)(((v >> 5) & 63) * 255 / 63),
              b = (uint8_t)((v & 31) * 255 / 31);
      if (set_.night) { const uint8_t l = r > gg ? (r > b ? r : b) : (gg > b ? gg : b); r = l; gg = (uint8_t)(l * 3 / 10); b = (uint8_t)(l / 5); }
      const uint8_t a = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);   // coverage
      const uint16_t k = 255 - a;
      const uint8_t R = (uint8_t)(r + br * k / 255), G = (uint8_t)(gg + bgc * k / 255), B = (uint8_t)(b + bb * k / 255);
      line[xx] = rgb(R, G, B);
    }
    g().drawRGBBitmap(x, y + yy - oy(), line, SKYLOGO_SMALL_W, 1);
  }
}

static void header() {
  const Info& i = info_;
  stripBegin(0, 22, PANEL);
  Adafruit_GFX& d = g();
  // The logo always stays top left (as on the CYD and the Heltec); the name given on the
  // master goes on the right, before the battery.
  drawLogo(6, 2, PANEL);
  char b[24];
  if (i.battery_pct <= 100) snprintf(b, sizeof(b), "%u%%", i.battery_pct);
  else snprintf(b, sizeof(b), "USB");
  const int bx = W - 6 - (int)strlen(b) * 12;
  d.setTextSize(2);
  d.setTextColor(i.battery_pct <= 15 ? CRIT : TEXT, PANEL);
  d.setCursor(bx, 4);
  d.print(b);
  // page dots
  const int dx = 6 + SKYLOGO_SMALL_W + 12;   // just after the logo
  for (uint8_t p = 0; p < kPages; ++p) d.fillCircle(dx + p * 10, 11, 3, p == page_ ? ACCENT : MUTED);
  if (i.label && i.label[0]) {
    const int left = dx + kPages * 10 + 4, room = (bx - 8 - left) / 6;   // characters that fit at size 1
    char n[32];
    snprintf(n, sizeof(n), "%.*s", room > 0 ? (room < 31 ? room : 31) : 0, i.label);
    d.setTextSize(1);
    d.setTextColor(MUTED, PANEL);
    d.setCursor(bx - 8 - (int)strlen(n) * 6, 8);
    d.print(n);
  }
  stripEnd();
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
  snprintf(b, sizeof(b), "SUB-GHZ  %s", !subghz::ready() ? "radio not found" : !subghz::running() ? "off (category)"
                                         : !set_.bands ? "all bands off" : jam ? "JAMMING" : "watching");
  row(78, 22, 2, !subghz::ready() ? CRIT : jam ? CRIT : subghz::running() && set_.bands ? ACCENT : MUTED, b);
  snprintf(b, sizeof(b), "%lu bursts  %lu sweeps", (unsigned long)total, (unsigned long)subghz::sweeps());
  row(104, 14, 1, TEXT, b);
  // The cluster's latest alert, if there's a fresh one (more on ALERTS)
  if (dashMs_ && now - dashMs_ < 30000 && dash_.alert.seq && dash_.alert.level >= 2 && dash_.alert.age_s < 600) {
    char a[12];
    ago(a, sizeof(a), dash_.alert.age_s);
    snprintf(b, sizeof(b), "%.40s  %s", dash_.alert.text, a);
    row(124, 16, 1, CRIT, b);
  } else {
    row(124, 16, 1, MUTED, "");
  }
  const uint32_t up = i.uptime_s;
  snprintf(b, sizeof(b), "up %luh%02lum   hold the wheel: settings", (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  row(150, 14, 1, MUTED, b);
}

// ALERTS: what the cluster is warning about, from the master (refreshed with every report,
// about every 5 s).
static void pageAlerts() {
  const uint32_t now = millis();
  char b[64], a[12];
  const bool fresh = dashMs_ && now - dashMs_ < 30000;
  if (!fresh) {
    row(28, 14, 1, MUTED, "CLUSTER ALERTS");
    row(50, 22, 2, WARN, info_.paired ? "Waiting for the master" : "No master");
    row(78, 16, 1, MUTED, info_.paired ? "Its alerts arrive with the next report (about 5 s)." : "Alerts come from the master once this node joins.");
    for (int y = 96; y < H; y += 16) row(y, 16, 1, MUTED, "");
    return;
  }
  snprintf(b, sizeof(b), "CLUSTER ALERTS  %u devices now  nodes %u/%u", dash_.devices_now, dash_.nodes_online, dash_.nodes_total);
  row(28, 14, 1, MUTED, b);
  // The latest alert, big
  if (dash_.alert.seq && dash_.alert.level) {
    ago(a, sizeof(a), dash_.alert.age_s);
    snprintf(b, sizeof(b), "%.21s %s", dash_.alert.text, a);
    row(44, 22, 2, dash_.alert.level >= 2 ? CRIT : WARN, b);
  } else {
    row(44, 22, 2, OK, "No alerts");
  }
  // What's of concern nearby (surveillance the cluster hears now)
  uint8_t r = 0;
  for (uint8_t i = 0; i < dash_.n_threats && r < 2; ++i, ++r) {
    const DashThreat& t = dash_.threats[i];
    snprintf(b, sizeof(b), "%-26.26s %4d dBm%s", t.text[0] ? t.text : categoryLabel(t.category), t.rssi,
             (t.flags & DR_FOLLOWING) ? " FOLLOWING" : (t.flags & DR_STRONG) ? " strong" : "");
    row(70 + r * 14, 14, 1, (t.flags & DR_FOLLOWING) ? CRIT : WARN, b);
  }
  if (!dash_.n_threats) { row(70, 14, 1, OK, "Nothing of concern nearby."); r = 1; }
  for (; r < 2; ++r) row(70 + r * 14, 14, 1, MUTED, "");
  // Recent events, newest first
  row(100, 12, 1, MUTED, "RECENT EVENTS");
  const bool evFresh = eventsMs_ && now - eventsMs_ < 30000;
  for (uint8_t k = 0; k < 4; ++k) {
    const int y = 112 + k * 14;
    if (evFresh && k < events_.n) {
      const DashEvent& e = events_.ev[k];
      ago(a, sizeof(a), e.age_s);
      snprintf(b, sizeof(b), "%4s  %.44s", a, e.text);
      row(y, 14, 1, e.level >= 2 ? CRIT : e.level == 1 ? WARN : TEXT, b);
    } else {
      row(y, 14, 1, MUTED, k == 0 && evFresh && !events_.n ? "None yet." : "");
    }
  }
}

static void pageBands() {
  const SubGhzWatch& w = subghz::watch();
  const uint32_t now = millis();
  char b[48];
  row(28, 12, 1, MUTED, "BAND      LEVEL (floor . now . peak)        BURSTS");
  for (uint8_t row_ = 0; row_ < kSubGhzBands; ++row_) {
    const uint8_t k = kSubGhzBandOrder[row_];   // lowest frequency first
    const int y = 43 + row_ * 21;
    stripBegin(y, 21, BG);
    Adafruit_GFX& d = g();
    const int ly = y - oy() - 2;   // this row's top in the strip (rows are 21 px: 6 bands)
    const bool watched = set_.bands & (1u << k);
    const bool jam = watched && w.jammed(k, now), cur = watched && subghz::currentBand() == k;
    text(8, y + 3, 2, !watched ? MUTED : jam ? CRIT : cur ? ACCENT : TEXT, kSubGhzBandList[k].name);
    // bar from -120 to -30 dBm
    const int bx = 112, bw = 140;
    auto px = [&](int dbm) { int v = (dbm + 120) * bw / 90; return bx + (v < 0 ? 0 : v > bw ? bw : v); };
    d.drawRect(bx, ly + 6, bw, 12, MUTED);
    if (watched) {
      const int8_t fl = w.floorDbm(k), now_ = w.lastDbm(k), pk = w.peakDbm(k);
      if (now_ > -127) d.fillRect(bx + 1, ly + 7, px(now_) - bx, 10, jam ? CRIT : ACCENT);
      if (fl > -127) d.drawFastVLine(px(fl), ly + 4, 16, OK);
      if (pk > -127) d.drawFastVLine(px(pk), ly + 4, 16, WARN);
      snprintf(b, sizeof(b), jam ? "JAM" : "%lu", (unsigned long)w.bursts(k));
    } else {
      snprintf(b, sizeof(b), "off");
    }
    text(262, y + 3, 2, !watched ? MUTED : jam ? CRIT : TEXT, b);
    stripEnd();
  }
}

// HISTORY: the last 10 minutes of each band, a bar per 10 s: how far its loudest moment
// rose over the band's quiet level; a slot with a burst in it is in color.
static void pageHistory() {
  const SubGhzWatch& w = subghz::watch();
  row(28, 12, 1, MUTED, "LAST 10 MIN (bar = 10 s, color = burst)      now >");
  int8_t pk[subghz::kHistSlots];
  uint8_t bu[subghz::kHistSlots];
  for (uint8_t row_ = 0; row_ < kSubGhzBands; ++row_) {
    const uint8_t k = kSubGhzBandOrder[row_];
    const int y = 43 + row_ * 21;
    subghz::history(k, pk, bu);
    stripBegin(y, 21, BG);
    Adafruit_GFX& d = g();
    const int ly = y - oy();
    const bool watched = set_.bands & (1u << k);
    text(8, y + 7, 1, watched ? TEXT : MUTED, kSubGhzBandList[k].name);
    const int gx = 62, base = ly + 19;
    d.drawFastHLine(gx, base, subghz::kHistSlots * 4, PANEL);
    const int8_t fl = w.floorDbm(k);
    for (uint8_t s = 0; s < subghz::kHistSlots; ++s) {
      if (pk[s] <= -127 || fl <= -127) continue;
      int rise = pk[s] - fl;   // dB over the quiet level: 0..36 -> 1..18 px
      int h = rise <= 0 ? 1 : rise >= 36 ? 18 : 1 + rise / 2;
      d.fillRect(gx + s * 4, base - h, 3, h, bu[s] ? (bu[s] > 2 ? CRIT : WARN) : MUTED);
    }
    stripEnd();
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
    if (k >= n) { if (n) tft.fillRect(0, y, W, 20, BG); continue; }   // (n == 0: the hint row below)
    const uint32_t ago_s = (now - r[k].ms) / 1000;
    snprintf(b, sizeof(b), "%4lus  %-7s %4d dBm  %s", (unsigned long)ago_s, kSubGhzBandList[r[k].band].name, r[k].peak,
             kSubGhzBandList[r[k].band].what);
    row(y, 20, 1, k == 0 && ago_s < 5 ? WARN : TEXT, b);
  }
  if (!n) row(60, 20, 1, MUTED, "Nothing yet. Press a car fob or garage remote nearby.");
}

// FOXHUNT page: what there is to hunt; the wheel opens the list.
static void pageHunt() {
  buildTargets();
  row(28, 14, 1, MUTED, "FOXHUNT: walk toward a signal, louder = closer");
  row(46, 22, 2, ACCENT, "Press the wheel to pick");
  char b[64];
  const uint8_t ble = nTargets_ > kSubGhzBands ? (uint8_t)(nTargets_ - kSubGhzBands) : 0;
  snprintf(b, sizeof(b), "%u sub-GHz bands, %u Bluetooth devices heard here", kSubGhzBands, ble);
  row(74, 14, 1, TEXT, b);
  // the strongest few Bluetooth devices (surveillance first)
  for (uint8_t k = 0; k < 4; ++k) {
    const int y = 94 + k * 16;
    if (k < ble) {
      const Target& t = targets_[kSubGhzBands + k];
      snprintf(b, sizeof(b), "%-36.36s %4d dBm", t.text, t.rssi);
      row(y, 16, 1, (kSurveillanceMask & catBit(t.cat)) ? WARN : MUTED, b);
    } else {
      row(y, 16, 1, MUTED, k == 0 && !ble ? "No Bluetooth heard here in the last minute." : "");
    }
  }
}

static void drawAll(bool full) {
  if (!ok_ || !on_) return;
  tembed::SpiLock l;
  if (menu_) {
    const uint8_t id = 250 + menu_;
    if (full || shown_ != id) { tft.fillScreen(BG); shown_ = id; full = true; }
    if (menu_ == M_POWER) drawPowerMenu();
    else if (menu_ == M_SETTINGS) drawSettingsMenu();
    else if (menu_ == M_HUNTPICK) drawHuntPick();
    else drawHunting(full);
    return;
  }
  if (!haveInfo_) return;
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
    // Drawn once per change (state or code), not every second: a full-screen redraw flickers.
    static uint32_t drawnKey = 0;
    const uint32_t key = ((uint32_t)i.pair << 24) ^ i.pair_code ^ (uint32_t)set_.rot << 28 ^ (uint32_t)set_.night << 30;
    if (!full && shown_ == 249 && drawnKey == key) return;
    drawnKey = key;
    shown_ = 249;
    // Centered text (the built-in font is 6 x 8 pixels per character at size 1).
    auto center = [](int y, uint8_t size, uint16_t fg, const char* s) {
      text((W - (int)strlen(s) * 6 * size) / 2, y, size, fg, s);
    };
    tft.fillScreen(BG);
    drawLogo((W - SKYLOGO_SMALL_W) / 2, 8, BG);
    if (i.pair == 2) {
      char b[16];
      snprintf(b, sizeof(b), "%06lu", (unsigned long)i.pair_code);
      center(38, 2, TEXT, "Pair with this master?");
      center(66, 5, ACCENT, b);
      center(118, 1, MUTED, "Check it matches the code on the master's page,");
      center(130, 1, MUTED, "then press ACCEPT there.");
    } else {
      center(52, 2, WARN, i.pair == 3 ? "Pairing open" : "Waiting to be paired");
      center(92, 1, MUTED, "On the master's page press PAIR A NODE,");
      center(104, 1, MUTED, "then hold this board close to the master.");
      center(140, 1, MUTED, "Hold BACK 2 s for the power menu.");
    }
    return;
  }
  if (full || shown_ != page_) { tft.fillScreen(BG); shown_ = page_; }
  header();
  switch (page_) {
    case P_HOME: pageHome(); break;
    case P_ALERTS: pageAlerts(); break;
    case P_BANDS: pageBands(); break;
    case P_HISTORY: pageHistory(); break;
    case P_RECENT: pageRecent(); break;
    default: pageHunt(); break;
  }
}

// ---------------------------------------------------------------------------
// LED ring: the alert light. Highest first: a color being tried in SETTINGS, the FOXHUNT
// meter, jamming (solid while it lasts), a new cluster alert (pulses for 30 s), a sub-GHz
// burst (a short flash), lost master (a slow blink). Dimmer at night.
static uint8_t ledLevel() {
  const uint8_t lv = kLedLevel[set_.ledBright];
  return set_.night ? (uint8_t)(lv / 5 > 2 ? lv / 5 : 2) : lv;
}
static void ledTick(uint32_t now) {
  static uint32_t burstsSeen = 0, flashUntil = 0, shown = 0xFFFFFFFF;
  const SubGhzWatch& w = subghz::watch();
  uint32_t total = 0;
  bool jam = false;
  for (uint8_t k = 0; k < kSubGhzBands; ++k) {
    total += w.bursts(k);
    jam = jam || ((set_.bands & (1u << k)) && w.jammed(k, now));
  }
  if (total != burstsSeen) { burstsSeen = total; flashUntil = now + 150; }
  const uint8_t lv = ledLevel();
  // FOXHUNT: the ring is the meter: more LEDs as it gets louder (-100 to -40 dBm), blue to red
  if (menu_ == M_HUNTING && (int32_t)(ledTestUntil_ - now) <= 0) {
    uint32_t age = 0;
    const int8_t v = huntNow(now, &age);
    const int lit = v <= -100 ? (v > -127 ? 1 : 0) : v >= -40 ? 8 : 1 + (v + 100) * 7 / 60;
    const uint32_t want = 0x80000000u | ((uint32_t)lit << 8) | lv;
    if (want == shown) return;
    shown = want;
    static const uint8_t kMeter[8][3] = {{0, 0, 255}, {0, 120, 255}, {0, 220, 200}, {0, 255, 80},
                                         {160, 255, 0}, {255, 200, 0}, {255, 100, 0}, {255, 0, 0}};
    for (int i = 0; i < 8; ++i) {
      if (i < lit) {
        uint8_t r = kMeter[i][0], g = kMeter[i][1], b = kMeter[i][2];
        if (set_.night) { const uint8_t l = r > g ? (r > b ? r : b) : (g > b ? g : b); r = l; g = 0; b = 0; }
        ring.setPixelColor(i, ring.Color(r * lv / 255, g * lv / 255, b * lv / 255));
      } else {
        ring.setPixelColor(i, 0);
      }
    }
    ring.show();
    return;
  }
  const bool lost = haveInfo_ && info_.role && strcmp(info_.role, "NODE") == 0 && !info_.paired && !info_.pair;
  uint8_t c = kColorOff;
  if ((int32_t)(ledTestUntil_ - now) > 0) c = ledTestColor_;
  else if (!set_.led) c = kColorOff;
  else if (jam && set_.colJam != kColorOff) c = set_.colJam;
  else if ((alertLedHeld_ || (int32_t)(alertLedUntil_ - now) > 0) && set_.colAlert != kColorOff && now % 1000 < 500) c = set_.colAlert;
  else if ((int32_t)(flashUntil - now) > 0 && set_.colBurst != kColorOff) c = set_.colBurst;
  else if (lost && set_.colLost != kColorOff && now % 2000 < 200) c = set_.colLost;
  const uint32_t want = ((uint32_t)c << 8) | lv;
  if (want == shown) return;
  shown = want;
  const Color& col = kColors[c];
  ring.fill(ring.Color(col.r * lv / 255, col.g * lv / 255, col.b * lv / 255));
  ring.show();
}

void update(const Info& i) {
  info_ = i;
  haveInfo_ = true;
  if (menu_ != M_HUNTING) drawAll(false);   // hunting redraws itself 10x a second
}

static void leaveMenu() {
  if (menu_ == M_HUNTING) { huntStop(); menu_ = M_HUNTPICK; menuSel_ = 0; menuTop_ = 0; buildTargets(); return; }
  menu_ = M_NONE;
  shown_ = 255;
}

void tick(uint32_t now) {
  static int16_t last = 0;
  static uint32_t lastDraw = 0, keyAt = 0, backAt = 0, lastInput = 0;
  static bool keyWas = false, backWas = false, backHeld = false, keyHeld = false, offByTimeout = false;
  bool input = false;
  // Asleep by the timeout: the first touch only wakes the screen.
  auto wake = [&]() -> bool {
    lastInput = now;
    if (on_ || !offByTimeout) return false;
    offByTimeout = false;
    setOn(true);
    drawAll(true);
    return true;
  };

  const int16_t c = encCount_;
  // One click of the T-Embed's wheel = 2 transitions (it rests at both 00 and 11; Bruce
  // reads it the same way). 4 here took 2 to 3 clicks per page.
  constexpr int16_t kPerClick = 2;
  const int16_t steps = (int16_t)(c - last) / kPerClick;
  if (steps) {
    last = (int16_t)(last + steps * kPerClick);
    if (!wake()) {
      if (menu_ == M_HUNTING) {
        // the wheel does nothing while hunting
      } else if (menu_) {
        if (!confirm_) menuSel_ = (uint8_t)((menuSel_ + menuCount() + (steps % menuCount())) % menuCount());
      } else {
        page_ = (uint8_t)((page_ + kPages + (steps % kPages)) % kPages);
      }
      if (on_ && menu_ != M_HUNTING) drawAll(true);
    }
    lastDraw = now;
  }

  const bool key = digitalRead(tembed::ENC_KEY) == LOW, back = digitalRead(tembed::BACK_KEY) == LOW;
  // Wheel: a press acts when let go; held 1 s, SETTINGS.
  if (key && !keyWas) { keyAt = now; keyHeld = false; }
  if (key && !keyHeld && now - keyAt > 1000 && !menu_) {
    keyHeld = true;
    offByTimeout = false;
    if (!on_) setOn(true);
    menu_ = M_SETTINGS; menuSel_ = 0; menuTop_ = 0;
    drawAll(true);
  }
  if (!key && keyWas && !keyHeld && now - keyAt > 30) {
    if (!wake()) {
      if (menu_ == M_POWER) {
        if (confirm_ == 1) goDown(true);
        else if (!confirm_) powerDo();
      } else if (menu_ == M_SETTINGS) {
        settingNext(menuSel_);
      } else if (menu_ == M_HUNTPICK) {
        if (menuSel_ < nTargets_) { huntStart(targets_[menuSel_]); menu_ = M_HUNTING; }
      } else if (menu_ == M_HUNTING) {
        // nothing: BACK stops
      } else if (page_ == P_HUNT) {
        buildTargets();
        menu_ = M_HUNTPICK; menuSel_ = 0; menuTop_ = 0;
      } else {
        page_ = (uint8_t)((page_ + 1) % kPages);
      }
      if (on_) drawAll(true);
    }
  }
  if (key) input = true;

  // BACK: a press turns the screen off and on (or backs out); held 2 s, POWER.
  if (back && !backWas) { backAt = now; backHeld = false; }
  if (back && !backHeld && now - backAt > 2000) {
    backHeld = true;
    offByTimeout = false;
    if (!on_) setOn(true);
    if (menu_ == M_HUNTING) huntStop();
    menu_ = M_POWER; menuSel_ = 0; confirm_ = 0;
    drawAll(true);
  }
  if (!back && backWas && !backHeld && now - backAt > 30) {
    if (!wake()) {
      if (menu_) {
        if (confirm_) confirm_ = 0;
        else leaveMenu();
        drawAll(true);
      } else {
        setOn(!on_);
        if (on_) drawAll(true);
      }
    }
  }
  if (back) input = true;
  keyWas = key;
  backWas = back;
  if (input || steps) {
    lastInput = now;
    alertLedHeld_ = false;   // a button: the alert is seen, stop pulsing
    alertLedUntil_ = now;
  }

  // Screen timeout (never in a menu or while hunting).
  if (on_ && !menu_ && kTimeoutMs[set_.timeout] && now - lastInput > kTimeoutMs[set_.timeout]) {
    setOn(false);
    offByTimeout = true;
  }
  // live pages: BANDS 4x a second, a foxhunt 10x
  if (on_ && menu_ == M_HUNTING && now - lastDraw > 100) { lastDraw = now; drawAll(false); }
  else if (on_ && !menu_ && page_ == P_BANDS && now - lastDraw > 250) { lastDraw = now; drawAll(false); }
  ledTick(now);
}

}  // namespace display
}  // namespace ss
#endif
