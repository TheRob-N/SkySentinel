// Core-side helpers shared by the Core build and the single-board "solo" build:
// device table setup, sighting ingest, and the JSON-lines output format.
#pragma once

#include <Arduino.h>

#include "ss_dash.h"
#include "ss_devices.h"
#include "ss_follow.h"
#include "ss_proto.h"

namespace ss {

extern bool g_jsonOn;  // live JSON stream (USB serial, and Wi-Fi viewers)
extern bool g_uiOn;    // web UI attached over USB: also stream the live device list ("ui on")
extern volatile uint32_t g_usbWriteMs;  // last time Out wrote to USB (0 = never)
extern bool g_webOn;   // someone is watching over the board's Wi-Fi access point

// Everything the board reports goes through Out: it writes to USB serial and, line
// by line, to anyone watching over Wi-Fi.
class OutTee : public Print {
 public:
  using LineSink = void (*)(const char* line, size_t len);
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t len) override;
  void setSink(LineSink sink) { sink_ = sink; }
 private:
  LineSink sink_ = nullptr;
  String line_;
};
extern OutTee Out;
// Master with a dashboard node (CYD): alerts also go here. Null elsewhere.
extern DashAlerts* g_dashAlerts;

// Allocates the device table (PSRAM when available, else a small internal one).
DeviceTable* createDeviceTable();

// Feeds sightings from node `slot` into the table; prints {"t":"dev"} for devices
// that are new and interesting, or just became interesting.
void ingestSightings(DeviceTable& table, uint8_t slot, const Sighting* s, size_t n, uint32_t now_ms);

void jsonStr(const char* s);
void jsonMac(const uint8_t* mac);
void emitDevice(const Device& d, const char* why);
void emitEvent(const Event& ev, int slot);
// Emits a ble_spam event if the table detected a new burst.
void pollSpamEvents(DeviceTable& table);
// Prints the counts part of a status object: ,"now":{...},"total":{...},"devices_now":... (no braces).
void emitCounts(const DeviceTable& table, uint32_t now_ms);
// {"t":"devs",...}: compact list of devices heard in the last minute, for the web UI.
// Most interesting first, capped so a busy street can't flood the USB link.
void emitDeviceList(const DeviceTable& table, uint32_t now_ms, size_t max_devices = 150);
// {"t":"aps","id":..,"list":[...]}: an access point and every network grouped under it
// (same router, other SSIDs), for the web UI's "+N networks" popup.
void emitAccessPointGroup(const DeviceTable& table, uint32_t id, uint32_t now_ms);
// Files a MSG_NAME (or a local one): the device's name and AP details on the device,
// a probed network or WPS make / model in the notes. Probed networks are kept only
// while "probes on" (the web page's opt-in setting), in memory, never logged.
void applyName(DeviceTable& table, const Name& n, uint32_t now_ms);
extern bool g_probesOn;
extern uint32_t g_listWindowMs;   // LIVE DEVICES: heard within this long ("list window <s>")
// {"t":"info","id":..,"wps":"..","probes":[["ssid",age_s],...]}: the extras for the
// web page's device detail window ("info <id>").
void emitDeviceInfo(const DeviceTable& table, uint32_t id, uint32_t now_ms);
// {"t":"event","event":"following"|"lingering",...} for a FollowMonitor alert.
void emitFollow(const DeviceTable& table, const FollowAlert& a);
// Handles commands shared by every Core-type build ("ui on|off", "json on|off").
// Returns true if the line was one of them.
bool handleCommonCommand(const String& line);
// Human-readable table of devices seen in the last minute.
void dumpDevices(const DeviceTable& table, uint32_t now_ms);

}  // namespace ss
