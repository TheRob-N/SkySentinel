// Optional GPS on a CYD: any NMEA module (ATGM336H, NEO-6M/M8N, BN-220...) with its TX
// wire on GPIO 35. That's the 3-pin expansion plug on the ESP32-32E (GND, 3.3V, IO35)
// and P3 on the ESP32-2432S028. Receive only: the module's RX stays unconnected.
//
// Finds the baud rate by itself (9600, then 115200, 38400). Nothing is reported until
// the first valid sentence, so a CYD without a GPS doesn't show "GPS no data"; once one
// has been seen it's remembered, and a GPS that stops talking does show up.
// The position and GPS clock go to the master in the heartbeat, like the Heltec's.
// Built only with -DSS_CYD.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "ss_proto.h"

namespace ss {
namespace cydgps {

void begin(Preferences* prefs);
void loop(uint32_t now_ms);
// Adds gps state, satellites, position and clock to a heartbeat (no-op without a GPS).
void fill(Heartbeat& hb, uint32_t now_ms);
uint8_t state(uint32_t now_ms);   // GpsState; GPS_NONE until one has been seen
uint8_t sats();
// Fix details and satellites (MSG_GPS). False without a GPS.
bool info(GpsInfo& out, uint32_t now_ms);
bool handleCommand(const String& line);   // "gps": status

}  // namespace cydgps
}  // namespace ss
