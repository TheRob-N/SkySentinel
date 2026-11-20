#include "ss_gps.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ss_proto.h"

namespace ss {

static constexpr uint32_t kStaleMs = 5000;

const char* gnssName(uint8_t s) {
  switch (s) {
    case GNSS_GPS: return "GPS";
    case GNSS_GLONASS: return "GLONASS";
    case GNSS_GALILEO: return "Galileo";
    case GNSS_BEIDOU: return "BeiDou";
    case GNSS_QZSS: return "QZSS";
    default: return "?";
  }
}

static uint8_t talkerSys(const char* t) {
  if (t[0] == 'G' && t[1] == 'P') return GNSS_GPS;
  if (t[0] == 'G' && t[1] == 'L') return GNSS_GLONASS;
  if (t[0] == 'G' && t[1] == 'A') return GNSS_GALILEO;
  if ((t[0] == 'B' && t[1] == 'D') || (t[0] == 'G' && t[1] == 'B')) return GNSS_BEIDOU;
  if (t[0] == 'G' && t[1] == 'Q') return GNSS_QZSS;
  return GNSS_UNKNOWN;  // GN = combined
}

const char* gpsStateName(uint8_t s) {
  switch (s) {
    case GPS_SILENT: return "silent";
    case GPS_SEARCHING: return "searching";
    case GPS_FIX_2D: return "2D";
    case GPS_FIX_3D: return "3D";
    default: return "none";
  }
}

static int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// "4916.45" + "N" -> 49.274167
static double coord(const char* v, const char* hemi) {
  if (!v || !*v) return 0;
  const double raw = atof(v);
  const int deg = (int)(raw / 100);
  double d = deg + (raw - deg * 100) / 60.0;
  if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
  return d;
}

void Nmea::feed(char c, uint32_t now_ms) {
  if (c == '$') { in_ = true; len_ = 0; return; }
  if (!in_) return;
  if (c == '\r' || c == '\n') {
    in_ = false;
    buf_[len_] = '\0';
    sentence(now_ms);
    return;
  }
  if (len_ >= sizeof(buf_) - 1) { in_ = false; return; }  // too long: not NMEA
  buf_[len_++] = c;
}

void Nmea::sentence(uint32_t now_ms) {
  // Checksum: XOR of everything between '$' and '*'
  char* star = strchr(buf_, '*');
  if (!star || star[1] == '\0' || star[2] == '\0') { ++bad_; return; }
  uint8_t x = 0;
  for (char* p = buf_; p < star; ++p) x ^= (uint8_t)*p;
  const int hi = hex(star[1]), lo = hex(star[2]);
  if (hi < 0 || lo < 0 || ((hi << 4) | lo) != x) { ++bad_; return; }
  *star = '\0';
  ++good_;
  heard_ = true;
  last_ms_ = now_ms;
  if (strlen(buf_) < 5) return;

  // Split into fields (empty fields kept)
  const char* f[24] = {nullptr};
  int n = 0;
  char* p = buf_;
  f[n++] = p;
  for (; *p && n < 24; ++p)
    if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
  const char* type = f[0] + 2;  // after the talker id (GP, GN, GL, BD, ...)

  if (!strcmp(type, "GGA") && n >= 10) {
    quality_ = (uint8_t)atoi(f[6]);
    sats_ = (uint8_t)atoi(f[7]);
    hdop_ = (float)atof(f[8]);
    if (quality_ > 0 && *f[2] && *f[4]) {
      lat_ = coord(f[2], f[3]);
      lon_ = coord(f[4], f[5]);
      fixed_ = true;
      fix_ms_ = now_ms;
      has_alt_ = *f[9] != 0;
      if (has_alt_) alt_ = (float)atof(f[9]);
    } else {
      fixed_ = false;
    }
  } else if (!strcmp(type, "RMC") && n >= 10) {
    if (f[2][0] == 'A' && *f[7]) {
      kmh_ = (float)atof(f[7]) * 1.852f;   // knots
      course_ = *f[8] ? (float)atof(f[8]) : 0;
      has_motion_ = true;
    }
    // hhmmss(.ss), status, ..., ddmmyy in field 9
    const char* t = f[1];
    const char* d = f[9];
    if (strlen(t) >= 6 && strlen(d) == 6) {
      auto two = [](const char* s) { return (s[0] - '0') * 10 + (s[1] - '0'); };
      const int yy = two(d + 4);
      const int mo = two(d + 2), dd = two(d), hh = two(t), mi = two(t + 2), ss = two(t + 4);
      if (yy >= 24 && yy < 80 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh < 24 && mi < 60 && ss < 61) {
        utc_ = unixTime(2000 + yy, mo, dd, hh, mi, ss);
        utc_ms_ = now_ms;
      }
    }
  } else if (!strcmp(type, "GSA") && n >= 3) {
    // One GSA per constellation on multi-GNSS receivers: the best one wins this second.
    const uint8_t m = (uint8_t)atoi(f[2]);
    if (now_ms - mode_ms_ > 500 || m > mode_) mode_ = m;
    mode_ms_ = now_ms;
    // Satellites used in the fix: fields 3..14. NMEA 4.10 adds the system id in field 18.
    uint8_t sys = talkerSys(f[0]);
    if (n >= 19 && *f[18]) {
      const int id = atoi(f[18]);
      if (id >= 1 && id <= 5) sys = (uint8_t)id;   // same numbering as GnssSys
    }
    for (int i = 3; i <= 14 && i < n; ++i) {
      if (!*f[i]) continue;
      const int prn = atoi(f[i]);
      if (prn <= 0 || prn > 255) continue;
      size_t k = 0;
      for (; k < n_used_; ++k) if (used_[k].sys == sys && used_[k].prn == prn) break;
      if (k == n_used_) {
        if (n_used_ < 32) ++n_used_;
        else { k = 0; for (size_t j = 1; j < 32; ++j) if (used_[j].ms < used_[k].ms) k = j; }
      }
      used_[k] = Used{sys, (uint8_t)prn, now_ms};
    }
  } else if (!strcmp(type, "GSV") && n >= 4) {
    // total msgs, msg no, sats in view, then up to 4 x (prn, elevation, azimuth, snr)
    const uint8_t sys = talkerSys(f[0]);
    for (int i = 4; i + 3 < n; i += 4) {
      if (!*f[i]) continue;
      const int prn = atoi(f[i]);
      if (prn <= 0 || prn > 255) continue;
      const int el = atoi(f[i + 1]), az = atoi(f[i + 2]), snr = *f[i + 3] ? atoi(f[i + 3]) : 0;
      noteSat(sys, (uint8_t)prn, (uint8_t)(el < 0 ? 0 : el > 90 ? 90 : el), (uint16_t)(az < 0 ? 0 : az % 360),
              (uint8_t)(snr < 0 ? 0 : snr > 99 ? 99 : snr), now_ms);
    }
  }
}

uint8_t Nmea::state(uint32_t now_ms) const {
  if (!heard_ || now_ms - last_ms_ > kStaleMs) return GPS_SILENT;
  if (!fixed_ || now_ms - fix_ms_ > kStaleMs) return GPS_SEARCHING;
  if (now_ms - mode_ms_ <= kStaleMs && mode_ == 2) return GPS_FIX_2D;
  return GPS_FIX_3D;
}

bool Nmea::position(double& lat, double& lon) const {
  if (!fixed_) return false;
  lat = lat_;
  lon = lon_;
  return true;
}

void Nmea::noteSat(uint8_t sys, uint8_t prn, uint8_t elev, uint16_t az, uint8_t snr, uint32_t now_ms) {
  GpsSat* slot = nullptr;
  for (auto& s : sats_v_)
    if (s.ms && s.sys == sys && s.prn == prn) { slot = &s; break; }
  if (!slot)
    for (auto& s : sats_v_)
      if (!s.ms || now_ms - s.ms > 10000) { slot = &s; break; }
  if (!slot) {  // full: replace the one heard longest ago
    slot = &sats_v_[0];
    for (auto& s : sats_v_) if (s.ms < slot->ms) slot = &s;
  }
  slot->sys = sys; slot->prn = prn; slot->elev = elev; slot->az = az; slot->snr = snr;
  slot->ms = now_ms ? now_ms : 1;
}

size_t Nmea::satellites(GpsSat* out, size_t max, uint32_t now_ms) const {
  size_t n = 0;
  for (const auto& s : sats_v_) {
    if (!s.ms || now_ms - s.ms > 10000 || n >= max) continue;
    out[n] = s;
    out[n].used = false;
    for (size_t k = 0; k < n_used_; ++k)
      if (used_[k].prn == s.prn && now_ms - used_[k].ms <= 3000 &&
          (used_[k].sys == s.sys || used_[k].sys == GNSS_UNKNOWN || s.sys == GNSS_UNKNOWN))
        out[n].used = true;
    ++n;
  }
  return n;
}

bool Nmea::altitude(float& m) const {
  if (!fixed_ || !has_alt_) return false;
  m = alt_;
  return true;
}

bool Nmea::motion(float& kmh, float& course) const {
  if (!fixed_ || !has_motion_) return false;
  kmh = kmh_;
  course = course_;
  return true;
}

void fillGpsInfo(const Nmea& g, uint32_t now, GpsInfo& o) {
  o = GpsInfo();
  o.state = g.state(now);
  o.sats_used = g.sats();
  const float h = g.hdop() * 10.0f;
  o.hdop10 = h <= 0 ? 0 : h >= 255 ? 255 : (uint8_t)(h + 0.5f);
  double la, lo;
  if (o.state >= GPS_FIX_2D && g.position(la, lo)) {
    o.has_pos = true;
    o.lat_e7 = (int32_t)(la * 1e7);
    o.lon_e7 = (int32_t)(lo * 1e7);
  }
  float a;
  if (o.state >= GPS_FIX_3D && g.altitude(a)) { o.has_alt = true; o.alt_m = (int16_t)(a < -32000 ? -32000 : a > 32000 ? 32000 : a); }
  float k, c;
  if (o.state >= GPS_FIX_2D && g.motion(k, c)) {
    o.has_motion = true;
    o.kmh10 = (uint16_t)(k * 10 > 65000 ? 65000 : k * 10 + 0.5f);
    o.course10 = (uint16_t)(c * 10 + 0.5f);
  }
  g.utc(now, o.utc);
  GpsSat s[kGpsSats];
  const size_t n = g.satellites(s, kGpsSats, now);
  o.n_sat = (uint8_t)n;
  for (size_t i = 0; i < n; ++i)
    o.sat[i] = GpsInfo::Sat{s[i].sys, s[i].prn, s[i].elev, (uint8_t)(s[i].az / 2), s[i].snr, s[i].used};
}

bool Nmea::utc(uint32_t now_ms, uint32_t& unix_s) const {
  if (!utc_) return false;
  unix_s = utc_ + (now_ms - utc_ms_) / 1000;
  return true;
}

uint32_t unixTime(int y, int m, int d, int hh, int mm, int ss) {
  // Howard Hinnant's days_from_civil
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + (long)doe - 719468;
  return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

void formatUtc(uint32_t t, char out[20]) {
  const long z0 = (long)(t / 86400) + 719468;
  const long era = z0 / 146097;
  const unsigned doe = (unsigned)(z0 - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const long y = (long)yoe + era * 400 + (m <= 2);
  const uint32_t s = t % 86400;
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%04ld-%02u-%02u %02lu:%02lu:%02lu", y % 10000, m, d, (unsigned long)(s / 3600),
           (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  memcpy(out, tmp, 19);
  out[19] = '\0';
}

// ---------------------------------------------------------------------------
// Jamming and spoofing

void gpsSnapshot(const Nmea& g, uint32_t now_ms, GpsSnap& o) {
  o = GpsSnap();
  o.state = g.state(now_ms);
  o.alive = o.state >= GPS_SEARCHING;
  GpsSat v[48];
  const size_t n = g.satellites(v, 48, now_ms);
  uint8_t snr[48];
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!v[i].snr) continue;
    snr[k++] = v[i].snr;
    if (v[i].used) ++o.used;
  }
  o.tracked = (uint8_t)k;
  for (size_t i = 1; i < k; ++i)   // strongest first
    for (size_t j = i; j > 0 && snr[j] > snr[j - 1]; --j) { const uint8_t t = snr[j]; snr[j] = snr[j - 1]; snr[j - 1] = t; }
  if (k >= 4) o.top4 = (snr[0] + snr[1] + snr[2] + snr[3]) / 4.0f;
  if (k >= 6) o.spread = (float)(snr[0] - snr[k - 1]);
  if (o.state >= GPS_FIX_2D) o.has_pos = g.position(o.lat, o.lon);
  o.has_utc = g.utc(now_ms, o.utc);
}

const char* gpsSpoofWhyName(uint8_t why) {
  switch (why) {
    case SPOOF_JUMP: return "position jump";
    case SPOOF_CLOCK: return "clock jump";
    case SPOOF_UNIFORM: return "all satellites equally strong";
    case SPOOF_STRONG: return "satellites too strong";
    default: return "unknown";
  }
}

double gpsDistanceM(double lat1, double lon1, double lat2, double lon2) {
  const double kR = 6371000.0, d2r = 3.14159265358979323846 / 180.0;
  const double dlat = (lat2 - lat1) * d2r, dlon = (lon2 - lon1) * d2r;
  const double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dlon / 2) * sin(dlon / 2);
  return 2 * kR * atan2(sqrt(a), sqrt(1 - a));
}

bool GpsWatch::update(const GpsSnap& s, uint32_t now, Event& ev) {
  if (!s.alive) {   // receiver quiet (unplugged, rebooting): nothing to judge, start over
    low_since_ = 0;
    had_pos_ = had_utc_ = false;
    return false;
  }
  const bool healthy = s.state >= GPS_FIX_2D && s.tracked >= 4 && s.top4 > 0;

  // --- Jamming: the strongest satellites fall well under their normal level together,
  // or almost all of them vanish, while the receiver keeps talking.
  if (healthy && !jammed_) {   // learn what normal looks like (about the last 5 minutes)
    base_ = base_n_ ? base_ + (s.top4 - base_) / (base_n_ < 300 ? (float)(base_n_ + 1) : 300.0f) : s.top4;
    base_trk_ = base_n_ ? base_trk_ + (s.tracked - base_trk_) / 8.0f : s.tracked;
    ++base_n_;
    base_tracked_ = (uint8_t)(base_trk_ + 0.5f);
  }
  const bool learned = base_n_ >= 60 && base_ >= 30;
  const float now4 = s.top4;   // 0 when fewer than 4 are left: counts as fully drowned
  const bool low = learned && (now4 <= base_ - kJamDropDb || (s.tracked <= 1 && base_tracked_ >= 5));
  if (low) {
    if (!low_since_) low_since_ = now ? now : 1;
    if (!jammed_ && now - low_since_ >= kJamHoldMs && rearmed(0, now)) {
      jammed_ = true;
      ok_since_ = 0;
      last_[0] = now ? now : 1;
      ++alarms_;
      ev = Event();
      ev.type = EV_GPS_JAM;
      ev.channel = base_tracked_;
      const float lost = base_ - now4;
      ev.count = (uint16_t)(lost < 0 ? 0 : lost + 0.5f);
      ev.dst[0] = s.tracked;
      return true;
    }
  } else {
    low_since_ = 0;
    if (jammed_) {   // back to normal for a minute: over
      if (!ok_since_) ok_since_ = now ? now : 1;
      if (healthy && now - ok_since_ >= 60000) jammed_ = false;
    }
  }

  // --- Spoofing
  auto spoof = [&](uint8_t why, uint32_t detail) {
    last_[why] = now ? now : 1;
    ++alarms_;
    ev = Event();
    ev.type = EV_GPS_SPOOF;
    ev.channel = why;
    ev.count = (uint16_t)(detail > 0xFFFF ? 0xFFFF : detail);
    ev.dst[0] = s.tracked;
    return true;
  };
  bool fire = false;
  if (s.has_pos) {
    if (had_pos_ && now - pos_ms_ >= 1000 && now - pos_ms_ <= 10000) {
      const double m = gpsDistanceM(lat_, lon_, s.lat, s.lon);
      const double kmh = m / ((now - pos_ms_) / 1000.0) * 3.6;
      if (kmh > 1200 && m > 500 && rearmed(SPOOF_JUMP, now)) fire = spoof(SPOOF_JUMP, (uint32_t)kmh);
    }
    had_pos_ = true;
    lat_ = s.lat; lon_ = s.lon; pos_ms_ = now;
  } else if (had_pos_ && now - pos_ms_ > 10000) {
    had_pos_ = false;   // fix lost for a while: compare afresh when it's back
  }
  if (s.has_utc) {
    const int64_t off = (int64_t)s.utc - (int64_t)(now / 1000);
    if (had_utc_) {
      const int64_t d = off - utc_off_;
      const int64_t ad = d < 0 ? -d : d;
      if (ad > 5 && !fire && rearmed(SPOOF_CLOCK, now)) fire = spoof(SPOOF_CLOCK, (uint32_t)ad);
      // Follow it slowly (our millis() drifts a little), but a jump is reported once.
      utc_off_ = ad > 5 ? off : utc_off_;
    } else {
      utc_off_ = off;
      had_utc_ = true;
    }
  }
  if (!fire && s.tracked >= 6 && s.spread > 0 && s.spread <= 3 && s.top4 >= 40 && rearmed(SPOOF_UNIFORM, now))
    fire = spoof(SPOOF_UNIFORM, (uint32_t)(s.spread * 10 + 0.5f));
  if (!fire && s.top4 >= 56 && rearmed(SPOOF_STRONG, now)) fire = spoof(SPOOF_STRONG, (uint32_t)(s.top4 + 0.5f));
  return fire;
}

}  // namespace ss
