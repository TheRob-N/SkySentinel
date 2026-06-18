# CYD Flock & Drone Detection Firmware — Architecture & Build Plan

**Project:** FlockHound-CY
 Created by:
  Rob Neyhard
 Copyright (c) 2026D  
**Hardware:** ESP32-2432S028 (CYD), ILI9341 240×320 resistive touch, built-in SD, WiFi + BLE  
**IDE:** Arduino IDE · Board target: ESP32 Dev Module (240 MHz, 4MB flash, partition: Default 4MB with spiffs)

---

## Hardware Pin Reference (ESP32-2432S028)

| Function | GPIO | Notes |
|---|---|---|
| TFT MOSI | 13 | HSPI |
| TFT MISO | 12 | HSPI |
| TFT SCLK | 14 | HSPI |
| TFT CS | 15 | |
| TFT DC | 2 | ⚠️ Shared with LDR — keep LDR analog reads away from TFT writes |
| TFT RST | – | Tied to EN/RST on board |
| TFT BL | 21 | Backlight PWM |
| Touch CS | 33 | XPT2046, VSPI |
| Touch IRQ | 36 | Input-only pin |
| SD MOSI | 23 | VSPI |
| SD MISO | 19 | VSPI |
| SD SCLK | 18 | VSPI |
| SD CS | 5 | |
| RGB LED R | 4 | Active LOW |
| RGB LED G | 16 | Active LOW |
| RGB LED B | 17 | Active LOW |
| LDR | 34 | Analog input-only |

**Two SPI buses are in use simultaneously:**
- HSPI (TFT): MOSI=13, MISO=12, SCLK=14
- VSPI (Touch + SD): MOSI=23, MISO=19, SCLK=18, Touch_CS=33, SD_CS=5

---

## File Structure

```
FlockHound-CYD/
├── FlockHound-CYD.ino      # Main sketch, setup/loop, task spawning
├── flock.h                  # Your existing OUI/UUID/SSID lists + flockOUIMatch()
├── display.h / display.cpp  # TFT_eSPI wrapper, screens, UI drawing
├── ble_scan.h / ble_scan.cpp  # BLE scanner (FreeRTOS task, Core 0)
├── wifi_scan.h / wifi_scan.cpp # WiFi passive scan (Core 1 with main)
├── drone_id.h / drone_id.cpp   # Remote ID decode (BLE + WiFi NAN)
├── sd_log.h / sd_log.cpp       # SD card CSV logging
├── gps.h / gps.cpp             # Optional: NMEA GPS via UART (pin 27/GPIO)
└── User_Setup.h                # TFT_eSPI config — goes into library folder
```

---

## TFT_eSPI User_Setup.h (copy to `libraries/TFT_eSPI/User_Setup.h`)

```cpp
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFF_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1   // Connected to ESP32 RST

#define TOUCH_CS 33
#define SPI_FREQUENCY        55000000
#define SPI_TOUCH_FREQUENCY   2500000
```

---

## Library Dependencies (Arduino Library Manager)

| Library | Version | Purpose |
|---|---|---|
| TFT_eSPI | ≥2.5.43 | Display driver |
| XPT2046_Touchscreen | ≥1.4 | Resistive touch |
| SD (built-in) | – | SD card |
| ESP32 BLE Arduino | built-in with esp32 core | BLE scan |
| TinyGPS++ | ≥1.0.3 | Optional GPS parsing |

No external Remote ID library needed — we decode ASTM F3411 manually from raw ad payloads.

---

## Architecture: Dual-Core Task Layout

```
Core 0 (Protocol CPU)          Core 1 (App CPU — Arduino default)
─────────────────────          ─────────────────────────────────
bleTask()                      loop()
  └─ BLEScan continuous          └─ wifiScanTick()   (non-blocking)
     └─ FlockBLECallback            └─ ESP WiFi promiscuous sniff
        - OUI match (flock.h)           - Flock WiFi OUI + SSID match
        - Name substring match          - Remote ID WiFi beacon parse
        - Mfgr ID 0x09C8           └─ displayTick()   (non-blocking)
        - Raven GATT UUID match         - Touch poll
        - Remote ID BLE parse           - Screen redraw at ~10fps
        → push to detectionQueue    └─ sdLogTick()     (non-blocking)
                                        - Flush queue to SD
                                    └─ gpsTick()       (non-blocking)
                                        - Parse NMEA serial
```

