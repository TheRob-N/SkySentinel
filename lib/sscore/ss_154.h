// IEEE 802.15.4 frames (Zigbee, Thread / Matter) as a C5 node hears them: who sent the
// frame, on which network, and which protocol it is. Pure C++, host tested.
//
// What can be told without keys:
//  - Zigbee doesn't encrypt at the MAC layer, so its network (NWK) header is readable:
//    protocol version 2 (Zigbee PRO), and often the sender's 64-bit IEEE address.
//    Coordinators and routers answer beacon requests with a Zigbee beacon payload.
//  - Thread encrypts every data frame at the MAC layer; its unencrypted frames (MLE, the
//    joiner's) carry 6LoWPAN headers. Matter over Thread is Thread on the air.
//  - Anything else (proprietary stacks, RF4CE remotes) is filed as plain 802.15.4.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_classify.h"
#include "ss_proto.h"

namespace ss {

struct Frame154 {
  uint8_t ftype = 0;          // MAC frame type: 0 beacon, 1 data, 2 ack, 3 command, ...
  uint8_t version = 0;        // 0 = 2003, 1 = 2006, 2 = 2015
  bool secured = false;       // MAC-layer security (Thread data; never Zigbee)
  uint8_t seq = 0;
  uint16_t pan = 0xFFFF;      // the sender's PAN (network) id, 0xFFFF = not in the frame
  bool has_short = false, has_ext = false;
  uint16_t src_short = 0;
  uint8_t src_ext[8] = {0};   // EUI-64, most significant byte first (as printed)
  uint8_t kind = KIND_NONE;   // KIND_ZIGBEE / KIND_THREAD / KIND_154_OTHER, NONE = nothing to report
  bool zigbee_beacon = false; // a Zigbee coordinator / router answering a beacon request
  bool thread_beacon = false;
};

// `psdu` = the MAC frame (frame control onwards), `len` without the 2-byte FCS.
// False for frames that say nothing about a sender (ACKs, beacon requests, garbage).
bool parse154(const uint8_t* psdu, size_t len, Frame154& out);

Match classify154(const Frame154& f);

// The 6-byte device key a sighting carries: the EUI-64's maker block (first 3 bytes) and
// last 3 bytes, or 02:15 + PAN + short address when only the short one was sent.
void addr154(const Frame154& f, uint8_t out[6]);

// 802.15.4 channels 11..26 (2405 + 5 x (ch - 11) MHz).
constexpr uint8_t k154First = 11, k154Last = 26;
// Sighting for a classified frame (hits = 1).
Sighting make154Sighting(const Frame154& f, const Match& m, uint8_t channel, int8_t rssi);
// Its name for the device list: "Zigbee PAN 1A2B" / "Thread PAN 1A2B" (the network it's on).
// Returns the length (0 if the frame carried no PAN).
uint8_t name154(const Frame154& f, const Match& m, char* out, size_t cap);

}  // namespace ss
