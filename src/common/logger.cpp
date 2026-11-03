#include "logger.h"

#include <LittleFS.h>
#include <Preferences.h>

#include "core_common.h"

namespace ss {
namespace logger {

static constexpr const char* kCur = "/log.csv";
static constexpr const char* kOld = "/log1.csv";
static constexpr size_t kMaxFile = 384 * 1024;
static constexpr uint32_t kFlushMs = 5000;
static constexpr uint32_t kPosMaxAgeMs = 120000;    // older than this: not "where we are" any more
static constexpr uint32_t kTrackEveryMs = 60000;
static constexpr int32_t kTrackMoveE7 = 450;        // ~50 m

static bool started_ = false, fsOk_ = false, enabled_ = true;
static Preferences* prefs_ = nullptr;
static Sink sink_ = nullptr;
static uint32_t seq_ = 0, written_ = 0, dropped_ = 0, boot_ = 0;
static String pending_;
static uint32_t lastFlush_ = 0;
static bool havePos_ = false;
static int32_t lat_ = 0, lon_ = 0;
static uint32_t posMs_ = 0;
static uint32_t utcBase_ = 0;  // utc - millis()/1000, 0 = unknown
static bool clockGps_ = false;  // false: the clock came from a browser (or nowhere)
static int32_t trackLat_ = 0, trackLon_ = 0;
static uint32_t trackMs_ = 0;
static bool tracked_ = false;
static size_t flashBytes_ = 0;   // both files, refreshed after each write

static size_t fileSize(const char* path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  const size_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

static void startFile() {
  File f = LittleFS.open(kCur, "w");
  if (f) { f.print(kLogCsvHeader); f.close(); }
}

static void flush() {
  if (!fsOk_ || !pending_.length()) { pending_ = ""; return; }
  if (fileSize(kCur) + pending_.length() > kMaxFile) {
    LittleFS.remove(kOld);
    LittleFS.rename(kCur, kOld);
    startFile();
  }
  if (!LittleFS.exists(kCur)) startFile();
  File f = LittleFS.open(kCur, "a");
  if (!f || f.print(pending_) != pending_.length()) {
    ++dropped_;
    Out.println("[log] flash write failed");
  }
  if (f) f.close();
  pending_ = "";
  flashBytes_ = fileSize(kCur) + fileSize(kOld);
}

static void fill(LogRec& r, uint8_t kind, uint32_t now) {
  r.seq = ++seq_;
  r.kind = kind;
  r.up_s = now / 1000;
  r.utc = utcNow(now);
  if (havePos_ && now - posMs_ < kPosMaxAgeMs) {
    r.has_pos = true;
    r.lat_e7 = lat_;
    r.lon_e7 = lon_;
  }
}

static void put(const LogRec& r) {
  if (!started_ || !enabled_) return;
  char line[200];
  const size_t n = formatLogCsv(r, line, sizeof(line));
  if (n) {
    pending_ += line;
    ++written_;
    if (pending_.length() > 2048) flush();
  }
  if (sink_) sink_(r);
}

static void fromDevice(LogRec& r, const Device& d) {
  r.cat = d.category;
  r.sig = d.sig_id;
  r.conf = d.conf;
  r.strong = d.flags & SF_STRONG_MATCH;
  r.rssi = d.rssi_last;
  r.has_mac = true;
  memcpy(r.mac, d.addr, 6);
  strncpy(r.name, d.name, kLogName);
  r.name[kLogName] = '\0';
}

void begin(Preferences* prefs) {
  prefs_ = prefs;
  enabled_ = !prefs || prefs->getBool("logon", true);
  boot_ = prefs ? prefs->getUInt("logboot", 0) + 1 : 0;
  if (prefs) prefs->putUInt("logboot", boot_);
  fsOk_ = LittleFS.begin(true, "/littlefs", 5, "spiffs");
  if (!fsOk_) Out.println("[log] no flash file system: logging to the dashboard nodes only");
  else if (!LittleFS.exists(kCur)) startFile();
  if (fsOk_) flashBytes_ = fileSize(kCur) + fileSize(kOld);
  started_ = true;
  LogRec r;
  fill(r, LK_START, millis());
  r.a = (uint16_t)boot_;
  put(r);
  flush();
  Out.printf("[log] logging %s (%u KB in flash)\n", enabled_ ? "on" : "off",
             (unsigned)((fileSize(kCur) + fileSize(kOld)) / 1024));
}

bool started() { return started_; }
void setSink(Sink sink) { sink_ = sink; }

void setPosition(int32_t lat, int32_t lon, uint32_t now) {
  havePos_ = true;
  lat_ = lat;
  lon_ = lon;
  posMs_ = now;
}

void setClock(uint32_t utc, uint32_t now) {
  if (!utc) return;
  utcBase_ = utc - now / 1000;
  clockGps_ = true;
}

// The web page's clock (phone or PC), until a GPS gives a better one.
void setClockFromHost(uint32_t utc, uint32_t now) {
  if (!utc || clockGps_ || utc < 1700000000u) return;
  utcBase_ = utc - now / 1000;
}

uint32_t utcNow(uint32_t now) { return utcBase_ ? utcBase_ + now / 1000 : 0; }

void device(const Device& d, bool upgraded) {
  if (!started_ || !(kSurveillanceMask & catBit(d.category))) return;
  LogRec r;
  fill(r, LK_DEVICE, millis());
  fromDevice(r, d);
  r.a = upgraded ? 1 : 0;
  put(r);
}

void event(const Event& ev) {
  if (!started_) return;
  LogRec r;
  const uint8_t kind = ev.type == EV_BLE_SPAM ? LK_BLE_SPAM : ev.type == EV_WIFI_SPAM ? LK_WIFI_SPAM
                     : ev.type == EV_EVIL_TWIN ? LK_EVIL_TWIN : ev.type == EV_RF_JAM ? LK_RF_JAM : ev.type == EV_DEAUTH_FLOOD ? LK_DEAUTH : LK_DISASSOC;
  fill(r, kind, millis());
  r.has_mac = true;
  memcpy(r.mac, ev.src, 6);
  if (kind == LK_BLE_SPAM || kind == LK_WIFI_SPAM) {
    r.a = ev.count;
    r.rssi = (int8_t)ev.channel;  // the spam event carries RSSI there
  } else if (kind == LK_RF_JAM) {
    r.a = ev.count;
    r.b = ev.dst[0];
  } else if (kind == LK_EVIL_TWIN) {
    r.a = ev.count;
    r.b = ev.channel;
  } else {
    r.a = ev.channel;
    r.b = ev.count;
  }
  put(r);
  flush();  // rare and important: don't wait
}

void follow(const Device& d, const FollowAlert& a) {
  if (!started_) return;
  LogRec r;
  fill(r, a.lingering ? LK_LINGERING : LK_FOLLOWING, millis());
  fromDevice(r, d);
  r.a = a.places;
  r.b = (uint16_t)(a.with_us_ms / 60000);
  put(r);
  flush();
}

void poll(uint32_t now) {
  if (!started_) return;
  if (pending_.length() && now - lastFlush_ >= kFlushMs) {
    flush();
    lastFlush_ = now;
  }
  if (!enabled_ || !havePos_ || now - posMs_ > kPosMaxAgeMs) return;
  if (tracked_ && now - trackMs_ < kTrackEveryMs) return;
  const int32_t dl = lat_ - trackLat_, dn = lon_ - trackLon_;
  if (tracked_ && abs(dl) < kTrackMoveE7 && abs(dn) < kTrackMoveE7) return;
  tracked_ = true;
  trackMs_ = now;
  trackLat_ = lat_;
  trackLon_ = lon_;
  LogRec r;
  fill(r, LK_TRACK, now);
  put(r);
}

// Raw bytes to USB, waiting for the host to keep up (Out would drop them).
static bool usbWrite(const char* p, size_t n) {
  const uint32_t t0 = millis();
  while (n) {
    const size_t w = Serial.write((const uint8_t*)p, n);
    p += w;
    n -= w;
    if (n) {
      if (millis() - t0 > 2000) return false;
      delay(1);
    }
  }
  return true;
}

static void dumpFile(const char* path, bool skipHeader, uint32_t& lines, bool& ok) {
  if (!ok || !LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  bool first = true;
  String line;
  char out[420];
  while (ok && f.available()) {
    line = f.readStringUntil('\n');
    if (first && skipHeader) { first = false; continue; }
    first = false;
    if (!line.length()) continue;
    // {"t":"log","l":"..."} with quotes and backslashes escaped
    size_t o = 0;
    const char* pre = "{\"t\":\"log\",\"l\":\"";
    memcpy(out, pre, strlen(pre));
    o = strlen(pre);
    for (size_t i = 0; i < line.length() && o < sizeof(out) - 8; ++i) {
      const char c = line[i];
      if (c == '"' || c == '\\') out[o++] = '\\';
      if ((uint8_t)c >= 0x20) out[o++] = c;
    }
    memcpy(out + o, "\"}\n", 3);
    o += 3;
    ok = usbWrite(out, o);
    if ((++lines & 63) == 0) delay(1);
  }
  f.close();
}

void streamCsv(Print& out) {
  flush();
  if (!fsOk_) { out.print(kLogCsvHeader); return; }
  bool header = true;
  for (const char* path : {kOld, kCur}) {
    if (!LittleFS.exists(path)) continue;
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    uint8_t buf[1024];
    bool firstLine = true;
    while (f.available()) {
      size_t n = f.read(buf, sizeof(buf));
      size_t off = 0;
      if (!header && firstLine) {  // the second file's header line
        while (off < n && buf[off] != '\n') ++off;
        if (off < n) { ++off; firstLine = false; }
        else continue;
      }
      firstLine = false;
      if (n > off && out.write(buf + off, n - off) == 0) { f.close(); return; }
    }
    f.close();
    header = false;
  }
}

bool handleCommand(const String& line) {
  if (line.startsWith("clock ")) {  // "clock <unix seconds>", sent by the web page when it connects
    if (started_) setClockFromHost((uint32_t)strtoul(line.c_str() + 6, nullptr, 10), millis());
    return true;
  }
  if (!line.startsWith("log")) return false;
  if (line != "log" && line[3] != ' ') return false;
  const String arg = line.length() > 4 ? line.substring(4) : String("status");
  if (!started_) { Out.println("[log] this board doesn't keep the log (the master does)"); return true; }
  if (arg == "dump") {
    flush();
    uint32_t lines = 0;
    bool ok = usbWrite("{\"t\":\"log_begin\"}\n", 18);
    if (ok) {
      char h[160];
      snprintf(h, sizeof(h), "{\"t\":\"log\",\"l\":\"%.*s\"}\n", (int)strlen(kLogCsvHeader) - 1, kLogCsvHeader);
      ok = usbWrite(h, strlen(h));
    }
    dumpFile(kOld, true, lines, ok);
    dumpFile(kCur, true, lines, ok);
    char end[64];
    snprintf(end, sizeof(end), "{\"t\":\"log_end\",\"lines\":%lu,\"ok\":%s}\n", (unsigned long)lines, ok ? "true" : "false");
    usbWrite(end, strlen(end));
  } else if (arg == "clear") {
    pending_ = "";
    if (fsOk_) { LittleFS.remove(kOld); startFile(); flashBytes_ = fileSize(kCur); }
    written_ = 0;
    Out.println("{\"t\":\"ack\",\"cmd\":\"log clear\"}");
    Out.println("[log] cleared");
  } else if (arg == "on" || arg == "off") {
    enabled_ = arg == "on";
    if (prefs_) prefs_->putBool("logon", enabled_);
    Out.printf("[log] logging %s\n", enabled_ ? "on" : "off");
  } else {
    flush();
    Out.printf("[log] %s, %lu records since power on, %u KB in flash of %u KB, clock %s, position %s\n",
               enabled_ ? "on" : "off", (unsigned long)written_,
               (unsigned)((fileSize(kCur) + fileSize(kOld)) / 1024),
               fsOk_ ? (unsigned)(LittleFS.totalBytes() / 1024) : 0u, utcBase_ ? (clockGps_ ? "from GPS" : "from the web page's computer") : "unknown (no GPS date yet)",
               havePos_ && millis() - posMs_ < kPosMaxAgeMs ? "from GPS" : "none");
    Out.println("[log] commands: log status | log dump (USB) | log clear | log on | log off; over Wi-Fi: /log.csv");
  }
  return true;
}

void status(Print& out, uint32_t now) {
  if (!started_) return;
  out.printf(",\"log\":{\"on\":%s,\"flash\":%s,\"recs\":%lu,\"kb\":%u,\"clock\":%s,\"pos\":%s}", enabled_ ? "true" : "false",
             fsOk_ ? "true" : "false", (unsigned long)written_, (unsigned)((flashBytes_ + pending_.length()) / 1024),
             utcBase_ ? (clockGps_ ? "\"gps\"" : "\"browser\"") : "false", havePos_ && now - posMs_ < kPosMaxAgeMs ? "true" : "false");
}

}  // namespace logger
}  // namespace ss