Use a `QueueHandle_t detectionQueue` (FreeRTOS) to hand detections from Core 0 to Core 1's display/log loop.

---

## Detection Logic

### Flock BLE Detection (`ble_scan.cpp`)

```cpp
class FlockBLECallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    // 1. OUI match (32 prefixes from flock.h)
    uint8_t mac[6];
    parseMACStr(dev.getAddress().toString(), mac);
    bool ouiHit = flockOUIMatch(mac);  // your existing helper

    // 2. Name substrings
    String name = String(dev.getName().c_str());
    bool nameHit = name.indexOf("Flock") >= 0 ||
                   name.indexOf("Penguin") >= 0 ||
                   name.indexOf("Pigvision") >= 0 ||
                   name.indexOf("FS Ext Battery") >= 0;

    // 3. Manufacturer ID 0x09C8
    bool mfgrHit = false;
    if (dev.haveManufacturerData()) {
      auto mfgr = dev.getManufacturerData();
      if (mfgr.length() >= 2) {
        uint16_t id = (uint8_t)mfgr[1] << 8 | (uint8_t)mfgr[0];
        mfgrHit = (id == 0x09C8);
      }
    }

    // 4. Raven GATT service UUIDs (8 from flock.h)
    bool ravenHit = false;
    for (auto& uuid : RAVEN_SERVICE_UUIDS) {
      if (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(uuid)))
        ravenHit = true;
    }

    if (ouiHit || nameHit || mfgrHit || ravenHit) {
      Detection d = {TYPE_FLOCK_BLE, dev.getAddress().toString(), name,
                     ouiHit, nameHit, mfgrHit, ravenHit,
                     dev.getRSSI(), currentLat, currentLon, millis()};
      xQueueSend(detectionQueue, &d, 0);
    }

    // 5. Remote ID BLE check (runs on all devices)
    checkRemoteID_BLE(dev);
  }
};
```

### Remote ID BLE Detection (`drone_id.cpp`)

ASTM F3411 over BLE uses **Service UUID `0xFFFA`** in the advertisement.
The payload is a sequence of ODID message frames starting with a 1-byte header:
- Bits [7:3] = message type (0=Basic ID, 1=Location, 4=System, 5=Operator ID…)
- Bits [2:0] = protocol version

```cpp
void checkRemoteID_BLE(BLEAdvertisedDevice& dev) {
  // Service UUID 0xFFFA signals Open Drone ID
  if (!dev.isAdvertisingService(BLEUUID((uint16_t)0xFFFA))) return;

  // Service data payload
  std::string svcData = dev.getServiceData();
  if (svcData.length() < 3) return;

  uint8_t header = svcData[0];
  uint8_t msgType = (header >> 3) & 0x0F;

  DroneDetection d;
  d.mac = dev.getAddress().toString();
  d.rssi = dev.getRSSI();
  d.timestamp = millis();

  if (msgType == 0) {  // Basic ID
    // Bytes 1-2: ID type + UA type
    // Bytes 3-22: UAS ID (serial or session)
    memcpy(d.uasID, &svcData[3], min((int)svcData.length()-3, 20));
    d.uasID[20] = 0;
    d.type = DRONE_BASIC_ID;
  } else if (msgType == 1) {  // Location/Vector
    // Lat/Lon at bytes 5-12 as int32 * 1e-7
    int32_t lat, lon;
    memcpy(&lat, &svcData[5], 4);
    memcpy(&lon, &svcData[9], 4);
    d.droneLat = lat * 1e-7;
    d.droneLon = lon * 1e-7;
    d.type = DRONE_LOCATION;
  }

  xQueueSend(droneQueue, &d, 0);
}
```

### Remote ID WiFi Detection (`wifi_scan.cpp`)

