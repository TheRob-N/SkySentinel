// 802.15.4 listening (Zigbee, Thread / Matter) on an ESP32-C5 node: its radio has an
// 802.15.4 receiver beside Wi-Fi and Bluetooth, all sharing one antenna. The node lends
// it the radio in short blocks (see main.cpp zbShare) and reports who it heard.
// Receive only: it never transmits on 802.15.4.
#pragma once

#include <stdint.h>

#include "ss_proto.h"

namespace zb {

// True when this chip and build have the 802.15.4 driver.
bool supported();
// Turn the 802.15.4 driver on (once). False if it isn't there or wouldn't start.
bool begin();
bool ready();
// Listen on one channel (11..26) until stop().
void listen(uint8_t channel);
void stop();
bool listening();

// Hands what was heard since the last call to `sink` (sighting, plus a name such as
// "Zigbee PAN 1A2B" or nullptr). Call from the main loop.
typedef void (*Sink)(const ss::Sighting& s, const char* name, uint8_t name_len);
void poll(Sink sink, int8_t rssi_floor);

uint32_t frames();     // frames received (any)
uint32_t reported();   // frames that became sightings
uint32_t dropped();    // frames lost because the queue was full
uint8_t channel();     // the channel last listened on (0 = never)
uint32_t heardOn(uint8_t channel);   // frames heard on a channel (11..26)

}  // namespace zb
