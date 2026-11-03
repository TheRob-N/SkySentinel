// Detection log on the master: what was found, when and where.
//
// Records (ss_log.h) go to three places:
//  * the master's own flash (LittleFS on the "spiffs" partition), as CSV. Two files
//    of up to 384 KB each, the older one dropped when the newer fills, so the last
//    several thousand records survive a power cut;
//  * every dashboard node (CYD) in the cluster, which writes them to its SD card;
//  * the web UI: SETTINGS > LOG downloads the flash copy (over USB with "log dump",
//    over Wi-Fi from /log.csv).
//
// Time and place come from a GPS board in the cluster (the Heltec), sent in its
// heartbeat. Without one the record has the master's uptime only.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "ss_classify.h"
#include "ss_devices.h"
#include "ss_follow.h"
#include "ss_log.h"
#include "ss_proto.h"

namespace ss {
namespace logger {

using Sink = void (*)(const LogRec& r);

// Mounts flash and writes a "start" record. Only the master (or a solo board) calls it;
// until then every call below does nothing.
void begin(Preferences* prefs);
bool started();
// Forward every record here too (the master sends it to dashboard nodes).
void setSink(Sink sink);

// Where we are and what time it is, from a GPS (local or a node's heartbeat).
void setPosition(int32_t lat_e7, int32_t lon_e7, uint32_t now_ms);
void setClock(uint32_t utc, uint32_t now_ms);
uint32_t utcNow(uint32_t now_ms);  // 0 = unknown
// The connected web page's clock: used until a GPS clock arrives (which always wins).
void setClockFromHost(uint32_t utc, uint32_t now_ms);

void device(const Device& d, bool upgraded);
void event(const Event& ev);
void follow(const Device& d, const FollowAlert& a);

// Flushes to flash every few seconds, adds a track point while moving.
void poll(uint32_t now_ms);
// "log status|dump|clear|on|off". True if it was a log command.
bool handleCommand(const String& line);
// ,"log":{...} for the status JSON.
void status(Print& out, uint32_t now_ms);
// The whole log as CSV (older file first), for GET /log.csv.
void streamCsv(Print& out);

}  // namespace logger
}  // namespace ss
