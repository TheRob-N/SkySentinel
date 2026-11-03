#ifdef SS_CYD
#include "cyd_gps.h"

#include "ss_gps.h"

namespace ss {
namespace cydgps {

static constexpr int kRxPin = 35;
static const uint32_t kBauds[] = {9600, 115200, 38400};
static Nmea nmea;
static Preferences* prefs_ = nullptr;
static bool present_ = false;      // a GPS has talked to this board (now or before)
static uint8_t baudIdx_ = 0;
static uint32_t bytes_ = 0, lastGood_ = 0, tryStart_ = 0;

static void openAt(uint8_t i) {
  baudIdx_ = i;
  Serial2.end();
  Serial2.setRxBufferSize(1024);
  Serial2.begin(kBauds[i], SERIAL_8N1, kRxPin, -1);   // RX only
  bytes_ = 0;
  lastGood_ = nmea.sentences();
  tryStart_ = millis();
}

void begin(Preferences* prefs) {
  prefs_ = prefs;
  present_ = prefs && prefs->getBool("gps_seen", false);
  pinMode(kRxPin, INPUT);
  openAt(prefs ? (uint8_t)prefs->getUChar("gps_baud", 0) % 3 : 0);
}

void loop(uint32_t now) {
  while (Serial2.available()) {
    nmea.feed((char)Serial2.read(), now);
    ++bytes_;
  }
  if (nmea.sentences() != lastGood_) {
    lastGood_ = nmea.sentences();
    if (!present_) {
      present_ = true;
      Serial.printf("[gps] GPS found on GPIO %d at %lu baud\n", kRxPin, (unsigned long)kBauds[baudIdx_]);
      if (prefs_) { prefs_->putBool("gps_seen", true); prefs_->putUChar("gps_baud", baudIdx_); }
    }
    tryStart_ = now;
    return;
  }
  // Bytes but no valid sentences for 3 s: wrong speed, try the next one.
  // (No bytes at all: nothing plugged in, or it's off. Leave it.)
  if (now - tryStart_ > 3000) {
    if (bytes_ > 64) openAt((uint8_t)((baudIdx_ + 1) % 3));
    else { bytes_ = 0; tryStart_ = now; }
  }
}

uint8_t state(uint32_t now) { return present_ ? nmea.state(now) : GPS_NONE; }
uint8_t sats() { return nmea.sats(); }

bool info(GpsInfo& out, uint32_t now) {
  if (!present_) return false;
  fillGpsInfo(nmea, now, out);
  return true;
}

void fill(Heartbeat& hb, uint32_t now) {
  if (!present_) return;
  hb.gps = nmea.state(now);
  hb.gps_sats = nmea.sats();
  double la, lo;
  if (hb.gps >= GPS_FIX_2D && nmea.position(la, lo)) {
    hb.has_pos = true;
    hb.lat_e7 = (int32_t)(la * 1e7);
    hb.lon_e7 = (int32_t)(lo * 1e7);
  }
  nmea.utc(now, hb.utc);
}

bool handleCommand(const String& line) {
  if (line == "gps forget") {   // GPS removed for good: stop reporting it
    present_ = false;
    if (prefs_) prefs_->remove("gps_seen");
    Serial.println("[gps] forgotten");
    return true;
  }
  if (line != "gps") return false;
  const uint32_t now = millis();
  double la = 0, lo = 0;
  const bool pos = nmea.position(la, lo);
  Serial.printf("[gps] %s, %s, %u sats, %lu sentences (%lu bad), %lu baud on GPIO %d%s\n",
                present_ ? "present" : "none seen", gpsStateName(nmea.state(now)), nmea.sats(),
                (unsigned long)nmea.sentences(), (unsigned long)nmea.badChecksums(), (unsigned long)kBauds[baudIdx_],
                kRxPin, pos ? "" : ", no position yet");
  if (pos) Serial.printf("[gps] %.6f, %.6f\n", la, lo);
  return true;
}

}  // namespace cydgps
}  // namespace ss
#endif  // SS_CYD