WiFi Remote ID uses **NAN (Neighbor Awareness Networking) Action Frames** and also shows up in standard **beacon probe responses** with a vendor-specific IE (OUI `FA-0B-BC`, type `0x0D`).

```cpp
// Enable promiscuous mode to catch NAN action frames
esp_wifi_set_promiscuous(true);
esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_cb);

void wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  auto* pkt = (wifi_promiscuous_pkt_t*)buf;
  auto* payload = pkt->payload;
  uint16_t len = pkt->rx_ctrl.sig_len;

  // Look for 802.11 management frames (type=0, subtype=0xD action)
  uint8_t frameType = (payload[0] & 0x0C) >> 2;
  uint8_t subtype   = (payload[0] & 0xF0) >> 4;

  if (frameType == 0 && subtype == 13) {  // Action frame
    // Check for NAN category (0x04) + OUI FA:0B:BC
    if (len > 30 && payload[24] == 0x04 &&
        payload[26] == 0xFA && payload[27] == 0x0B && payload[28] == 0xBC) {
      parseRemoteID_WiFi(payload, len);
    }
  }

  // Also scan beacon frames for Flock WiFi
  if (frameType == 0 && subtype == 8) {  // Beacon
    checkFlockBeacon(payload, len);
  }
}
```

**Critical constraint:** WiFi promiscuous mode and BLE scanning **cannot run simultaneously** on the ESP32 — they share the RF coprocessor. You have two options:

**Option A (Recommended): Time-division multiplexing**
- BLE scan for 4s → stop → WiFi scan for 2s → stop → repeat
- Implement with a state machine in `loop()`

**Option B: BLE only**
- Skip WiFi promiscuous entirely; catch Flock WiFi via normal `WiFi.scanNetworks()` (non-promiscuous, checks SSID + BSSID OUI in results)
- Much simpler, loses drone WiFi NAN frames but keeps Flock WiFi OUI/SSID detection

**Recommendation for v1.0:** Use Option B for stability. `WiFi.scanNetworks(true)` in async mode is reliable, lightweight, and cooperative with BLE if you stop/restart each in sequence. Add NAN frame sniffing in v1.1.

---

## UI Design

**Display:** 240×320 portrait, TFT_eSPI, ~10fps redraw

### Screen Layout

```
┌──────────────────────────────┐  y=0
│ ● FlockHound  [BLE][WiFi]    │  Header bar 28px, dark bg
├──────────────────────────────┤  y=28
│  FLOCK  │  DRONE  │  LOG     │  Tab bar 32px
├──────────────────────────────┤  y=60
│                              │
│   [SCREEN CONTENT 260px]     │
│                              │
│                              │
│                              │
└──────────────────────────────┘  y=320
```

### Screen 1 — Live Counters (default)
```
┌──────────────────────────────┐
│ FlockHound  [BLE●][WiFi○]    │
├─────────┬────────┬───────────┤
│ FLOCK   │ DRONE  │ LOG       │
├─────────┴────────┴───────────┤
│                              │
│    FLOCK  ██ 3               │
│    DRONE  ██ 1               │
│                              │
│  Last: Flock BLE             │
│  OUI: AC:23:3F  -72dBm       │
│  10:42:31                    │
│                              │
│  GPS: 41.7694 -77.0734       │
│  Sats: 8                     │
└──────────────────────────────┘
```

### Screen 2 — Detection Log
Scrollable list, newest first. Each row: `[type] [MAC] [RSSI] [time]`

### Alert Overlay
On any new detection: full-screen red overlay for 3 seconds showing detection type, MAC, match reason. RGB LED pulses red (Flock) or blue (Drone). Can be dismissed by tap.

---

## SD Logging Format

`FLOCK_LOG.CSV` and `DRONE_LOG.CSV` on SD root:

```
timestamp_ms,type,mac,name,match_reason,rssi,lat,lon
1234567,FLOCK_BLE,AC:23:3F:xx:xx:xx,Flock-S3,OUI+NAME,-72,41.7694,-77.0734
1234890,DRONE_BLE,AA:BB:CC:DD:EE:FF,,REMOTE_ID,-65,41.7701,-77.0729
```

