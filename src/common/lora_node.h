// 915 MHz LoRa listener for boards with an SX1262 (Heltec Wireless Tracker). Built
// only with -DSS_LORA. Runs in its own task and never transmits.
//
// Time-sliced, since one LoRa radio hears one frequency + spreading factor at a time:
//   Meshtastic LongFast   906.875 MHz SF11  (the US default)        20 s
//   Meshtastic MediumFast 913.125 MHz SF9   (many meshes moved here) 10 s
//   LoRaWAN uplinks       903.9-905.3 MHz SF7-10, activity scan     10 s
#pragma once

#include <Arduino.h>

#include "ss_proto.h"

namespace ss {
namespace lora {

// sink gets each sighting (+ optional name) from the LoRa task; must be thread-safe.
// what = NAME_DEVICE (a name) or NAME_POS (a position, "lat,lon,alt_m" as text).
using Sink = void (*)(const Sighting& s, const char* name, uint8_t name_len, uint8_t what);

// enabled is read each cycle (the board's category mask has CAT_LORA set).
bool begin(Sink sink, volatile uint16_t* category_mask);
bool ready();

struct Stats {
  uint32_t packets = 0, meshtastic = 0, lorawan = 0, other = 0, crc_errors = 0, cad_hits = 0;
  uint32_t mesh_read = 0;   // Meshtastic packets on the public default channel, decrypted
  const char* listening = "off";
};
Stats stats();

}  // namespace lora
}  // namespace ss
