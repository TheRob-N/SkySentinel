/*
 * settings.h — SkySentinel
 * Created by:
 * Rob Neyhard
 * Copyright (c) 2026
 * NVS-backed settings using ESP32 Preferences library
 * All settings declared here, defined in SkySentinel.ino
 */
#pragma once
#include <Preferences.h>

// ── Brightness / Volume levels ────────────────────────────────────────────────
enum BrightnessLevel : uint8_t { BRIGHT_LOW = 0, BRIGHT_MED = 1, BRIGHT_HIGH = 2, BRIGHT_AUTO = 3 };
enum VolumeLevel     : uint8_t { VOL_OFF = 0, VOL_LOW = 1, VOL_MED = 2, VOL_HIGH = 3 };

// ── Settings variables (defined in SkySentinel.ino) ──────────────────────────
extern bool           settingSDLogging;   // Log detections to SD card (WiGLE CSV format)
extern BrightnessLevel settingBrightness; // LOW / MED / HIGH / AUTO (GPS day-night)
extern VolumeLevel     settingVolume;     // OFF / LOW / MED / HIGH
extern bool           settingFlipped;     // Rotate display 180° (USB on left vs right)
extern bool           settingBLEEnabled;
extern bool           settingWiFiEnabled;

void loadSettings() {
  Preferences p;
  p.begin("skysentinel", true);
  settingSDLogging   = p.getBool("sdlog",   true);
  settingBrightness  = (BrightnessLevel)p.getUChar("bright", BRIGHT_HIGH);
  settingVolume      = (VolumeLevel)p.getUChar("volume", VOL_MED);
  settingFlipped     = p.getBool("flipped", false);
  settingBLEEnabled  = p.getBool("ble",     true);
  settingWiFiEnabled = p.getBool("wifi",    true);
  p.end();
}

void saveSettings() {
  Preferences p;
  p.begin("skysentinel", false);
  p.putBool("sdlog",   settingSDLogging);
  p.putUChar("bright", settingBrightness);
  p.putUChar("volume", settingVolume);
  p.putBool("flipped", settingFlipped);
  p.putBool("ble",     settingBLEEnabled);
  p.putBool("wifi",    settingWiFiEnabled);
  p.end();
}
