#ifdef SS_DISPLAY_ST7735
#include "node_display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// Heltec Wireless Tracker V1.1 (from Meshtastic's variant for this board). V1.0 boards
// use backlight 45 and VTFT_CTRL 46 (active low): build with those -D overrides.
#ifndef SS_TFT_CS
#define SS_TFT_CS 38
#define SS_TFT_DC 40
#define SS_TFT_RST 39
#define SS_TFT_SCK 41
#define SS_TFT_MOSI 42
#define SS_TFT_BL 21
#define SS_TFT_COL_OFFSET 26
#define SS_TFT_ROW_OFFSET 1
#endif
#ifndef SS_VEXT_PIN
#define SS_VEXT_PIN 3      // powers the screen and GPS, HIGH = on
#endif
#ifndef SS_TFT_INVERT
#define SS_TFT_INVERT 1    // IPS panels show inverted colors without this
#endif
#define SS_BATT_ADC 1
#define SS_BATT_CTRL 2

namespace ss {
namespace display {

// Adafruit's driver with the panel offsets this board needs.
class Panel : public Adafruit_ST7735 {
 public:
  Panel(SPIClass* spi) : Adafruit_ST7735(spi, SS_TFT_CS, SS_TFT_DC, SS_TFT_RST) {}
  void offsets(uint8_t col, uint8_t row) { _colstart = col; _rowstart = row; }
};

static SPIClass tftSpi(HSPI);  // the LoRa radio has the other SPI bus
static Panel tft(&tftSpi);
static bool ok_ = false, on_ = true;
static Info last_;
static bool drawn_ = false;

// Colors match the web UI's day theme (RGB565).
static constexpr uint16_t BG = 0x0883;      // #06101C
static constexpr uint16_t TEXT = 0xDF5E;    // #DCE9F3
static constexpr uint16_t MUTED = 0x8D37;   // #8AA4BA
static constexpr uint16_t ACCENT = 0x4E5D;  // #48C9E8
static constexpr uint16_t WARN = 0xF547;    // #F2A93B
static constexpr uint16_t CRIT = 0xFB4B;    // #FF6B5E

static void line(int y, uint16_t color, const char* text) {
  tft.fillRect(0, y, 160, 14, BG);
  tft.setCursor(4, y + 3);
  tft.setTextColor(color);
  tft.print(text);
}

bool begin() {
  pinMode(SS_VEXT_PIN, OUTPUT);
  digitalWrite(SS_VEXT_PIN, HIGH);
  delay(20);
  pinMode(SS_TFT_BL, OUTPUT);
  digitalWrite(SS_TFT_BL, HIGH);
  tftSpi.begin(SS_TFT_SCK, -1, SS_TFT_MOSI, SS_TFT_CS);
  tft.initR(INITR_MINI160x80);
  tft.offsets(SS_TFT_COL_OFFSET, SS_TFT_ROW_OFFSET);
  tft.setRotation(1);  // landscape, 160 x 80
  tft.invertDisplay(SS_TFT_INVERT);
  tft.fillScreen(BG);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.setTextColor(ACCENT);
  tft.setTextSize(2);
  tft.print("SKYSENTINEL");
  tft.setTextSize(1);
  ok_ = true;
  return true;
}

void setOn(bool on) {
  if (!ok_ || on == on_) return;
  on_ = on;
  digitalWrite(SS_TFT_BL, on ? HIGH : LOW);
  tft.enableDisplay(on);
  drawn_ = false;  // full redraw when it comes back
}

bool isOn() { return on_; }

static void fmtUptime(char* out, size_t n, uint32_t s) {
  snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

static bool identifying_ = false;

void update(const Info& in) {
  if (!ok_) return;
  char buf[40], up[16];
  // "Which one is this?": light the screen up even if screens are off, big and bright.
  if (in.identify) {
    if (!on_) { digitalWrite(SS_TFT_BL, HIGH); tft.enableDisplay(true); }
    static bool flip = false;
    flip = !flip;
    tft.fillScreen(flip ? ACCENT : BG);
    tft.setTextColor(flip ? BG : ACCENT);
    tft.setTextSize(2);
    tft.setCursor(32, 18);
    tft.print("THIS ONE");
    tft.setTextSize(1);
    tft.setCursor(4, 50);
    tft.print(in.label && in.label[0] ? in.label : "SkySentinel node");
    identifying_ = true;
    return;
  }
  if (identifying_) {
    identifying_ = false;
    drawn_ = false;
    if (!on_) { digitalWrite(SS_TFT_BL, LOW); tft.enableDisplay(false); }
  }
  // Pairing: the screen is all about it (and stays on even if screens are off)
  static uint8_t lastPair = 0;
  static uint32_t lastCode = 0xFFFFFFFF;
  if (in.pair == 1 || in.pair == 2) {
    if (!on_) { digitalWrite(SS_TFT_BL, HIGH); tft.enableDisplay(true); }
    if (lastPair != in.pair || lastCode != in.pair_code) {
      lastPair = in.pair;
      lastCode = in.pair_code;
      tft.fillScreen(BG);
      tft.setTextColor(ACCENT);
      tft.setTextSize(1);
      tft.setCursor(4, 4);
      tft.print("SKYSENTINEL  PAIRING");
      if (in.pair == 1) {
        tft.setTextColor(WARN);
        tft.setCursor(4, 24);
        tft.print("Not paired yet.");
        tft.setTextColor(TEXT);
        tft.setCursor(4, 40);
        tft.print("On the master's page:");
        tft.setCursor(4, 52);
        tft.print("CLUSTER > PAIR A NODE,");
        tft.setCursor(4, 64);
        tft.print("then hold this close.");
      } else {
        char b[24];
        snprintf(b, sizeof(b), "Master %04X code:", in.pair_master);
        tft.setTextColor(TEXT);
        tft.setCursor(4, 20);
        tft.print(b);
        snprintf(b, sizeof(b), "%03lu %03lu", (unsigned long)(in.pair_code / 1000 % 1000), (unsigned long)(in.pair_code % 1000));
        tft.setTextColor(ACCENT);
        tft.setTextSize(3);
        tft.setCursor(4, 36);
        tft.print(b);
        tft.setTextSize(1);
        tft.setTextColor(MUTED);
        tft.setCursor(4, 68);
        tft.print("Same on the page? ACCEPT");
      }
    }
    drawn_ = false;
    return;
  }
  if (lastPair) { lastPair = 0; lastCode = 0xFFFFFFFF; drawn_ = false; if (!on_) { digitalWrite(SS_TFT_BL, LOW); tft.enableDisplay(false); } }
  if (!on_) return;
  if (!drawn_) {
    tft.fillScreen(BG);
    tft.fillRect(0, 0, 160, 16, BG);
    tft.setCursor(4, 4);
    tft.setTextColor(ACCENT);
    tft.print("SKYSENTINEL");
    drawn_ = true;
    last_ = Info();
    last_.role = nullptr;  // forces the header to redraw
  }
  // Header: SKYSENTINEL + this node's name (or role) + battery
  static char lastLabel[24] = {1};  // copy: the caller reuses one buffer for the name
  const char* label = in.label ? in.label : "";
  if (last_.role != in.role || last_.battery_pct != in.battery_pct || last_.usb != in.usb ||
      strcmp(lastLabel, label) != 0) {
    snprintf(lastLabel, sizeof(lastLabel), "%s", label);
    tft.fillRect(72, 0, 88, 15, BG);
    tft.setTextColor(MUTED);
    const char* who = in.label && in.label[0] ? in.label : in.role;
    if (in.usb) snprintf(buf, sizeof(buf), "%.9s USB", who);
    else if (in.battery_pct <= 100) snprintf(buf, sizeof(buf), "%.9s %u%%", who, in.battery_pct);
    else snprintf(buf, sizeof(buf), "%.13s", who);
    tft.setCursor(156 - (int)strlen(buf) * 6, 4);
    tft.print(buf);
  }
  tft.drawFastHLine(0, 15, 160, 0x2A0B);

  // Line 1: link
  if (!strcmp(in.role, "NODE")) {
    if (in.paired) snprintf(buf, sizeof(buf), "Master %04X %4d dBm %s", in.master_id, in.link_rssi, in.configured ? "OK" : "...");
    else snprintf(buf, sizeof(buf), "Looking for a master...");
    line(17, in.paired ? (in.configured ? TEXT : WARN) : CRIT, buf);
  } else if (!strcmp(in.role, "MASTER")) {
    if (in.pair == 3) snprintf(buf, sizeof(buf), "PAIRING OPEN (%u online)", in.nodes_online);
    else snprintf(buf, sizeof(buf), "%u node%s online", in.nodes_online, in.nodes_online == 1 ? "" : "s");
    line(17, in.pair == 3 ? WARN : TEXT, buf);
  } else {
    line(17, TEXT, "Standalone");
  }
  // Line 2: what it's scanning, and GPS
  char g[12] = "";
  if (in.gps == 1) snprintf(g, sizeof(g), " GPS off");
  else if (in.gps == 2) snprintf(g, sizeof(g), " GPS --");
  else if (in.gps >= 3) snprintf(g, sizeof(g), " GPS %u", in.gps_sats);
  if (in.wifi_channels) snprintf(buf, sizeof(buf), "BLE %u%% WiFi %uch%s", in.ble_duty, in.wifi_channels, g);
  else snprintf(buf, sizeof(buf), "BLE %u%% WiFi ch6%s", in.ble_duty, g);
  line(31, in.gps >= 3 ? TEXT : MUTED, buf);
  // Line 3: LoRa
  if (in.lora) snprintf(buf, sizeof(buf), "LoRa %.10s %lu pk", in.lora_on, (unsigned long)in.lora_packets);
  else snprintf(buf, sizeof(buf), "LoRa off");
  line(45, in.lora ? MUTED : 0x4208, buf);
  // Line 4: counts + uptime
  fmtUptime(up, sizeof(up), in.uptime_s);
  if (!strcmp(in.role, "NODE")) snprintf(buf, sizeof(buf), "Sent %lu  up %s", (unsigned long)in.sent, up);
  else snprintf(buf, sizeof(buf), "Now %lu/%lu  up %s", (unsigned long)in.sent, (unsigned long)in.session, up);
  line(59, MUTED, buf);
  (void)CRIT;
  last_ = in;
}

uint8_t batteryPercent() {
  pinMode(SS_BATT_CTRL, OUTPUT);
  digitalWrite(SS_BATT_CTRL, HIGH);
  delay(2);
  const uint32_t mv = analogReadMilliVolts(SS_BATT_ADC);
  digitalWrite(SS_BATT_CTRL, LOW);
  const float v = mv * 4.9f * 1.045f / 1000.0f;  // divider ratio from Meshtastic's variant
  if (v < 2.5f) return 255;                       // no battery: USB power only
  const float pct = (v - 3.3f) / (4.2f - 3.3f) * 100.0f;
  return (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
}

}  // namespace display
}  // namespace ss
#endif  // SS_DISPLAY_ST7735
