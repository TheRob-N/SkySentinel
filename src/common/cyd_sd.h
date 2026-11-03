// CYD SD card: writes the master's detection log (MSG_LOG records, ss_log.h) as CSV.
// Both CYD boards wire the card slot to GPIO 18 (SCK), 19 (MISO), 23 (MOSI), 5 (CS),
// on the ESP32's VSPI bus. FAT32 cards up to 32 GB.
//
// Files: /SkySentinel/2026-10-30.csv, one per UTC day, once a GPS board in the
// cluster has given the time; /SkySentinel/undated.csv before that.
// Built only with -DSS_CYD.
#pragma once

#include <Arduino.h>

#include "ss_fileserve.h"
#include "ss_log.h"
#include "ss_proto.h"

namespace ss {
namespace cydsd {

void begin();
void onRecord(const LogRec& r);
// Writes what's waiting, notices a card being put in or pulled out. Call every loop.
void loop(uint32_t now_ms);
uint8_t state();           // SdState (ss_proto.h)
uint32_t saved();          // records written since power on
uint32_t missed();         // records lost on the radio (gaps in the master's numbering)
const char* fileName();    // "" until something was written
// Serial: "sd" / "sd status" / "sd ls" (JSON list) / "sd get <file>" (streams it as
// {"t":"log"} lines, like the master's "log dump") / "sd format" (asks) / "sd format yes"
bool handleCommand(const String& line);

// Erase and format the card (FAT32). Runs from loop() about half a second later, so the
// screen can say "formatting" first; it blocks for a few seconds to a minute.
void requestFormat();
bool formatPending();
// Result of the last format since power on ("" = none yet) and whether it worked.
const char* formatResult();
bool formatOk();
uint32_t cardMB();     // 0 = no card
// The card's log folder, for remote downloads through the master (ss_fileserve.h)
FileSource& files();

}  // namespace cydsd
}  // namespace ss
