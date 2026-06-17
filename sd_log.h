/*
 * sd_log.h — SkySentinel
 * SD logging in WiGLE CSV format (the only format — no native format needed)
 *
 * Files: /SS_WIGLE_WIFI.CSV   /SS_WIGLE_BLE.CSV
 * Format: WiGLE-1.4 spec — compatible with wigle.net upload and WiGLE apps
 */
#pragma once
#include <SD.h>
#include "ble_scan.h"

#define WIGLE_HEADER \
  "WigleWifi-1.4,appRelease=SkySentinel-0.2,model=ESP32-CYD,release=0.2,device=SkySentinel,display=,board=ESP32,brand=Espressif\n" \
  "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n"

static bool _sdLogReady = false;

static const char* sdMatchStr(uint8_t r) {
  static char buf[24];
  buf[0] = 0;
  if (r & FLOCK_MATCH_OUI)       strncat(buf, "OUI+",      sizeof(buf)-strlen(buf)-1);
  if (r & FLOCK_MATCH_NAME)      strncat(buf, "NAME+",     sizeof(buf)-strlen(buf)-1);
  if (r & FLOCK_MATCH_MFGR)      strncat(buf, "MFGR+",     sizeof(buf)-strlen(buf)-1);
  if (r & FLOCK_MATCH_RAVEN)     strncat(buf, "RAVEN+",    sizeof(buf)-strlen(buf)-1);
  if (r & FLOCK_MATCH_WIFI_OUI)  strncat(buf, "W-OUI+",   sizeof(buf)-strlen(buf)-1);
  if (r & FLOCK_MATCH_WIFI_SSID) strncat(buf, "W-SSID",   sizeof(buf)-strlen(buf)-1);
  int len = strlen(buf);
  if (len > 0 && buf[len-1] == '+') buf[len-1] = 0;
  return buf;
}

void sdLogInit() {
  if (!SD.exists("/SS_WIGLE_WIFI.CSV")) {
    File f = SD.open("/SS_WIGLE_WIFI.CSV", FILE_WRITE);
    if (f) { f.print(WIGLE_HEADER); f.close(); }
  }
  if (!SD.exists("/SS_WIGLE_BLE.CSV")) {
    File f = SD.open("/SS_WIGLE_BLE.CSV", FILE_WRITE);
    if (f) { f.print(WIGLE_HEADER); f.close(); }
  }
  _sdLogReady = true;
}

void sdLogFlock(FlockDetection& fd) {
  if (!_sdLogReady) return;

  const char* path = fd.isBLE ? "/SS_WIGLE_BLE.CSV" : "/SS_WIGLE_WIFI.CSV";
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;

  uint32_t sec = fd.timestamp / 1000;
  char ts[24];
  snprintf(ts, sizeof(ts), "2026-01-01 %02lu:%02lu:%02lu",
           (sec/3600)%24, (sec/60)%60, sec%60);

  // Use the match reason as a pseudo-SSID/name tag so Flock hits are
  // identifiable in WiGLE without losing the actual SSID/device name
  char tag[48];
  snprintf(tag, sizeof(tag), "%s [%s]",
           fd.name[0] ? fd.name : "FLOCK", sdMatchStr(fd.matchReason));

  if (fd.isBLE) {
    f.printf("%s,\"%s\",[BLE],\"%s\",0,2402,%d,%.6f,%.6f,0.0,0.0,BLE\n",
             fd.mac, tag, ts, fd.rssi, fd.lat, fd.lon);
  } else {
    f.printf("%s,\"%s\",[WPA2][ESS],\"%s\",6,2437,%d,%.6f,%.6f,0.0,0.0,WIFI\n",
             fd.mac, tag, ts, fd.rssi, fd.lat, fd.lon);
  }
  f.close();
}

void sdLogDrone(DroneDetection& dd) {
  if (!_sdLogReady) return;

  File f = SD.open("/SS_WIGLE_BLE.CSV", FILE_APPEND);
  if (!f) return;

  uint32_t sec = dd.timestamp / 1000;
  char ts[24];
  snprintf(ts, sizeof(ts), "2026-01-01 %02lu:%02lu:%02lu",
           (sec/3600)%24, (sec/60)%60, sec%60);

  char tag[48];
  snprintf(tag, sizeof(tag), "DRONE-RID:%s", dd.uasID[0] ? dd.uasID : "unknown");

  f.printf("%s,\"%s\",[BLE],\"%s\",0,2402,%d,%.6f,%.6f,0.0,0.0,BLE\n",
           dd.mac, tag, ts, dd.rssi, dd.droneLat, dd.droneLon);
  f.close();
}

void sdLogTick() {
  // Write-per-detection is already safe for power loss; nothing to flush.
}
