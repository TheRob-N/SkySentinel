/*
 * wifi_scan.h — SkySentinel
 * Created by:
 * Rob Neyhard
 * Copyright (c) 2026
 * Non-blocking async WiFi scan for Flock OUI + SSID detection
 */
#pragma once
#include <WiFi.h>
#include <freertos/queue.h>
#include "flock.h"
#include "ble_scan.h"

extern volatile float gpsLat;
extern volatile float gpsLon;

static const uint32_t WIFI_SCAN_INTERVAL_MS = 12000;
static uint32_t _wifiLastScan  = 0;
static bool     _wifiPending   = false;
static uint8_t  _wifiScanTick  = 0;   // for scan bar animation

uint8_t wifiGetScanTick() { return _wifiScanTick; }

void wifiScanInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
}

void wifiScanTick(QueueHandle_t flockQ, QueueHandle_t droneQ) {
  uint32_t now = millis();

  if (!_wifiPending && (now - _wifiLastScan) >= WIFI_SCAN_INTERVAL_MS) {
    WiFi.scanNetworks(true);
    _wifiPending  = true;
    _wifiLastScan = now;
    return;
  }
  if (!_wifiPending) return;

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n == WIFI_SCAN_FAILED || n < 0) { _wifiPending = false; return; }

  _wifiScanTick++;   // advance animation tick on each completed scan

  for (int i = 0; i < n; i++) {
    uint8_t mac[6];
    WiFi.BSSID(i, mac);
    uint8_t match = 0;

    if (flockOUIMatch(mac, FLOCK_WIFI_OUIS, FLOCK_WIFI_OUI_COUNT))
      match |= FLOCK_MATCH_WIFI_OUI;

    String ssid = WiFi.SSID(i);
    for (int j = 0; j < FLOCK_SSID_PATTERN_COUNT; j++) {
      if (ssid.startsWith(FLOCK_SSID_PATTERNS[j])) {
        match |= FLOCK_MATCH_WIFI_SSID;
        break;
      }
    }

    if (match) {
      FlockDetection fd = {};
      snprintf(fd.mac, sizeof(fd.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      strncpy(fd.name, ssid.c_str(), 31);
      fd.matchReason = match;
      fd.rssi        = WiFi.RSSI(i);
      fd.lat         = gpsLat;
      fd.lon         = gpsLon;
      fd.timestamp   = millis();
      fd.isBLE       = false;
      xQueueSend(flockQ, &fd, 0);
    }
  }

  WiFi.scanDelete();
  _wifiPending = false;
}
