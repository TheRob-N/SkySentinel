// Detection log: one record per thing worth keeping (a surveillance device first seen,
// a following alert, a flood), with the time and place when a GPS board knows them.
// The master writes records to its own flash and forwards each one to dashboard
// nodes (CYD), which write them to an SD card. Same CSV either way.
// Pure C++, host tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {

enum LogKind : uint8_t {
  LK_NONE = 0,
  LK_START = 1,      // the master started logging (power on)
  LK_DEVICE = 2,     // a surveillance device first seen, or it became a confident match
  LK_FOLLOWING = 3,
  LK_LINGERING = 4,
  LK_DEAUTH = 5,
  LK_DISASSOC = 6,
  LK_BLE_SPAM = 7,
  LK_TRACK = 8,      // where we were (every couple of minutes while moving)
  LK_WIFI_SPAM = 9,  // beacon flood: fake Wi-Fi networks from one transmitter
  LK_EVIL_TWIN = 10, // one radio answering as many network names (Pineapple-style)
  LK_RF_JAM = 11,    // a sub-GHz band jammed (T-Embed CC1101)
};
const char* logKindName(uint8_t k);  // "start", "device", "following", ...

constexpr size_t kLogName = 20;

struct LogRec {
  uint32_t seq = 0;       // master's record counter since boot (gaps = records lost on the radio)
  uint32_t utc = 0;       // Unix seconds, 0 = unknown (no GPS date yet)
  uint32_t up_s = 0;      // master uptime
  bool has_pos = false;
  int32_t lat_e7 = 0, lon_e7 = 0;
  uint8_t kind = LK_NONE;
  uint8_t cat = 0;
  uint16_t sig = 0;
  uint8_t conf = 0;
  bool strong = false;
  int8_t rssi = -127;
  bool has_mac = false;
  uint8_t mac[6] = {0};
  uint16_t a = 0, b = 0;  // kind-specific: device 0 new / 1 upgraded; following places, minutes;
                          // flood channel, count; spam count
  char name[kLogName + 1] = {0};
};

size_t encodeLogRec(const LogRec& m, uint8_t* out, size_t cap);
bool decodeLogRec(const uint8_t* p, size_t n, LogRec& m);

// First line of every log file.
extern const char kLogCsvHeader[];
// One CSV line, '\n' included. Returns its length (0 if it didn't fit).
size_t formatLogCsv(const LogRec& r, char* out, size_t cap);

}  // namespace ss
