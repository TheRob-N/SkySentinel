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

}  // namespace subghz
}  // namespace ss
