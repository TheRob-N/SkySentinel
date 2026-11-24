// Sub-GHz band watch on the T-Embed CC1101 (built with -DSS_SUBGHZ). Its own task steps
// the CC1101 through the bands in ss_subghz.h, about 100 ms each, sampling the signal
// level; SubGhzWatch turns that into noise floors, bursts and jamming alarms. Receive
// only: the CC1101 is never put in transmit.
#pragma once

#include <Arduino.h>

#include "ss_proto.h"
#include "ss_subghz.h"
#include "ss_tpms.h"

namespace ss {
namespace subghz {

// Category mask: the watch runs while CAT_PENTEST (jammers) or CAT_VEHICLE is on.
bool begin(volatile CatMask* category_mask);
bool ready();
bool running();
// A jamming alarm for the master (EV_RF_JAM), once.
bool pollEvent(Event& out);
// Read-only view for the screen (values may be a sweep old).
const SubGhzWatch& watch();
uint8_t currentBand();
uint32_t sweeps();
// Settings (T-Embed settings menu): which bands to watch (bit per kSubGhzBandList entry),
// burst sensitivity, and whether jamming is sent to the master as an alert.
void setBandMask(uint8_t mask);
uint8_t bandMask();
void setBurstDb(int8_t db);
// A burst needs this many loud samples (of 40, 2.5 ms each) in one look.
void setBurstSamples(uint8_t n);
void setJamAlerts(bool on);
// Bursts heard since the last call, oldest first (for the device list on the master).
bool nextBurst(SubGhzBurst& out);

// HUNT: park the radio on one band (kSubGhzBandList number; 0xFF = back to watching
// every band) and read its strongest sample about 10 times a second.
void setHunt(uint8_t band);
// The latest look on the hunted band: strongest sample (dBm) and when (millis).
int8_t huntPeak(uint32_t* at_ms = nullptr);

// History: kHistSlots slots of kHistSlotMs per band, oldest first. peak = the loudest
// look's level in the slot (the burst level, -127 = not watched then), bursts = how many.
constexpr uint8_t kHistSlots = 60;
constexpr uint32_t kHistSlotMs = 10000;   // 60 x 10 s = the last 10 minutes
void history(uint8_t band, int8_t peak[kHistSlots], uint8_t bursts[kHistSlots]);
// Tire pressure sensors (ss_tpms.h): after each sweep of the bands the radio listens for
// them for about a second (FSK sensors, then Schrader's OOK ones) while the Vehicles
// category is on. 0 = off, 1 = 315 MHz (North America), 2 = 433.92 MHz (Europe, Asia).
void setTpms(uint8_t band);
uint8_t tpms();
struct TpmsHeard { TpmsReading r; int8_t rssi; uint8_t band; };   // band: kSubGhzBandList number
bool nextTpms(TpmsHeard& out);
uint32_t tpmsWindows();   // signals the carrier sense caught while listening
uint32_t tpmsDecoded();   // tire readings decoded

// Before deep sleep / power off: puts the CC1101 to sleep and keeps the SPI bus so the
// watch task stops using it. Draw on the screen before calling this.
void shutdown();

}  // namespace subghz
}  // namespace ss
