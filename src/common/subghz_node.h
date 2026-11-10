// Sub-GHz band watch on the T-Embed CC1101 (built with -DSS_SUBGHZ). Its own task steps
// the CC1101 through the bands in ss_subghz.h, about 100 ms each, sampling the signal
// level; SubGhzWatch turns that into noise floors, bursts and jamming alarms. Receive
// only: the CC1101 is never put in transmit.
#pragma once

#include <Arduino.h>

#include "ss_proto.h"
#include "ss_subghz.h"

namespace ss {
namespace subghz {

// Category mask: the watch runs while CAT_PENTEST (jammers) or CAT_VEHICLE is on.
bool begin(volatile uint16_t* category_mask);
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
// Before deep sleep / power off: puts the CC1101 to sleep and keeps the SPI bus so the
// watch task stops using it. Draw on the screen before calling this.
void shutdown();

}  // namespace subghz
}  // namespace ss
