// GPS lock status from a receiver's NMEA output (the Heltec Wireless Tracker's UC6580).
// Feed it characters; it keeps the fix type, satellites in use and position.
// Pure C++, host tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {

enum GpsState : uint8_t {
  GPS_NONE = 0,       // this board has no GPS
  GPS_SILENT = 1,     // has one, but it isn't talking (no NMEA in 5 s)
  GPS_SEARCHING = 2,  // talking, no fix yet
  GPS_FIX_2D = 3,
  GPS_FIX_3D = 4,
};
const char* gpsStateName(uint8_t s);  // "none", "silent", "searching", "2D", "3D"

// Satellite constellations (from the NMEA talker id or the GSA system id)
enum GnssSys : uint8_t { GNSS_UNKNOWN = 0, GNSS_GPS = 1, GNSS_GLONASS = 2, GNSS_GALILEO = 3, GNSS_BEIDOU = 4, GNSS_QZSS = 5 };
const char* gnssName(uint8_t sys);  // "GPS", "GLONASS", ...

struct GpsSat {
  uint8_t sys = 0, prn = 0;
  uint8_t elev = 0;      // degrees above the horizon
  uint16_t az = 0;       // degrees from north
  uint8_t snr = 0;       // dB-Hz, 0 = not tracked
  bool used = false;     // in the position fix
  uint32_t ms = 0;       // last heard (internal)
};

class Nmea {
 public:
  void feed(char c, uint32_t now_ms);
  uint8_t state(uint32_t now_ms) const;
  uint8_t sats() const { return sats_; }   // satellites used in the fix
  bool position(double& lat, double& lon) const;
  float hdop() const { return hdop_; }
  // UTC from the receiver's clock (RMC), as Unix seconds, advanced by millis since it
  // was read. False until it has sent a date (often before the position fix).
  bool utc(uint32_t now_ms, uint32_t& unix_s) const;
  // Height above sea level (m), ground speed (km/h) and course (degrees from north).
  // False before a fix.
  bool altitude(float& m) const;
  bool motion(float& kmh, float& course) const;
  // Satellites in view (heard in the last 10 s), up to max. Returns how many.
  size_t satellites(GpsSat* out, size_t max, uint32_t now_ms) const;
  uint32_t sentences() const { return good_; }
  uint32_t badChecksums() const { return bad_; }

 private:
  void sentence(uint32_t now_ms);
  char buf_[100] = {0};
  uint8_t len_ = 0;
  bool in_ = false;
  uint32_t good_ = 0, bad_ = 0;
  uint32_t last_ms_ = 0, fix_ms_ = 0;
  bool heard_ = false, fixed_ = false;
  uint8_t quality_ = 0, sats_ = 0, mode_ = 0;  // mode: GSA 1 none, 2 2D, 3 3D
  uint32_t mode_ms_ = 0;
  double lat_ = 0, lon_ = 0;
  float hdop_ = 0;
  uint32_t utc_ = 0, utc_ms_ = 0;
  float alt_ = 0, kmh_ = 0, course_ = 0;
  bool has_alt_ = false, has_motion_ = false;
  static constexpr size_t kMaxSats = 48;
  GpsSat sats_v_[kMaxSats];
  struct Used { uint8_t sys, prn; uint32_t ms; };
  Used used_[32];
  uint8_t n_used_ = 0;
  void noteSat(uint8_t sys, uint8_t prn, uint8_t elev, uint16_t az, uint8_t snr, uint32_t now_ms);
};

// Fills a GpsInfo (MSG_GPS) from what the receiver has said.
struct GpsInfo;
void fillGpsInfo(const Nmea& g, uint32_t now_ms, GpsInfo& out);

// Days-from-civil: Unix seconds for a UTC date and time (year >= 1970).
uint32_t unixTime(int year, int month, int day, int hour, int min, int sec);
// "2026-10-30 15:04:05" (20 bytes with the terminator)
void formatUtc(uint32_t unix_s, char out[20]);

}  // namespace ss
