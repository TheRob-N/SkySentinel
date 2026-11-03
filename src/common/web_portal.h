// The board's own Wi-Fi access point: serves the web UI from flash and streams the
// same JSON lines the USB port carries, so a phone can use SkySentinel with no
// cable, no app and no internet.
//
//   GET  /           the web UI (gzipped, built into the firmware)
//   GET  /events     live stream (Server-Sent Events: one JSON line per message)
//   POST /cmd        one command line, same as typing it on USB serial
//   GET  /info       {"skysentinel":true,...} so the page knows where it is
//   GET  /log.csv    the detection log (logger.h), as a download
//   anything else    302 to the page; with the DNS catch-all this makes phones show
//                    the "sign in to network" popup that opens the UI by itself
#pragma once

#include <Arduino.h>

namespace ss {

namespace portal {

using CommandHandler = void (*)(const String& line);

// Starts the AP (SSID, password or "" for open) on `channel`. Call after WiFi.mode(WIFI_AP_STA).
bool begin(const char* ssid, const char* password, uint8_t channel, CommandHandler onCommand);
// Accepts clients, answers requests, sends pending stream data. Call every loop.
void loop();
// Queues one line for every stream client (called by Out for each line).
void pushLine(const char* line, size_t len);

// Writes the body of GET /log.csv (set by the master; 404 when unset).
using FileWriter = void (*)(Print& out);
void setLogWriter(FileWriter w);

bool active();
uint8_t stationCount();   // devices joined to the AP
uint8_t viewerCount();    // browsers with the live stream open

}  // namespace portal
}  // namespace ss
