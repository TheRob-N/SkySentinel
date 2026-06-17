/*
 * ble_scan.h — SkySentinel
 * BLE advertisement callbacks: Flock Safety + FAA Remote ID (ASTM F3411)
 * Runs on Core 0 via FreeRTOS task in main .ino
 */
#pragma once
#include <BLEAdvertisedDevice.h>
#include <freertos/queue.h>
#include "flock.h"

extern volatile float gpsLat;
extern volatile float gpsLon;

// ── Match reason bitmask ──────────────────────────────────────────────────────
enum FlockMatchReason : uint8_t {
  FLOCK_MATCH_OUI       = 0x01,
  FLOCK_MATCH_NAME      = 0x02,
  FLOCK_MATCH_MFGR      = 0x04,
  FLOCK_MATCH_RAVEN     = 0x08,
  FLOCK_MATCH_WIFI_OUI  = 0x10,
  FLOCK_MATCH_WIFI_SSID = 0x20,
};

// ── Detection structs ─────────────────────────────────────────────────────────
struct FlockDetection {
  char     mac[18];
  char     name[32];
  uint8_t  matchReason;
  int8_t   rssi;
  float    lat;
  float    lon;
  uint32_t timestamp;
  bool     isBLE;
};

enum DroneMessageType : uint8_t {
  DRONE_BASIC_ID = 0,
  DRONE_LOCATION = 1,
  DRONE_PACK     = 0xF,
  DRONE_UNKNOWN  = 0xFF,
};

struct DroneDetection {
  char             mac[18];
  char             uasID[21];
  DroneMessageType type;
  float            droneLat;
  float            droneLon;
  float            altM;
  int8_t           rssi;
  uint32_t         timestamp;
};

static const uint16_t REMOTE_ID_SVC_UUID = 0xFFFA;

// ── Helper: parse MAC address Arduino String → 6 bytes ───────────────────────
static void parseMACBytes(const String& macStr, uint8_t* out) {
  unsigned int b[6] = {0};
  sscanf(macStr.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
         &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
}

// ── BLE Callback ──────────────────────────────────────────────────────────────
class FlockBLECallback : public BLEAdvertisedDeviceCallbacks {
public:
  FlockBLECallback(QueueHandle_t fQ, QueueHandle_t dQ) : _fQ(fQ), _dQ(dQ) {}

  void onResult(BLEAdvertisedDevice dev) override {
    uint8_t mac[6];
    parseMACBytes(dev.getAddress().toString(), mac);
    uint8_t match = 0;

    // 1. BLE OUI match
    if (flockOUIMatch(mac, FLOCK_BLE_OUIS, FLOCK_BLE_OUI_COUNT))
      match |= FLOCK_MATCH_OUI;

    // 2. Device name substrings
    if (dev.haveName()) {
      String name = dev.getName();
      for (int i = 0; i < FLOCK_BLE_NAME_COUNT; i++) {
        if (name.indexOf(FLOCK_BLE_NAMES[i]) >= 0) {
          match |= FLOCK_MATCH_NAME;
          break;
        }
      }
    }

    // 3. Manufacturer ID 0x09C8
    if (dev.haveManufacturerData()) {
      String m = dev.getManufacturerData();
      if (m.length() >= 2) {
        uint16_t id = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
        if (id == FLOCK_BLE_MFGR_ID) match |= FLOCK_MATCH_MFGR;
      }
    }

    // 4. Raven GATT service UUIDs
    if (dev.haveServiceUUID()) {
      for (int i = 0; i < FLOCK_RAVEN_UUID_COUNT; i++) {
        if (dev.isAdvertisingService(BLEUUID(FLOCK_RAVEN_UUIDS[i]))) {
          match |= FLOCK_MATCH_RAVEN;
          break;
        }
      }
    }

    if (match) {
      FlockDetection fd = {};
      strncpy(fd.mac, dev.getAddress().toString().c_str(), 17);
      if (dev.haveName()) strncpy(fd.name, dev.getName().c_str(), 31);
      fd.matchReason = match;
      fd.rssi        = dev.getRSSI();
      fd.lat         = gpsLat;
      fd.lon         = gpsLon;
      fd.timestamp   = millis();
      fd.isBLE       = true;
      xQueueSend(_fQ, &fd, 0);
    }

    // 5. Remote ID BLE (service UUID 0xFFFA) — check all devices
    checkRemoteID(dev);
  }

private:
  QueueHandle_t _fQ, _dQ;

  void checkRemoteID(BLEAdvertisedDevice& dev) {
    if (!dev.haveServiceUUID()) return;
    if (!dev.isAdvertisingService(BLEUUID(REMOTE_ID_SVC_UUID))) return;
    if (!dev.haveServiceData()) return;

    String sd = dev.getServiceData(0);
    if (sd.length() < 3) return;

    uint8_t msgType = ((uint8_t)sd[0] >> 3) & 0x0F;
    DroneDetection dd = {};
    strncpy(dd.mac, dev.getAddress().toString().c_str(), 17);
    dd.rssi      = dev.getRSSI();
    dd.timestamp = millis();
    dd.type      = DRONE_UNKNOWN;

    if (msgType == 0 && sd.length() >= 23) {
      for (int i = 0; i < 20 && (i+3) < (int)sd.length(); i++)
        dd.uasID[i] = sd[i+3];
      dd.uasID[20] = 0;
      dd.type = DRONE_BASIC_ID;
    } else if (msgType == 1 && sd.length() >= 17) {
      int32_t lat32 = 0, lon32 = 0;
      for (int i = 0; i < 4; i++) lat32 |= ((uint8_t)sd[5+i]) << (8*i);
      for (int i = 0; i < 4; i++) lon32 |= ((uint8_t)sd[9+i]) << (8*i);
      dd.droneLat = lat32 * 1e-7f;
      dd.droneLon = lon32 * 1e-7f;
      dd.type = DRONE_LOCATION;
    } else if (msgType == 0xF) {
      strncpy(dd.uasID, "(pack)", 20);
      dd.type = DRONE_PACK;
    }

    if (dd.type != DRONE_UNKNOWN) xQueueSend(_dQ, &dd, 0);
  }
};
