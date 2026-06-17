#pragma once
#ifndef flock_h
#define flock_h

// ============================================================
// Flock Safety & Raven detection data
// Research credits:
//   WiFi OUI list (31):   OrdoOuroborous/@NitekryDPaul (30) +
//                         Michael/DeFlockJoplin (1 = 82:6b:f2)
//   BLE OUI list (32):    colonelpanichacks/ouispy-detector
//                         (@NitekryDPaul + @konradit)
//   Raven UUIDs (8):      GainSec (raven_configurations.json)
//                         firmware versions 1.1.7, 1.2.0, 1.3.1
//   BLE mfgr ID 0x09C8:   Will Greenberg/@wgreenberg
//   BLE names:            colonelpanichacks/flock-you
//   SSID patterns:        VirtuallyScott/flock-you + nyanBOX
// ============================================================

// ---- WiFi OUI prefixes (31) — Flock ALPR cameras ----
// Match against BSSID in scan results and addr2 in probe requests
static const uint8_t FLOCK_WIFI_OUIS[][3] = {
  {0x70, 0xC9, 0x4E},
  {0x3C, 0x91, 0x80},
  {0xD8, 0xF3, 0xBC},
  {0x80, 0x30, 0x49},
  {0xB8, 0x35, 0x32},
  {0x14, 0x5A, 0xFC},
  {0x74, 0x4C, 0xA1},
  {0x08, 0x3A, 0x88},
  {0x9C, 0x2F, 0x9D},
  {0xC0, 0x35, 0x32},
  {0x94, 0x08, 0x53},
  {0xE4, 0xAA, 0xEA},
  {0xF4, 0x6A, 0xDD},
  {0xF8, 0xA2, 0xD6},
  {0x24, 0xB2, 0xB9},
  {0x00, 0xF4, 0x8D},
  {0xD0, 0x39, 0x57},
  {0xE8, 0xD0, 0xFC},
  {0xE0, 0x4F, 0x43},
  {0xB8, 0x1E, 0xA4},
  {0x70, 0x08, 0x94},
  {0x58, 0x8E, 0x81},
  {0xEC, 0x1B, 0xBD},
  {0x3C, 0x71, 0xBF},
  {0x58, 0x00, 0xE3},
  {0x90, 0x35, 0xEA},
  {0x5C, 0x93, 0xA2},
  {0x64, 0x6E, 0x69},
  {0x48, 0x27, 0xEA},
  {0xA4, 0xCF, 0x12},
  {0x82, 0x6B, 0xF2},  // DeFlockJoplin
};
static const int FLOCK_WIFI_OUI_COUNT = 31;

// ---- BLE OUI prefixes (32) — Flock/Raven BLE devices ----
// From colonelpanichacks/ouispy-detector (@NitekryDPaul + @konradit)
static const uint8_t FLOCK_BLE_OUIS[][3] = {
  {0x18, 0x7F, 0x88},
  {0x24, 0x2B, 0xD6},
  {0x34, 0x3E, 0xA4},
  {0x54, 0xE0, 0x19},
  {0x5C, 0x47, 0x5E},
  {0x64, 0x9A, 0x63},
  {0x90, 0x48, 0x6C},
  {0x9C, 0x76, 0x13},
  {0xAC, 0x9F, 0xC3},
  {0xC4, 0xDB, 0xAD},
  {0xCC, 0x3B, 0xFB},
  {0x00, 0x25, 0xDF},
  {0xB4, 0x1E, 0x52},
  {0x0C, 0x9A, 0xE6},
  {0x8C, 0x58, 0x23},
  {0x04, 0xA8, 0x5A},
  {0x58, 0xB8, 0x58},
  {0xE4, 0x7A, 0x2C},
  {0x60, 0x60, 0x1F},
  {0x48, 0x1C, 0xB9},
  {0x34, 0xD2, 0x62},
  {0x00, 0x12, 0x1C},
  {0x00, 0x26, 0x7E},
  {0x90, 0x03, 0xB7},
  {0x90, 0x3A, 0xE6},
  {0xA0, 0x14, 0x3D},
  {0x38, 0x1D, 0x14},
  {0x7C, 0x2A, 0x9E},
  {0xCC, 0x66, 0x0A},
  {0xF4, 0x03, 0x43},
  {0x5C, 0xE9, 0x1E},
  {0x98, 0x59, 0x49},
};
static const int FLOCK_BLE_OUI_COUNT = 32;

// ---- BLE device name substrings ----
static const char* FLOCK_BLE_NAMES[] = {
  "FS Ext Battery",
  "Penguin",
  "Pigvision",
  "Flock",
};
static const int FLOCK_BLE_NAME_COUNT = 4;

// ---- BLE manufacturer company ID (XUNTONG) ----
#define FLOCK_BLE_MFGR_ID  0x09C8

// ---- Raven GATT service UUIDs (GainSec dataset) ----
// Identifies SoundThinking/ShotSpotter Raven gunshot detectors
// Firmware version estimation:
//   1.1.x (legacy):  00001809 + 00001819
//   1.2.x:           0000180a + 00003100 + 00003200 + 00003300
//   1.3.x (latest):  all of the above + 00003400 + 00003500
static const char* FLOCK_RAVEN_UUIDS[] = {
  "0000180a-0000-1000-8000-00805f9b34fb",  // Device Information (all versions)
  "00003100-0000-1000-8000-00805f9b34fb",  // GPS Location (1.2.x+)
  "00003200-0000-1000-8000-00805f9b34fb",  // Power Management (1.2.x+)
  "00003300-0000-1000-8000-00805f9b34fb",  // Network Status (1.2.x+)
  "00003400-0000-1000-8000-00805f9b34fb",  // Upload Statistics (1.3.x)
  "00003500-0000-1000-8000-00805f9b34fb",  // Error/Failure (1.3.x)
  "00001809-0000-1000-8000-00805f9b34fb",  // Health Thermometer (legacy 1.1.x)
  "00001819-0000-1000-8000-00805f9b34fb",  // Location/Navigation (legacy 1.1.x)
};
static const int FLOCK_RAVEN_UUID_COUNT = 8;

// ---- WiFi SSID patterns ----
// Cameras sometimes beacon with recognizable SSID prefix
static const char* FLOCK_SSID_PATTERNS[] = {
  "FLOCK-S3-",
  "FLOCK-",
};
static const int FLOCK_SSID_PATTERN_COUNT = 2;

// ---- Helper: check first 3 bytes of MAC against OUI list ----
inline bool flockOUIMatch(const uint8_t* mac,
                           const uint8_t list[][3],
                           int count) {
  for (int i = 0; i < count; i++) {
    if (mac[0] == list[i][0] &&
        mac[1] == list[i][1] &&
        mac[2] == list[i][2]) {
      return true;
    }
  }
  return false;
}

#endif