Flush to SD every 10 detections or every 60 seconds, not on every event (SD writes are slow and block).

---

## GPS Integration (Optional)

Attach any $3 UART GPS module to the CYD's P3 connector:
- TX of GPS → GPIO 27 (RX input on ESP32, available on P3)
- Use `HardwareSerial Serial2(2)` at 9600 baud
- TinyGPS++ parses NMEA; feed lat/lon into a `volatile float gpsLat, gpsLon`

---

## Build Order (Phases)

### Phase 1 — Hardware bringup
1. Install TFT_eSPI, copy `User_Setup.h`
2. Run TFT_eSPI `TFT_graphicstest` example — confirm display works
3. Run `TouchCalibrate` example — note the 5 calibration constants
4. Test SD: `SD_Test` example with FAT32 card

### Phase 2 — Skeleton firmware
5. `FlockHound-CYD.ino` with display init, tab UI, RGB LED
6. Port `flock.h` directly from your wardriver project (zero changes needed)
7. Add `sd_log.cpp` — open files on boot, confirm write

### Phase 3 — BLE detection
8. `ble_scan.cpp`: BLE scan task on Core 0, FlockBLECallback
9. Wire detectionQueue → display log + SD log
10. Test with a known Flock-vicinity location or BLE ad spoofer

### Phase 4 — WiFi detection
11. `wifi_scan.cpp`: async `WiFi.scanNetworks()` in time-division with BLE
12. OUI match on BSSID, SSID pattern match on name
13. Add Flock WiFi detections to same queue/log

### Phase 5 — Remote ID
14. `drone_id.cpp`: BLE service UUID 0xFFFA detection + Basic ID / Location decode
15. WiFi NAN sniff (if upgrading to promiscuous — optional)

### Phase 6 — Polish
16. GPS integration
17. Alert overlay + LED
18. Touch scrolling on log screen
19. Settings screen (BLE on/off, WiFi on/off, brightness)

---

## Key Gotchas

**GPIO 2 / LDR conflict:** GPIO2 is TFT_DC. The LDR is on GPIO34 (input-only). No actual conflict — the concern in community threads was a different board revision. You're fine.

**WiFi + BLE coexistence:** The ESP32 has a single RF frontend. `esp_coex_wifi_bt_coexist_open()` enables software coexistence but throughput drops. For passive scanning, time-division is cleaner.

**SD + Touch SPI sharing:** Both are on VSPI. The SD library and XPT2046_Touchscreen library manage their own CS pins — no special work needed as long as you initialize both with the same `SPIClass` instance.

**Promiscuous mode breaks WiFi scanning:** Once you call `esp_wifi_set_promiscuous(true)` you cannot also use `WiFi.scanNetworks()`. Pick one mode per scan window.

**BLE scan + connect:** We're scan-only (passive observation), never connecting. Use `BLEScan::setActiveScan(false)` to avoid sending scan requests that could alert the device.

**Stack size:** BLE task needs at least 8192 bytes of stack. FreeRTOS default is 4096 — will crash silently.

```cpp
xTaskCreatePinnedToCore(bleTask, "BLE", 8192, NULL, 1, &bleTaskHandle, 0);
```

---

## Remote ID BLE — Message Type Reference (ASTM F3411-22a)

| Type | Value | Content |
|---|---|---|
| Basic ID | 0x0 | UAS serial or session ID, UA type |
| Location/Vector | 0x1 | Lat, Lon, Alt, Speed, Heading |
| Authentication | 0x2 | Auth data pages |
| Self ID | 0x3 | Operator description text |
| System | 0x4 | Operator lat/lon, area count |
| Operator ID | 0x5 | Operator/pilot registration ID |
| Message Pack | 0xF | Multiple messages concatenated |

For v1.0, decode Type 0 (serial) and Type 1 (location) — that gives you drone ID + position.

---

## Next Steps

Start with **Phase 1–2** and share the output of `TouchCalibrate` — those 5 constants go into `display.cpp`. Once the skeleton is up and the display is running, we build `ble_scan.cpp` together, drop in your `flock.h`, and you should have your first Flock BLE detection in one evening.
