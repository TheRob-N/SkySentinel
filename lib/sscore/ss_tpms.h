// Tire pressure sensors (TPMS) heard by the T-Embed CC1101 (pure C++, host tested).
//
// Every tire on a car built since 2008 (US) has a sensor that sends its own fixed 32-bit
// ID, its pressure and often its temperature, about once a minute while the car moves.
// The ID never changes, so the same four IDs showing up at several of your stops is a car
// that went where you went. That's what this is for: the decoded ID becomes a device in
// the list and goes through the usual following / lingering checks.
//
// The radio hands over what it demodulated as runs: a level and how long it held, in
// microseconds. Two kinds of sensor, two radio setups:
//   TPMS_FSK  frequency shift keyed, ~52 us half-bits (19.2 kBaud): Toyota (PMV-C210),
//             Ford, Hyundai / Kia (Continental VDO), Jeep / Chrysler (Continental),
//             Hyundai Elantra / Honda Civic (TRW GQ4-44T), Kia (Rio III), Honda (TRW PPA-GF33)
//   TPMS_OOK  on-off keyed, ~122 us half-bits: GM and other Schrader sensors (EG53MA4,
//             MRXGG4)
// The wire formats (preambles, layouts, checks) follow what the rtl_433 project's
// contributors worked out from real captures; this is an independent implementation.
// Receive only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_proto.h"

namespace ss {

enum TpmsMode : uint8_t { TPMS_FSK = 0, TPMS_OOK = 1 };

enum TpmsMaker : uint8_t {
  TPMS_NONE = 0,
  TPMS_TOYOTA,        // Pacific PMV-C210 (Toyota / Lexus)
  TPMS_FORD,          // Continental VDO for Ford
  TPMS_VDO,           // Continental VDO TG1C (Hyundai, Kia, Mazda, Mitsubishi, FCA...)
  TPMS_CONTINENTAL,   // Jeep / Chrysler (same wire format as Citroen's)
  TPMS_TRW,           // TRW GQ4-44T (Hyundai Elantra, Honda Civic)
  TPMS_KIA,           // Kia Rio III and some Hyundai
  TPMS_HONDA,         // TRW PPA-GF33 (Honda)
  TPMS_SCHRADER_GM,   // Schrader EG53MA4 (GM 2006 on, Opel, Saab)
  TPMS_SCHRADER,      // Schrader MRXGG4
  TPMS_MAKERS
};
const char* tpmsMakerName(uint8_t maker);   // "Toyota", "GM (Schrader)", ...

struct TpmsReading {
  uint8_t maker = TPMS_NONE;
  uint32_t id = 0;
  int16_t psi_x10 = -1;    // pressure in tenths of a PSI, -1 = not sent / not plausible
  int16_t temp_c = -128;   // -128 = not sent
};

// A run: the level in bit 15 (1 = high / the upper frequency), microseconds in bits 0..14.
constexpr uint16_t kTpmsHigh = 0x8000;
constexpr uint16_t kTpmsUsMask = 0x7FFF;

// Decodes one capture (a burst of runs, in order). Writes up to `max` distinct readings
// to `out` (a burst usually repeats the same frame several times) and returns how many.
uint8_t tpmsDecode(const uint16_t* runs, size_t n, TpmsMode mode, TpmsReading* out, uint8_t max);

// The device-list record: addr = 02 + maker + the 32-bit ID (fixed: a tire never changes
// it), category Vehicles, channel = the sub-GHz band it was heard on.
Sighting tpmsSighting(const TpmsReading& r, int8_t rssi, uint8_t band);
// Its name, e.g. "Toyota tire 1A2B3C4D 34 psi". Returns the length.
uint8_t tpmsName(const TpmsReading& r, char* out, size_t cap);
// The sensor ID back from such an address (and its maker), false if it isn't one.
bool tpmsFromAddr(const uint8_t addr[6], uint8_t* maker, uint32_t* id);

// For tests: encode a frame of runs the way a sensor would send it.
size_t tpmsEncodeForTest(uint8_t maker, uint32_t id, uint8_t pressure_raw, uint8_t temp_raw, uint16_t* runs, size_t cap);

}  // namespace ss
