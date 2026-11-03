// SkySentinel firmware: one image for every board. The board's job is a setting
// saved in flash, chosen on first run (web UI today, the V8 screen later) and
// changeable any time without reflashing:
//
//   standalone  one board does everything: hops channels, keeps the device table,
//               streams JSON over USB. What a lone C5 dev board should run.
//   master      the Core: pairs nodes over ESP-NOW, hands out work, merges every
//               node's sightings into one table. Stays on the home channel; its own
//               radio also listens (BLE + Wi-Fi on the home channel) as "slot 0".
//   node        a sensor: listens, batches sightings, reports to the master.
//
// Until a role is chosen the board runs as standalone and reports "setup":false so
// the web UI can show the setup screen.
//
// Serial commands (115200, one per line):
//   role standalone|master|node     save the role and reboot into it
//   setup <role> [key]              first-run setup: role + cluster key, one reboot
//   setup done | setup reset        mark first-run setup finished / show it again
//   key <text> | key -              cluster key, must match on every board (reboots)
//   cats all | cats surv | cats <hex>   enabled categories (standalone, master)
//   mode split | mode mirror        how a master spreads work (master)
//   local on | local off            master's own radio listens too (master)
//   core <mac> | core -             pin a node to one master (node)
//   aps <id>                        networks grouped under access point <id> (web UI)
//   info <id>                       router make / model, networks a phone looked for (web UI)
//   probes on|off                   collect the networks phones look for (memory only; off forgets)
//   scan active|passive             BLE: also ask devices for their scan response (names)
//   deploy stationary|mobile|portable   following alerts when moving, lingering when not
//   screens on|off                  master: node screens (Heltec) on or off
//   screen on|off                   this board's own screen (Heltec as standalone / master)
//   inspect <mac>|off               stream one device's raw adverts (web UI detail view)
//   query <mac> r|p                 connect to ONE device, read its name/maker/model/battery
//   wifi on|off <password|-> <name> Wi-Fi access point serving the web UI (reboots)
//   devices | status | reboot | factory
//   json on|off | ui on|off         JSON stream / web UI device list
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#include <esp32-hal-psram.h>   // after this, CONFIG_SPIRAM is only set with BOARD_HAS_PSRAM
#if CONFIG_IDF_TARGET_ESP32C5 && (CONFIG_SPIRAM || CONFIG_SPIRAM_SUPPORT)
#include <esp_psram.h>
#include <esp_private/esp_psram_mspi.h>
// From esp_private/esp_psram_extram.h (not included: Arduino.h declares the test differently)
extern "C" esp_err_t esp_psram_extram_add_to_heap_allocator(void);
extern "C" bool ssPsramTest(void) __asm__("esp_psram_extram_test");
#define SS_PSRAM_RESCUE 1
#endif
// Note: the Arduino core hides CONFIG_SPIRAM unless BOARD_HAS_PSRAM is defined (and then
// never starts PSRAM at all), so every C5 build defines it (platformio.ini).
#if CONFIG_IDF_TARGET_ESP32C5 && !defined(SS_PSRAM_RESCUE)
#error "C5 build without PSRAM support: add -DBOARD_HAS_PSRAM"
#endif

#include "core_common.h"
#include "logger.h"
#include "espnow_link.h"
#include "ss_batch.h"
#include "ss_channels.h"
#include "ss_classify.h"
#include "ss_gps.h"
#include "ss_devices.h"
#include "ss_nodes.h"
#include "ss_proto.h"
#include "pairing.h"
#include "web_portal.h"
#ifdef SS_LORA
#include "lora_node.h"  // 915 MHz LoRa listener (Heltec Wireless Tracker)
#endif
#ifdef SS_HAS_DISPLAY
#include "node_display.h"  // the Heltec's little status screen, the T-Embed's screen and jog wheel
#endif
#ifdef SS_SUBGHZ
#include "subghz_node.h"  // T-Embed CC1101: sub-GHz band watch
#endif

using namespace ss;

#ifndef SS_FW_VERSION
#define SS_FW_VERSION 0x0001
#endif
#ifndef SS_HW_TYPE
#define SS_HW_TYPE HW_C5_DEVKIT
#endif

// Board role (not to be confused with a node's scan role, ROLE_BLE/WIFI/BOTH).
enum RunRole : uint8_t { RUN_STANDALONE = 0, RUN_MASTER = 1, RUN_NODE = 2 };
static const char* runRoleName(uint8_t r) {
  return r == RUN_MASTER ? "master" : r == RUN_NODE ? "node" : "standalone";
}

static constexpr uint32_t kHelloEveryMs = 1000;
static constexpr uint32_t kHeartbeatEveryMs = 5000;
static constexpr uint32_t kCoreLostMs = 20000;    // node: no CONFIG / WELCOME refresh, re-pair
static constexpr uint32_t kNodeTimeoutMs = 15000; // master: node silent this long = offline
static constexpr uint32_t kListenWindowMs = 60;   // node: listen this long for the master's reply
static constexpr uint32_t kBeatListenMs = 400;  // the master answers each heartbeat; a busy master can take a few hundred ms
static constexpr uint32_t kHelloListenMs = 150;   // ...and after a hello, for the WELCOME    // ...longer after a heartbeat (the master may be busy
                                                  //    streaming to a browser before it answers)
static constexpr uint32_t kUnconfiguredBeatMs = 1000;  // node: check in this often until it has a CONFIG
static constexpr uint32_t kStatusEveryMs = 1000;
static constexpr uint8_t kMasterLocalDuty = 30;   // master's own BLE scan share (leaves air time for ESP-NOW)
static constexpr uint8_t kLocalSlot = 0;
static constexpr uint8_t kApMaxBleDuty = 50;      // standalone + AP: keep half the air time for Wi-Fi          // master's own radio in the device table
static constexpr size_t kQueueDepth = 128;

// ---------------------------------------------------------------------------
// State

struct RxItem {        // classified hit, produced in radio callbacks
  Sighting s;
  uint8_t name_len;
  char name[kMaxName + 1];
  uint8_t what;        // NameWhat: NAME_PROBE = a network a phone asked for
  bool has_ap;
  ApMeta ap;           // access point details (beacons, probe responses)
  bool note_only;      // extra details about a device already reported (Remote ID): no hit
};

#ifdef SS_WIFI_TRIM
// ESP32-C5 boards without PSRAM run short of memory as the master (16 KB free with
// everything going). Wi-Fi's receive buffers are the biggest thing we can trim: by
// default a burst can take 32 x 1.6 KB. Cap it, and keep fewer fixed buffers. A burst
// past the cap drops frames instead of starving the rest of the board.
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  wifi_init_config_t c = *config;
  if (c.static_rx_buf_num > 6) c.static_rx_buf_num = 6;
  if (c.dynamic_rx_buf_num > 16) c.dynamic_rx_buf_num = 16;
  if (c.dynamic_tx_buf_num > 16) c.dynamic_tx_buf_num = 16;
  c.ampdu_rx_enable = 0;   // only matters for phones on our access point: the page is small
  c.ampdu_tx_enable = 0;
  return __real_esp_wifi_init(&c);
}
#endif

static EspNowLink espLink;  // (not "link": POSIX link() lives in unistd.h)
static Preferences prefs;
static QueueHandle_t hitQueue;
static SightingBatch<96> batch;
static NameTracker<64> names;
static NameTracker<64> probeNames;
static NameTracker<16> liveNames(2000);
static FloodMonitor flood;
static portMUX_TYPE floodMux = portMUX_INITIALIZER_UNLOCKED;

static RunRole runRole = RUN_STANDALONE;
static bool setupDone = false;
static uint8_t myMac[6];
static uint16_t myId;
static String clusterKey;
static pairing::NodePair nodePair;     // node with no cluster key: asking to join
static pairing::MasterPair masterPair; // master: the ADD NODE window
static bool unpaired = false;          // node: no cluster key yet

// Standalone + master: the device table and the user's category choice.
static DeviceTable* table = nullptr;
static uint16_t userCats = kAllCategoriesMask;

// Master
static NodeRegistry nodes;
static AssignMode assignMode = ASSIGN_SPLIT;
static bool localScan = true;

// Where the board lives: stationary, mobile (vehicle) or portable (handheld).
// Mobile/portable watch for devices that stay with us as we move.
static FollowMonitor follow;

// BLE scan style. Passive (default) only listens. Active also sends a scan request,
// so devices answer with their scan response, where many keep their name.
static bool activeScan = false;

// "inspect <mac>": stream one device's raw adverts to the web UI's detail view.
struct InspectBuf {
  uint8_t mac[6];
  uint8_t data[255];
  uint8_t len;
  int8_t rssi;
  bool random;
  bool connectable;
};
static InspectBuf inspectWant;       // which MAC (only mac[] used)
static volatile bool inspectOn = false;
static InspectBuf inspectHit;        // latest matching advert, filled in the BLE callback
static volatile bool inspectReady = false;
static portMUX_TYPE inspectMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t inspectLastMs = 0;

// "query <mac> r|p": briefly connect to ONE device and read what it publishes about
// itself (name, maker, model, firmware, battery). Runs in its own task; scanning
// pauses for the few seconds it takes. The device can see the connection.
static volatile bool queryBusy = false;
static volatile bool bleHeld = false;  // node: BLE paused so the radio can hear the master

// Status for the node's own screen and the master's NODES panel
static int8_t masterRssi = -127;      // node: how well we hear the master
static char nodeLabel[kMaxLabel + 1] = {0};  // node: the name it was given on the master
static uint32_t identifyUntil = 0;    // node: flash screen / LED until then
static uint32_t sightingsSent = 0;    // node: sightings reported to the master
static uint8_t battPct = 255;         // 0-100 %, 254 = on USB from a computer, 255 = no battery / can't tell
#ifdef SS_GPS
// Heltec Wireless Tracker V1.1: UC6580 GNSS on UART, powered by Vext (Meshtastic's variant)
#ifndef SS_GPS_RX
#define SS_GPS_RX 33      // board RX <- GPS TX
#define SS_GPS_TX 34
#define SS_GPS_RESET 35   // active low
#define SS_GPS_VEXT 3
#endif
static Nmea gps;
static void gpsBegin() {
  pinMode(SS_GPS_VEXT, OUTPUT);
  digitalWrite(SS_GPS_VEXT, HIGH);
  pinMode(SS_GPS_RESET, OUTPUT);
  digitalWrite(SS_GPS_RESET, HIGH);
  Serial1.setRxBufferSize(2048);  // the node listens for the master up to 200 ms at a time
  Serial1.begin(115200, SERIAL_8N1, SS_GPS_RX, SS_GPS_TX);
}
static void gpsPoll(uint32_t now) {
  while (Serial1.available()) gps.feed((char)Serial1.read(), now);
}
static uint8_t gpsState(uint32_t now) { return gps.state(now); }
#else
static uint8_t gpsState(uint32_t) { return GPS_NONE; }
#endif
#ifdef SS_HAS_DISPLAY
// On USB the battery pin shows the charger's output (about 4.1 V with or without a cell),
// so a percentage would be made up: report "USB" instead.
static uint8_t readBattery() {
#if ARDUINO_USB_MODE && !defined(SS_DISPLAY_TEMBED)   // the T-Embed's fuel gauge is right on USB too
  if (HWCDC::isPlugged()) return 254;
#endif
  return display::batteryPercent();
}
#endif
static bool nodeScreens = true;       // master: node screens on (sent in CONFIG)
static bool linkLr = false;
static uint32_t readmits = 0;
static String psramNote = "none";
static size_t psramTotal() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM); }
static String bootMemLog;   // free KB after each piece started at boot ("mem")
// Master pass timing: a node waits only ~200 ms for the master's answer, so a slow pass
// means missed answers. The longest pass since the last status, and a breakdown of any
// pass over kSlowPassMs ("[master] slow pass").
static constexpr uint32_t kSlowPassMs = 250;
static uint32_t loopMaxMs = 0, slowPasses = 0;
enum LoopPhase : uint8_t { LP_SERIAL, LP_WEB, LP_HITS, LP_RADIO, LP_REPORT, LP_MAINTAIN, LP_LOG, LP_OUTPUT, LP_COUNT };
static const char* const kPhaseNames[LP_COUNT] = {"serial", "web", "hits", "radio", "ingest", "maintain", "log", "output"};
static uint32_t phaseMs[LP_COUNT];
static uint32_t phaseMark = 0;
static inline void phaseEnd(LoopPhase p) { const uint32_t t = millis(); phaseMs[p] += t - phaseMark; phaseMark = t; }          // master: offline nodes taken back on their own frames           // master: Long Range link to nodes that can do it
static bool localScreen = true;       // this board's own screen, when it isn't a node
static volatile bool queryDone = false;
static char queryMacStr[18];
static uint8_t queryAddrType = 0;
static String queryJson;          // result, built in the task, printed by loop()
static Name queryName;            // device name to put in the table, if one came back
static bool needAssign = false;
static uint32_t badAuth = 0, badFrames = 0;

// Wi-Fi access point (standalone, master): phone/laptop UI with no cable
static bool apOn = false;           // saved setting
static bool apActive = false;       // actually running (never on a node)
static String apName, apPass;

// Node
static uint8_t pinnedCore[6];
static bool corePinned = false;
static bool paired = false;
static uint8_t coreMac[6];
static uint32_t lastCoreContact = 0;
// Health, reported in every heartbeat so the master can show why a node dropped out.
static constexpr uint32_t kNoMasterRebootMs = 5 * 60 * 1000;  // node: restart after this long alone
// ...but a sudden loss with a strong link has always been this node's radio getting stuck
// (it hears nothing, and comes back within 2 s of a restart), so the first few times in a
// row it restarts after just 60 s. If restarting doesn't find the master either (the
// master really is off), it settles back to every 5 minutes.
static constexpr uint32_t kNoMasterQuickMs = 60 * 1000;
static constexpr uint8_t kNoMasterQuickTries = 3;
static constexpr uint32_t kSelfRestartTag = 0x55AA0006;
static constexpr uint32_t kRadioRestartTag = 0x55AA0009;
static constexpr uint32_t kRadioDeadMs = 90000;   // master / standalone: no Wi-Fi frame at all this long = radio didn't start
RTC_NOINIT_ATTR static uint32_t rtcRestartTag;  // survives a software restart, not power loss
RTC_NOINIT_ATTR static uint32_t rtcNoMasterRuns;  // quick no-master restarts in a row
static uint8_t bootWhy = RW_UNKNOWN;
static uint16_t linkLosses = 0;
static uint32_t lostSince = 0;
static uint8_t readResetWhy() {
  const esp_reset_reason_t r = esp_reset_reason();
  const bool self = rtcRestartTag == kSelfRestartTag, radio = rtcRestartTag == kRadioRestartTag;
  rtcRestartTag = 0;
  if (r == ESP_RST_SW && radio) return RW_RADIO;
  switch (r) {
    case ESP_RST_POWERON: case ESP_RST_EXT: return RW_POWER;
    case ESP_RST_SW: return self ? RW_NO_MASTER : RW_SOFTWARE;
    case ESP_RST_PANIC: return RW_CRASH;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return RW_WATCHDOG;
    case ESP_RST_BROWNOUT: return RW_BROWNOUT;
    case ESP_RST_DEEPSLEEP: return RW_DEEPSLEEP;
#if ESP_IDF_VERSION_MAJOR >= 5
    case ESP_RST_USB: case ESP_RST_JTAG: return RW_USB;
#endif
    default: return RW_UNKNOWN;
  }
}
static uint8_t nodeSlot = 0xFF;

// Local scanner (every role)
static Config cfg;                         // current scan assignment
static volatile uint16_t catMask = 0;      // read in radio callbacks
static volatile int8_t rssiFloor = -95;
static volatile uint8_t curChannel = kHomeChannel;
static uint8_t hopCursor = kChannelCount - 1;
static volatile uint32_t wifiFrames = 0, bleAdvs = 0, queueDrops = 0;
static uint32_t lastHop = 0, lastReport = 0, lastHeartbeat = 0, lastHello = 0;
static uint32_t lastStatus = 0, lastMaintain = 0, droppedShown = 0;

static Name pendingNames[8];
static uint8_t nPendingNames = 0;

static bool hasTable() { return runRole != RUN_NODE; }
static bool roleWantsBle() { return cfg.role == ROLE_BLE || cfg.role == ROLE_BOTH; }
static bool roleWantsWifi() { return cfg.role == ROLE_WIFI || cfg.role == ROLE_BOTH; }
// Probed network names only while someone turned them on (they're chatty: phones rotate
// their address and ask again every few seconds).
static bool probesWanted() { return runRole == RUN_NODE ? (cfg.node_flags & NF_PROBES) != 0 : g_probesOn; }

// ---------------------------------------------------------------------------
// Radio callbacks: classify, filter, enqueue. Keep them short.

static void enqueue(const Sighting& s, const char* name, uint8_t name_len, uint8_t what = NAME_DEVICE,
                    const ApMeta* ap = nullptr, bool note_only = false) {
  RxItem it;
  it.s = s;
  it.note_only = note_only;
  it.name_len = name_len;
  it.what = what;
  it.has_ap = ap != nullptr;
  if (ap) it.ap = *ap;
  if (name_len) memcpy(it.name, name, name_len);
  it.name[name_len] = '\0';
  if (xQueueSend(hitQueue, &it, 0) != pdTRUE) queueDrops = queueDrops + 1;
}

#ifdef SS_LORA
static void loraSink(const Sighting& s, const char* name, uint8_t name_len, uint8_t what) { enqueue(s, name, name_len, what); }
#endif

// Wi-Fi clients heard in their own traffic: at most one report per client every few
// seconds (a busy client sends hundreds of frames a second). Wi-Fi task only.
static bool clientDue(const uint8_t mac[6], uint32_t now) {
  static uint8_t macs[48][6];
  static uint32_t at[48];
  static uint8_t used = 0;
  size_t oldest = 0;
  for (size_t i = 0; i < used; ++i) {
    if (memcmp(macs[i], mac, 6) == 0) {
      if (now - at[i] < 3000) return false;
      at[i] = now;
      return true;
    }
    if (at[i] < at[oldest]) oldest = i;
  }
  const size_t slot = used < 48 ? used++ : oldest;
  memcpy(macs[slot], mac, 6);
  at[slot] = now;
  return true;
}

// Remote ID: the drone's serial, where it is and where its pilot is, as notes on it.
static void enqueueRemoteId(const Sighting& s, const RemoteId& r) {
  char t[kMaxName + 1];
  auto e7 = [](char* o, size_t n, int32_t v) {
    const long a = labs((long)v);
    return snprintf(o, n, "%s%ld.%07ld", v < 0 ? "-" : "", a / 10000000, a % 10000000);
  };
  int l;
  if (r.has_id) {
    l = snprintf(t, sizeof(t), "%s|%u|%u", r.id, r.id_type, r.ua_type);
    enqueue(s, t, (uint8_t)(l < (int)kMaxName ? l : (int)kMaxName), NAME_RID_ID, nullptr, true);
  }
  if (r.has_loc) {
    l = e7(t, sizeof(t), r.lat_e7); t[l++] = ',';
    l += e7(t + l, sizeof(t) - l, r.lon_e7);
    l += snprintf(t + l, sizeof(t) - l, ",%d", r.alt_m);
    enqueue(s, t, (uint8_t)(l < (int)kMaxName ? l : (int)kMaxName), NAME_POS, nullptr, true);
    l = snprintf(t, sizeof(t), "%d,%u,%u,%u", r.height_m, r.speed_cms, r.dir, r.status);
    enqueue(s, t, (uint8_t)l, NAME_RID_MOVE, nullptr, true);
  }
  if (r.has_op) {
    l = e7(t, sizeof(t), r.op_lat_e7); t[l++] = ',';
    l += e7(t + l, sizeof(t) - l, r.op_lon_e7);
    enqueue(s, t, (uint8_t)(l < (int)kMaxName ? l : (int)kMaxName), NAME_RID_OP, nullptr, true);
  }
  if (r.text[0]) enqueue(s, r.text, (uint8_t)strlen(r.text), NAME_RID_TEXT, nullptr, true);
}

static void onWifiData(const wifi_promiscuous_pkt_t* pkt) {
  const int len = pkt->rx_ctrl.sig_len - 4;
  const int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
  if (len < 24 || !roleWantsWifi() || rssi < rssiFloor) return;
  WifiFrame f;
  // Only frames a client sends to its access point: the signal is the client's own.
  if (!parseWifiFrame(pkt->payload, (size_t)len, f) || !f.to_ds || f.from_ds) return;
  if (macIsMulticast(f.addr2) || macIsMulticast(f.addr1) || memcmp(f.addr1, f.addr2, 6) == 0) return;
  if (!clientDue(f.addr2, millis())) return;
  const Match m = classifyWifi(f);
  if (!(catMask & catBit(m.category))) return;
  char bssid[18];
  formatMac(f.addr1, bssid);
  enqueue(makeWifiSighting(f, m, curChannel, rssi), bssid, 17, NAME_LINK);   // and which network it's on
}

static void onPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_DATA) { onWifiData((const wifi_promiscuous_pkt_t*)buf); return; }
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  wifiFrames = wifiFrames + 1;
  int len = pkt->rx_ctrl.sig_len - 4;  // strip FCS
  if (len < 24) return;
  const int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;

  WifiFrame f;
  if (!parseWifiFrame(pkt->payload, (size_t)len, f)) return;
  const uint8_t ch = curChannel;

  if (f.kind == KIND_WIFI_OTHER) {  // deauth / disassoc floods
    if (catMask & catBit(CAT_PENTEST)) {
      portENTER_CRITICAL(&floodMux);
      flood.onFrame(f, ch, millis());
      portEXIT_CRITICAL(&floodMux);
    }
    return;
  }
  if (!roleWantsWifi() || rssi < rssiFloor || macIsMulticast(f.addr2)) return;
  if ((f.kind == KIND_WIFI_BEACON || f.kind == KIND_WIFI_PROBE_RESP) && (catMask & catBit(CAT_PENTEST))) {
    portENTER_CRITICAL(&floodMux);
    flood.onApFrame(f, ch, millis());
    portEXIT_CRITICAL(&floodMux);
  }
  const Match m = classifyWifi(f);
  if (!(catMask & catBit(m.category))) return;
  const Sighting s = makeWifiSighting(f, m, ch, rssi);
  const bool apFrame = f.kind == KIND_WIFI_BEACON || f.kind == KIND_WIFI_PROBE_RESP;
  const bool hasName = f.has_ssid && f.ssid_len && f.ssid[0];
  if (apFrame) {
    // An access point's SSID becomes its name (hidden networks send an empty or zeroed
    // one), plus what its beacon says about it.
    ApMeta ap;
    ap.sec = f.sec; ap.gen = f.gen; ap.width = f.width; ap.util = f.util; ap.sta = f.sta;
    if (f.tsf && f.tsf < 157680000ull * 1000000ull) ap.up_s = (uint32_t)(f.tsf / 1000000ull);   // < 5 years: plausible
    strncpy(ap.wps, f.wps, kMaxWps);
    ap.wps[kMaxWps] = '\0';
    enqueue(s, hasName ? f.ssid : nullptr, hasName ? f.ssid_len : 0, NAME_DEVICE, &ap);
    RemoteId rid;
    if (f.rid && decodeRemoteIdWifi(f.rid, f.rid_len, rid)) enqueueRemoteId(s, rid);
  } else if (f.kind == KIND_WIFI_PROBE_REQ && hasName && probesWanted()) {
    enqueue(s, f.ssid, f.ssid_len, NAME_PROBE);   // a network this phone is looking for
  } else {
    enqueue(s, nullptr, 0);
  }
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    bleAdvs = bleAdvs + 1;
    const int rssi = dev->getRSSI();
    if (rssi < rssiFloor) return;
    BleAdv a;
    const uint8_t* le = dev->getAddress().getVal();  // NimBLE stores LSB first
    for (int i = 0; i < 6; ++i) a.addr[i] = le[5 - i];
    a.addr_type = dev->getAddress().getType() == BLE_ADDR_PUBLIC ? SS_ADDR_PUBLIC : SS_ADDR_RANDOM;
    a.rssi = (int8_t)rssi;
    a.connectable = dev->isConnectable();
    const std::vector<uint8_t>& payload = dev->getPayload();
    parseBleAdv(payload.data(), payload.size(), a);
    if (inspectOn && !inspectReady && memcmp(a.addr, inspectWant.mac, 6) == 0) {
      portENTER_CRITICAL(&inspectMux);
      memcpy(inspectHit.mac, a.addr, 6);
      inspectHit.len = (uint8_t)(payload.size() > sizeof(inspectHit.data) ? sizeof(inspectHit.data) : payload.size());
      memcpy(inspectHit.data, payload.data(), inspectHit.len);
      inspectHit.rssi = a.rssi;
      inspectHit.random = a.addr_type != SS_ADDR_PUBLIC;
      inspectHit.connectable = a.connectable;
      inspectReady = true;
      portEXIT_CRITICAL(&inspectMux);
    }
    if (!a.name_len) a.name_len = bleModelName(a, a.name);  // e.g. "AirPods Pro (2nd gen)"
    const Match m = classifyBle(a);
    Sighting s = makeBleSighting(a, m, 0);
    if (!(catMask & catBit(m.category))) {
      // Pentest on: still forward fakeable adverts so BLE spam can be spotted,
      // flagged so they stay out of the (disabled) category's counts.
      if (!(catMask & catBit(CAT_PENTEST)) || !isSpoofableSig(m.sig_id)) return;
      s.flags |= SF_SPAM_PROBE;
    }
    enqueue(s, a.name, a.name_len);
    RemoteId rid;
    if (m.sig_id == SIG_REMOTE_ID_BLE && remoteIdFromBle(a, rid)) enqueueRemoteId(s, rid);
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    // Restart if the stack stopped us (e.g. after a duty change).
    if (queryBusy || bleHeld) return;  // paused on purpose
    if (roleWantsBle() && cfg.ble_duty_pct > 0) NimBLEDevice::getScan()->start(0, true, false);
  }
};
static ScanCallbacks scanCallbacks;

// ---------------------------------------------------------------------------
// Local scan plan

// Pause / resume BLE scanning from this (Arduino loop) task.
// Not NimBLEScan::stop() / start(..., isContinue=false): those free NimBLE's internal
// list of half-received adverts from this task while NimBLE's own task may be using
// it, a race that crashed the Heltec (heap corruption in NimBLEScan::clearResults)
// when it was stopping and starting the scan several times a second looking for a
// lost master. NimBLE cleans that list up itself, in its own task.
static void bleStop() { ble_gap_disc_cancel(); }
static void bleStart() { NimBLEDevice::getScan()->start(0, true, false); }

static void applyBleDuty() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  const uint8_t duty = roleWantsBle() ? cfg.ble_duty_pct : 0;
  bleStop();
  if (duty == 0) return;
  // NimBLE takes milliseconds: 100 ms interval, window = duty share of it.
  const uint16_t interval = 100;
  uint16_t window = (uint16_t)((interval * duty) / 100);
  if (window < 10) window = 10;
  scan->setInterval(interval);
  scan->setWindow(window);
  bleStart();
}

static void applyConfig(const Config& c) {
#ifdef SS_HAS_DISPLAY
  if (runRole == RUN_NODE && c.cfg_version) display::setOn(!(c.node_flags & NF_DISPLAY_OFF));
#endif
  // Long Range link to the master, when the master turned it on
  if (runRole == RUN_NODE && paired && c.cfg_version) {
    espLink.setLrFast((c.node_flags & NF_LR_FAST) != 0);
    espLink.setPeerLongRange(coreMac, (c.node_flags & NF_LONG_RANGE) != 0);
  }
  const bool bleChanged = c.ble_duty_pct != cfg.ble_duty_pct || c.role != cfg.role;
  cfg = c;
  catMask = c.category_mask;
  rssiFloor = c.rssi_floor;
  if (bleChanged) applyBleDuty();
  Out.printf("[%s] scan plan v%u role=%s cats=0x%04X channels=%u dwell=%u report=%u ble=%u%%\n",
                runRoleName(runRole), c.cfg_version, roleName(c.role), c.category_mask,
                channelCount(c.channel_mask), c.dwell_ms, c.report_ms, c.ble_duty_pct);
}

// Node before the master assigns anything: listen to everything on the home channel.
static void nodeDefaultConfig() {
  Config c;
  c.cfg_version = 0;
  // Unpaired: nothing to report to, so keep the radio on the home channel listening for
  // a master's pairing offer (no Bluetooth scanning taking turns with it).
  c.role = unpaired ? ROLE_WIFI : ROLE_BOTH;
  c.category_mask = kAllCategoriesMask;
  c.channel_mask = 0;
  c.ble_duty_pct = unpaired ? 0 : 70;
  c.report_ms = 1000;
  applyConfig(c);
}

// Standalone: the whole plan on this one radio, same rules as a single node.
// Master: its own radio never leaves the home channel (it must hear the nodes),
// so it gets BLE at a modest duty plus Wi-Fi on the home channel only.
static void applyLocalPlan() {
  Config c;
  c.cfg_version = (uint8_t)(cfg.cfg_version + 1);
  c.category_mask = userCats;
  c.report_ms = 1000;
  if (runRole == RUN_MASTER) {
    const ScanPlan plan = planForCategories(userCats);
    c.channel_mask = 0;
    c.ble_duty_pct = localScan && plan.ble_duty_pct ? kMasterLocalDuty : 0;
    c.role = !localScan ? ROLE_BLE : (!plan.channel_mask ? ROLE_BLE : (!plan.ble_duty_pct ? ROLE_WIFI : ROLE_BOTH));
    if (!localScan) c.category_mask = 0;
  } else if (apActive) {
    // Standalone serving its own Wi-Fi: an access point can't hop, so Wi-Fi listens
    // on the AP's channel only, and BLE leaves the radio enough air time for the phone.
    const ScanPlan plan = planForCategories(userCats);
    c.channel_mask = 0;
    c.ble_duty_pct = plan.ble_duty_pct > kApMaxBleDuty ? kApMaxBleDuty : plan.ble_duty_pct;
    c.role = !plan.channel_mask ? ROLE_BLE : (!plan.ble_duty_pct ? ROLE_WIFI : ROLE_BOTH);
  } else {
    const ScanPlan plan = planForCategories(userCats);
    c.channel_mask = plan.channel_mask;
    c.ble_duty_pct = plan.ble_duty_pct;
    c.dwell_ms = plan.dwell_ms;
    c.role = !plan.channel_mask ? ROLE_BLE : (!plan.ble_duty_pct ? ROLE_WIFI : ROLE_BOTH);
  }
  applyConfig(c);
}

static void goHome() {
  if (curChannel != kHomeChannel) {
    setRadioChannel(kHomeChannel);
    curChannel = kHomeChannel;
  }
}

// ---------------------------------------------------------------------------
// Settings

static void saveAndReboot(const char* why) {
  Out.printf("{\"t\":\"ack\",\"cmd\":\"%s\",\"reboot\":true}\n", why);
  Out.printf("[%s] %s saved, rebooting\n", runRoleName(runRole), why);
  Serial.flush();
  delay(300);
  ESP.restart();
}

static void setCats(uint16_t m) {
  userCats = m & kAllCategoriesMask;
  prefs.putUShort("cats", userCats);
  if (runRole == RUN_MASTER) needAssign = true;
  applyLocalPlan();
}

// ---------------------------------------------------------------------------
// Node side of the link

// A node that was told to use Long Range calls for a lost master in both modes, in
// turns, so a master that has since switched LR off (or a new one) still hears it.
static void helloRate() {
  static bool lr = false;
  const bool want = (cfg.node_flags & NF_LONG_RANGE) && espLink.lrCapable();
  lr = want ? !lr : false;
  espLink.setBroadcastLongRange(lr);
}

static void sendHello() {
  helloRate();
  Hello h;
  memcpy(h.mac, myMac, 6);
  h.fw_version = SS_FW_VERSION;
  h.hw = SS_HW_TYPE;
  h.caps = CAP_WIFI24 | CAP_BLE;
#if SOC_WIFI_SUPPORT_5G
  h.caps |= CAP_WIFI5;
#endif
#ifdef SS_LORA
  if (lora::ready()) h.caps |= CAP_LORA_915;
#endif
#ifdef SS_GPS
  h.caps |= CAP_GPS;
#endif
  h.role_pref = ROLE_AUTO;
  uint8_t buf[32];
  h.auth = espLink.authTag(buf, helloAuthInput(h, buf, sizeof(buf)));
  const size_t n = encodeHello(h, buf, sizeof(buf));
  // Broadcast, signed with the cluster key (not encrypted): the master can't decrypt a
  // node it hasn't met yet. Everything after WELCOME is encrypted.
  espLink.send(nullptr, MSG_HELLO, myId, buf, n);
}

static void sendHeartbeat() {
  Heartbeat hb;
  hb.uptime_s = millis() / 1000;
  hb.cfg_version = cfg.cfg_version;
  hb.channel = curChannel;
  const uint32_t dropped = batch.takeDropped() + queueDrops;
  hb.dropped = (uint16_t)(dropped > 0xFFFF ? 0xFFFF : dropped);
  queueDrops = 0;
  hb.wifi_frames = wifiFrames;
  hb.ble_adv = bleAdvs;
  hb.batt_pct = battPct;
  hb.reset = bootWhy;
  hb.heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
  hb.heap_min_kb = (uint16_t)(ESP.getMinFreeHeap() / 1024);
  hb.losses = linkLosses;
  hb.link = (espLink.lrCapable() ? HL_LR_CAPABLE : 0) | (paired && espLink.peerLongRange(coreMac) ? HL_LR_ON : 0);
  hb.gps = gpsState(millis());
#ifdef SS_GPS
  hb.gps_sats = gps.sats();
  {
    double la, lo;
    if (hb.gps >= GPS_FIX_2D && gps.position(la, lo)) {
      hb.has_pos = true;
      hb.lat_e7 = (int32_t)(la * 1e7);
      hb.lon_e7 = (int32_t)(lo * 1e7);
    }
    gps.utc(millis(), hb.utc);
  }
#endif
  uint8_t buf[64];
  espLink.send(coreMac, MSG_HEARTBEAT, myId, buf, encodeHeartbeat(hb, buf, sizeof(buf)));
#ifdef SS_GPS
  // Fix details and satellites for the web page's GPS window (every heartbeat, 5 s)
  if (hb.gps) {
    static GpsInfo gi;
    fillGpsInfo(gps, millis(), gi);
    uint8_t gb[kMaxPayload];
    espLink.send(coreMac, MSG_GPS, myId, gb, encodeGpsInfo(gi, gb, sizeof(gb)));
  }
#endif
}

// Forget the cluster (key and master) and come back up asking to be paired.
static void unpairAndRestart() {
  prefs.remove("key");
  prefs.remove("core");
  Out.println("{\"t\":\"ack\",\"cmd\":\"pair\",\"reboot\":true}");
  Out.println("[pair] cluster key dropped, restarting to pair again");
  Serial.flush();
  delay(200);
  ESP.restart();
}

static void pairRequestNow() { nodePair.request(); }

static void nodeHandlePacket(const RxPacket& p) {
  Header h;
  const uint8_t* pl;
  size_t n;
  if (!decodeFrame(p.data, p.len, h, &pl, &n)) return;
  if (unpaired) { nodePair.handle(p, h, pl, n, millis()); return; }  // only pairing until we have a key
  if (h.type == MSG_PAIR_REQ || h.type == MSG_PAIR_OFFER || h.type == MSG_PAIR_KEY) return;
  if (corePinned && memcmp(p.src, pinnedCore, 6) != 0) return;
  if (paired && memcmp(p.src, coreMac, 6) == 0) masterRssi = p.rssi;

  if (h.type == MSG_WELCOME) {
    Welcome w;
    if (!decodeWelcome(pl, n, w)) return;
    if (espLink.hasKey()) {
      uint8_t buf[32];
      if (w.auth != espLink.authTag(buf, welcomeAuthInput(w, myMac, buf, sizeof(buf)))) {
        Out.println("[node] WELCOME with bad auth ignored");
        return;
      }
    }
    if (!paired || memcmp(coreMac, p.src, 6) != 0) {
      memcpy(coreMac, p.src, 6);
      espLink.addPeer(coreMac);
      paired = true;
      rtcNoMasterRuns = 0;
      lostSince = 0;
      char s[18];
      formatMac(coreMac, s);
      Out.printf("[node] paired with master %s as node %u\n", s, w.node_index);
    }
    nodeSlot = w.node_index;
    lastCoreContact = millis();
  } else if (h.type == MSG_COMMAND) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    NodeCommand c;
    if (!decodeCommand(pl, n, c)) return;
    if (c.cmd == NC_REBOOT) {
      Out.println("[node] reboot requested by the master");
      Serial.flush();
      delay(100);
      ESP.restart();
    } else if (c.cmd == NC_IDENTIFY) {
      static uint32_t identifyStarted = 0;
      // The master repeats a request for ~16 s: one flash per request.
      if (!identifyStarted || millis() - identifyStarted > 20000) {
        identifyStarted = millis();
        identifyUntil = millis() + (uint32_t)(c.arg ? c.arg : 10) * 1000;
        Out.println("[node] identify requested by the master");
      }
    } else if (c.cmd == NC_LABEL && strcmp(nodeLabel, c.text) != 0) {
      memcpy(nodeLabel, c.text, sizeof(nodeLabel));
      Out.printf("[node] the master calls this node \"%s\"\n", nodeLabel);
    } else if (c.cmd == NC_UNPAIR) {
      Out.println("[node] the master forgot this node");
      unpairAndRestart();
    }
  } else if (h.type == MSG_CONFIG) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    Config c;
    if (!decodeConfig(pl, n, c)) return;
    lastCoreContact = millis();
    if (strcmp(nodeLabel, c.label) != 0) {  // the name you gave it on the master
      memcpy(nodeLabel, c.label, sizeof(nodeLabel));
      Out.printf("[node] the master calls this node \"%s\"\n", nodeLabel);
    }
    uint8_t a[kMaxConfigSize], b[kMaxConfigSize];
    const size_t la = encodeConfig(c, a, sizeof(a)), lb = encodeConfig(cfg, b, sizeof(b));
    if (la != lb || memcmp(a, b, la) != 0) applyConfig(c);  // only on a real change
  }
}

// ---------------------------------------------------------------------------
// Master side of the link

static void sendCommand(int slot, const NodeCommand& c) {
  uint8_t buf[40];
  espLink.send(nodes.at(slot).mac, MSG_COMMAND, myId, buf, encodeCommand(c, buf, sizeof(buf)));
}

// A node only hears the master while it isn't scanning Bluetooth, i.e. right after it
// checks in. So a command (identify, reboot) is held and repeated with each reply to
// its heartbeat for a while, and dropped once the node says hello again (it rebooted).
static constexpr uint32_t kCommandHoldMs = 16000;
static NodeCommand pendingCmd[kMaxNodes];
static uint32_t pendingUntil[kMaxNodes];
static void queueCommand(int slot, const NodeCommand& c) {
  pendingCmd[slot] = c;
  pendingUntil[slot] = millis() + kCommandHoldMs;
  sendCommand(slot, c);  // and try right away too
}
static void sendPending(int slot) {
  if (pendingUntil[slot] && (int32_t)(pendingUntil[slot] - millis()) > 0) sendCommand(slot, pendingCmd[slot]);
  else pendingUntil[slot] = 0;
}

static DashAlerts dashAlerts;
// Latest GPS details from each GPS node (MSG_GPS), for "gps <ID>" / the web page's GPS window
static GpsInfo gpsInfo[kMaxNodes];
static uint32_t gpsInfoMs[kMaxNodes] = {0};

// ---------------------------------------------------------------------------
// Remote SD card downloads (MSG_FILE_REQ / MSG_FILE), one at a time. The master asks a
// node for a window of chunks, passes each one to the page as base64, and asks again
// from where it got to. A lost frame only means asking again after a short wait.
static constexpr uint32_t kFileRetryMs = 350;
static constexpr uint8_t kFileTries = 40;   // ~14 s of silence: give up
static struct {
  bool on = false;
  int slot = -1;
  uint16_t id = 0;
  uint8_t req = 0, op = 0, tries = 0;
  char name[kMaxFileName + 1] = "";
  uint32_t next = 0, total = 0, winEnd = 0;   // read: bytes; list: entries
  uint32_t lastAsk = 0, started = 0;
  String list;
} xfer;

static const char* fileErrText(uint32_t e) {
  switch (e) {
    case FE_NO_CARD: return "no SD card in that node";
    case FE_NOT_FOUND: return "that file isn't on the card";
    case FE_BAD_NAME: return "bad file name";
    case FE_READ: return "the card stopped reading";
    case 99: return "cancelled";
    default: return "the node stopped answering";
  }
}

static void xferAsk(uint32_t now) {
  FileReq q;
  q.req = xfer.req; q.op = xfer.op; q.off = xfer.next; q.count = kFileWindow;
  memcpy(q.name, xfer.name, sizeof(q.name));
  xfer.winEnd = xfer.next + (uint32_t)kFileWindow * kFileChunk;
  uint8_t buf[64];
  espLink.send(nodes.at(xfer.slot).mac, MSG_FILE_REQ, myId, buf, encodeFileReq(q, buf, sizeof(buf)));
  xfer.lastAsk = now;
}

static void xferEnd(bool ok, uint32_t err) {
  if (xfer.op == FO_LIST) {
    if (ok) Out.printf("{\"t\":\"sd_ls\",\"node\":\"%04X\",\"files\":[%s],\"mounted\":true}\n", xfer.id, xfer.list.c_str());
    else Out.printf("{\"t\":\"sd_ls\",\"node\":\"%04X\",\"files\":[],\"mounted\":%s,\"err\":\"%s\"}\n", xfer.id,
                    err == FE_NO_CARD ? "false" : "true", fileErrText(err));
  } else {
    const uint32_t ms = millis() - xfer.started;
    Out.printf("{\"t\":\"rfile_end\",\"node\":\"%04X\",\"name\":\"%s\",\"ok\":%s,\"bytes\":%lu,\"ms\":%lu%s%s%s}\n", xfer.id,
               xfer.name, ok ? "true" : "false", (unsigned long)xfer.next, (unsigned long)ms,
               ok ? "" : ",\"err\":\"", ok ? "" : fileErrText(err), ok ? "" : "\"");
    Out.printf("[master] SD file %s from node %04X: %s, %lu bytes in %lu ms\n", xfer.name, xfer.id,
               ok ? "done" : fileErrText(err), (unsigned long)xfer.next, (unsigned long)ms);
  }
  xfer.on = false;
  xfer.list = "";
}

static bool xferStart(int slot, uint8_t op, const char* name, uint32_t off) {
  if (xfer.on && xfer.slot != slot) {   // the same node: the page is restarting or resuming it
    Out.printf("{\"t\":\"error\",\"msg\":\"Another SD download is running (%s from %04X). Wait for it, or cancel it.\"}\n",
               xfer.op == FO_LIST ? "file list" : xfer.name, xfer.id);
    return false;
  }
  xfer.on = true;
  xfer.slot = slot;
  xfer.id = nodes.at(slot).id;
  xfer.op = op;
  if (++xfer.req == 0) xfer.req = 1;
  xfer.tries = 0;
  xfer.next = off;
  xfer.total = 0;
  xfer.started = millis();
  xfer.list = "";
  memset(xfer.name, 0, sizeof(xfer.name));
  if (name) strncpy(xfer.name, name, kMaxFileName);
  xferAsk(millis());
  return true;
}

static void base64Out(const uint8_t* d, size_t n) {
  static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char out[4 * ((kFileChunk + 2) / 3) + 1];
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out[o++] = k[(v >> 18) & 63];
    out[o++] = k[(v >> 12) & 63];
    out[o++] = i + 1 < n ? k[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? k[v & 63] : '=';
  }
  out[o] = 0;
  Out.print(out);
}

static void xferOnChunk(int slot, const FileChunk& c, uint32_t now) {
  if (!xfer.on || slot != xfer.slot || c.req != xfer.req) return;   // stale: an old or cancelled request
  if (c.kind == FK_ERROR) { xferEnd(false, c.off); return; }
  xfer.tries = 0;
  if (xfer.op == FO_LIST && c.kind == FK_LIST) {
    if (c.off != xfer.next) return;
    xfer.total = c.total;
    uint32_t got = 0;
    for (size_t i = 0; i < c.len;) {
      const uint8_t l = c.data[i];
      if (i + 1 + l + 4 > c.len) break;
      char nm[kMaxFileName + 1];
      memcpy(nm, c.data + i + 1, l);
      nm[l] = 0;
      const uint8_t* z = c.data + i + 1 + l;
      const uint32_t size = (uint32_t)z[0] << 24 | (uint32_t)z[1] << 16 | (uint32_t)z[2] << 8 | z[3];
      if (fileNameOk(nm)) {
        char e[72];
        snprintf(e, sizeof(e), "%s{\"n\":\"%s\",\"b\":%lu}", xfer.list.length() ? "," : "", nm, (unsigned long)size);
        xfer.list += e;
      }
      i += 1 + l + 4;
      ++got;
    }
    xfer.next += got;
    if (xfer.next >= xfer.total || !got || xfer.list.length() > 12000) xferEnd(true, 0);
    else xferAsk(now);
  } else if (xfer.op == FO_READ && c.kind == FK_DATA) {
    xfer.total = c.total;
    if (c.off == xfer.next && c.len) {
      Out.printf("{\"t\":\"rfile\",\"node\":\"%04X\",\"name\":\"%s\",\"off\":%lu,\"total\":%lu,\"b64\":\"", xfer.id, xfer.name,
                 (unsigned long)c.off, (unsigned long)c.total);
      base64Out(c.data, c.len);
      Out.print("\"}\n");
      xfer.next += c.len;
    }
    if (xfer.next >= xfer.total) xferEnd(true, 0);
    else if (xfer.next >= xfer.winEnd || (c.off == xfer.next && !c.len)) xferAsk(now);
  }
}

static void xferTick(uint32_t now) {
  if (!xfer.on || (int32_t)(now - xfer.lastAsk) < (int32_t)kFileRetryMs) return;
  if (!nodes.at(xfer.slot).used || ++xfer.tries > kFileTries) { xferEnd(false, 0); return; }
  xferAsk(now);
}

static void emitGpsInfo(uint16_t id, const GpsInfo& g, uint32_t age_ms) {
  Out.printf("{\"t\":\"gps\",\"id\":\"%04X\",\"age\":%lu,\"st\":\"%s\",\"used\":%u,\"hdop\":%.1f", id,
             (unsigned long)(age_ms / 1000), gpsStateName(g.state), g.sats_used, g.hdop10 / 10.0);
  if (g.has_pos) Out.printf(",\"lat\":%.7f,\"lon\":%.7f", g.lat_e7 / 1e7, g.lon_e7 / 1e7);
  if (g.has_alt) Out.printf(",\"alt\":%d", g.alt_m);
  if (g.has_motion) Out.printf(",\"kmh\":%.1f,\"course\":%.1f", g.kmh10 / 10.0, g.course10 / 10.0);
  if (g.utc) Out.printf(",\"utc\":%lu", (unsigned long)g.utc);
  Out.print(",\"sats\":[");
  for (uint8_t i = 0; i < g.n_sat; ++i) {
    const auto& s = g.sat[i];
    Out.printf("%s[%u,%u,%u,%u,%u,%u]", i ? "," : "", s.sys, s.prn, s.elev, s.az2 * 2, s.snr, s.used ? 1 : 0);
  }
  Out.println("]}");
}
// The screen each dashboard node has open (DC_VIEW), so its data rides along with the dashboard.
struct DashViewState { uint8_t view; uint16_t offset; uint32_t id; };
static DashViewState dashView[kMaxNodes];

static void fillNodeRow(DashNode& r, const NodeInfo& n, uint32_t now) {
  (void)now;
  r.id = n.id;
  r.hw = n.hw;
  r.flags = (uint8_t)((n.online ? DN_ONLINE : 0) | (n.hb.cfg_version == n.cfg.cfg_version ? DN_ACK : 0));
  r.caps = n.caps;
  r.chans = channelCount(n.cfg.channel_mask);
  r.duty = n.cfg.ble_duty_pct;
  r.link = n.link_rssi;
  const uint32_t tot = n.frames + n.lost;
  const uint32_t loss = tot ? n.lost * 1000 / tot : 0;
  r.loss_pct = (uint8_t)(loss > 255 ? 255 : loss);
  r.batt = n.hb.batt_pct;
  r.gps = n.hb.gps;
  r.sats = n.hb.gps_sats;
  r.reset = n.hb.reset;
  r.heap_kb = n.hb.heap_kb;
  r.losses = n.hb.losses;
  r.up_s = n.hb.uptime_s;
  snprintf(r.label, sizeof(r.label), "%s", n.label);
}

static void sendView(int slot, uint32_t now) {
  const NodeInfo& n = nodes.at(slot);
  const DashViewState& v = dashView[slot];
  uint8_t buf[kMaxPayload];
  size_t len = 0;
  if (v.view == DV_DEVICES) {
    DashDevices dv;
    buildDeviceView(*table, now, userCats, v.offset, dv);
    len = encodeDashDevices(dv, buf, sizeof(buf));
  } else if (v.view == DV_DETAIL) {
    DashDetail dd;
    buildDetail(*table, v.id, now, dd);
    len = encodeDashDetail(dd, buf, sizeof(buf));
  } else if (v.view == DV_EVENTS) {
    DashEvents ev;
    dashAlerts.page(v.offset, now, ev);
    len = encodeDashEvents(ev, buf, sizeof(buf));
  } else if (v.view == DV_NODES) {
    // This master first, then every node it knows (online or not)
    DashNode all[kMaxNodes + 1];
    uint8_t total = 0;
    DashNode& me = all[total++];
    me.id = myId;
    me.hw = SS_HW_TYPE;
    me.flags = DN_ONLINE | DN_MASTER | DN_ACK;
    me.caps = CAP_WIFI24 | CAP_BLE;
#if SOC_WIFI_SUPPORT_5G
    me.caps |= CAP_WIFI5;
#endif
    me.duty = localScan ? cfg.ble_duty_pct : 0;
    me.chans = localScan ? 1 : 0;
    me.link = 0;
    me.batt = battPct;
    me.gps = gpsState(now);
    me.reset = bootWhy;
    me.heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
    me.up_s = now / 1000;
    for (int i = 0; i < kMaxNodes && total < kMaxNodes + 1; ++i)
      if (nodes.at(i).used) fillNodeRow(all[total++], nodes.at(i), now);
    DashNodes dn;
    dn.total = total;
    dn.offset = (uint8_t)(v.offset < total ? v.offset : 0);
    for (uint8_t i = dn.offset; i < total && dn.n < kDashNodes; ++i) dn.nodes[dn.n++] = all[i];
    len = encodeDashNodes(dn, buf, sizeof(buf));
  }
  if (len) espLink.send(n.mac, MSG_DASH_VIEW, myId, buf, len);
}
// Every detection-log record goes to the dashboard nodes too, for their SD cards.
static void sendLogRecord(const LogRec& r) {
  uint8_t buf[80];
  const size_t len = encodeLogRec(r, buf, sizeof(buf));
  if (!len) return;
  for (int i = 0; i < kMaxNodes; ++i) {
    const NodeInfo& n = nodes.at(i);
    if (n.used && n.online && (n.caps & CAP_DASH)) espLink.send(n.mac, MSG_LOG, myId, buf, len);
  }
}

// Dashboard nodes (CYD) get the summary right after each CONFIG, which they're
// listening for anyway. They check in every second, so it's about 1 Hz.
static void sendDash(int slot) {
  const NodeInfo& n = nodes.at(slot);
  if (!table || !(n.caps & CAP_DASH)) return;
  const uint32_t now = millis();
  Dash d;
  buildDash(*table, now, userCats, d);
  d.deploy = follow.deployment();
  d.nodes_online = nodes.onlineCount();
  for (int i = 0; i < kMaxNodes; ++i) if (nodes.at(i).used) ++d.nodes_total;
  dashAlerts.fill(d.alert, now);
  uint8_t buf[kMaxPayload];
  espLink.send(n.mac, MSG_DASH, myId, buf, encodeDash(d, buf, sizeof(buf)));
  sendView(slot, now);
}

static void setDeployment(uint8_t m) {
  follow.setDeployment(m);
  prefs.putUChar("deploy", m);
  Out.printf("[%s] deployment: %s\n", runRoleName(runRole), deploymentName(m));
}

static void sendConfig(int slot) {
  uint8_t buf[kMaxConfigSize];
  const NodeInfo& n = nodes.at(slot);
  // The node's name rides inside every CONFIG (the keep-alive the node must keep
  // hearing anyway), so it can't get lost on its own.
  Config c = n.cfg;
  memcpy(c.label, n.label, sizeof(c.label));
  espLink.send(n.mac, MSG_CONFIG, myId, buf, encodeConfig(c, buf, sizeof(buf)));
  sendPending(slot);
  sendDash(slot);
}

// What you set for a node on the master, kept by node id: job, screen, name.
struct NodePrefs { uint8_t force_role; int8_t screen; char label[kMaxLabel + 1]; };
static void nodePrefsKey(uint16_t id, char* key) { snprintf(key, 12, "nd_%04X", id); }
static void loadNodePrefs(int slot) {
  NodeInfo& n = nodes.at(slot);
  char key[12];
  nodePrefsKey(n.id, key);
  NodePrefs p;
  if (prefs.isKey(key) && prefs.getBytes(key, &p, sizeof(p)) == sizeof(p)) {
    n.force_role = p.force_role;
    n.screen = p.screen;
    memcpy(n.label, p.label, sizeof(n.label));
    n.label[kMaxLabel] = '\0';
  }
}
static void saveNodePrefs(int slot) {
  const NodeInfo& n = nodes.at(slot);
  char key[12];
  nodePrefsKey(n.id, key);
  NodePrefs p;
  p.force_role = n.force_role;
  p.screen = n.screen;
  memcpy(p.label, n.label, sizeof(p.label));
  prefs.putBytes(key, &p, sizeof(p));
}

static void sendWelcome(int slot, uint32_t now) {
  const NodeInfo& n = nodes.at(slot);
  Welcome w;
  memcpy(w.core_mac, myMac, 6);
  w.node_index = (uint8_t)slot;
  w.core_ms = now;
  uint8_t buf[32];
  w.auth = espLink.authTag(buf, welcomeAuthInput(w, n.mac, buf, sizeof(buf)));
  // Broadcast, signed for this node only: it can't decrypt us until it has added us.
  espLink.send(nullptr, MSG_WELCOME, myId, buf, encodeWelcome(w, buf, sizeof(buf)));
}

static void reassign() {
  nodes.assign(userCats, assignMode);
  for (int i = 0; i < kMaxNodes; ++i)
    if (nodes.at(i).used) {
      const NodeInfo& n = nodes.at(i);
      const bool on = n.screen < 0 ? nodeScreens : n.screen > 0;  // per-node setting wins
      const bool lr = linkLr && espLink.lrCapable() && (n.hb.link & HL_LR_CAPABLE);
      nodes.at(i).cfg.node_flags = (on ? 0 : NF_DISPLAY_OFF) | (lr ? NF_LONG_RANGE : 0) | (lr && espLink.lrFast() ? NF_LR_FAST : 0) |
                                   (g_probesOn ? NF_PROBES : 0);
      espLink.setPeerLongRange(n.mac, lr);
    }
  for (int i = 0; i < kMaxNodes; ++i)
    if (nodes.at(i).used && nodes.at(i).online) sendConfig(i);
  needAssign = false;
}

static void masterHandlePacket(const RxPacket& p, uint32_t now) {
  Header h;
  const uint8_t* pl;
  size_t n;
  if (!decodeFrame(p.data, p.len, h, &pl, &n)) { ++badFrames; return; }
  if (h.type == MSG_PAIR_REQ || h.type == MSG_PAIR_OFFER || h.type == MSG_PAIR_KEY) { masterPair.handle(p, h, pl, n, now); return; }

  if (h.type == MSG_HELLO) {
    Hello hello;
    if (!decodeHello(pl, n, hello) || memcmp(hello.mac, p.src, 6) != 0) { ++badFrames; return; }
    if (espLink.hasKey()) {
      uint8_t buf[32];
      if (hello.auth != espLink.authTag(buf, helloAuthInput(hello, buf, sizeof(buf)))) { ++badAuth; return; }
    }
    bool fresh = false;
    const int slot = nodes.add(hello, now, &fresh);
    if (slot < 0) return;
    masterPair.joined(hello.mac);
    nodes.at(slot).link_rssi = p.rssi;
    if (fresh) loadNodePrefs(slot);
    pendingUntil[slot] = 0;  // it restarted (or re-paired): old commands are done
    table->addClusterMac(hello.mac);
    espLink.addPeer(hello.mac);
    sendWelcome(slot, now);
    if (fresh) needAssign = true;
    else sendConfig(slot);
    if (fresh) {
      char t[kDashAlertText + 1];
      snprintf(t, sizeof(t), "Node %04X joined", nodes.at(slot).id);
      dashAlerts.note(0, CAT_NONE, t, now);
      dashView[slot] = DashViewState{DV_NONE, 0, 0};
    }
    if (g_jsonOn) {
      Out.printf("{\"t\":\"node\",\"event\":\"%s\",\"slot\":%d,\"id\":\"%04X\",\"hw\":%u,\"caps\":%u}\n",
                    fresh ? "joined" : "hello", slot, nodes.at(slot).id, hello.hw, hello.caps);
    }
    return;
  }

  // Everything else must come from a paired, online node.
  int slot = nodes.touch(p.src, now, p.rssi);
  if (slot < 0) {
    // A node this master knows, that it had marked offline, is still talking to it
    // (its frames are encrypted with the cluster key, so it's the real one). Take it
    // straight back and refresh it, instead of ignoring it until it gives up on us
    // (20 s) and starts saying hello again.
    const int known = nodes.find(p.src);
    if (known < 0 || !nodes.at(known).used || nodes.at(known).online || !espLink.hasKey()) return;
    if (h.type != MSG_HEARTBEAT && h.type != MSG_SIGHTINGS && h.type != MSG_NAME && h.type != MSG_EVENT) return;
    Hello again;
    memcpy(again.mac, p.src, 6);
    again.hw = nodes.at(known).hw;
    again.caps = nodes.at(known).caps;
    again.fw_version = nodes.at(known).fw;
    bool fresh = false;
    slot = nodes.add(again, now, &fresh);
    if (slot < 0) return;
    nodes.at(slot).link_rssi = p.rssi;
    ++readmits;
    Out.printf("[master] node %04X is back (heard it after marking it offline), re-admitted\n", nodes.at(slot).id);
    if (g_jsonOn) Out.printf("{\"t\":\"node\",\"event\":\"hello\",\"slot\":%d,\"id\":\"%04X\",\"hw\":%u,\"caps\":%u}\n",
                             slot, nodes.at(slot).id, again.hw, again.caps);
    sendWelcome(slot, now);
    needAssign = true;
  }
  nodes.noteSeq(slot, h.seq);

  switch (h.type) {
    case MSG_HEARTBEAT: {
      Heartbeat hb;
      if (!decodeHeartbeat(pl, n, hb)) { ++badFrames; return; }
      {
        // GPS just locked (or lost its fix): tell the page and the dashboards
        const uint8_t was = nodes.at(slot).hb.gps;
        const bool lockedNow = hb.gps >= GPS_FIX_2D, lockedBefore = was >= GPS_FIX_2D;
        if (hb.gps && lockedNow != lockedBefore && (lockedNow || was)) {
          char t[kDashAlertText + 1];
          if (lockedNow) snprintf(t, sizeof(t), "GPS lock: node %04X, %u sats", nodes.at(slot).id, hb.gps_sats);
          else snprintf(t, sizeof(t), "GPS lost its fix: node %04X", nodes.at(slot).id);
          dashAlerts.note(0, CAT_NONE, t, now);
          if (g_jsonOn)
            Out.printf("{\"t\":\"node\",\"event\":\"%s\",\"id\":\"%04X\",\"sats\":%u}\n", lockedNow ? "gps_lock" : "gps_lost",
                       nodes.at(slot).id, hb.gps_sats);
        }
      }
      if (linkLr && ((nodes.at(slot).hb.link ^ hb.link) & HL_LR_CAPABLE)) needAssign = true;
      nodes.at(slot).hb = hb;
      if (hb.has_pos) logger::setPosition(hb.lat_e7, hb.lon_e7, now);
      if (hb.utc) logger::setClock(hb.utc, now);
      // Every heartbeat gets the current CONFIG back: it is the node's keep-alive,
      // and it repairs a CONFIG the node missed while it was off channel.
      sendConfig(slot);
      break;
    }
    case MSG_GPS: {
      if (!decodeGpsInfo(pl, n, gpsInfo[slot])) { ++badFrames; return; }
      gpsInfoMs[slot] = now;
      break;
    }
    case MSG_FILE: {
      FileChunk c;
      if (!decodeFileChunk(pl, n, c)) { ++badFrames; return; }
      xferOnChunk(slot, c, now);
      break;
    }
    case MSG_SIGHTINGS: {
      Sighting s[kMaxSightingsPerFrame];
      size_t count = 0;
      if (!decodeSightings(pl, n, s, kMaxSightingsPerFrame, &count)) { ++badFrames; return; }
      ingestSightings(*table, (uint8_t)slot, s, count, now);
      break;
    }
    case MSG_NAME: {
      Name nm;
      if (!decodeName(pl, n, nm)) { ++badFrames; return; }
      applyName(*table, nm, now);
      break;
    }
    case MSG_EVENT: {
      Event ev;
      if (!decodeEvent(pl, n, ev)) { ++badFrames; return; }
      emitEvent(ev, slot);
      break;
    }
    case MSG_DASH_CMD: {
      // A tap on a dashboard node: same as doing it on the web page.
      DashCommand dc;
      if (!decodeDashCommand(pl, n, dc) || !(nodes.at(slot).caps & CAP_DASH)) { ++badFrames; return; }
      if (dc.cmd == DC_TOGGLE_CAT && dc.arg > 0 && dc.arg < CAT_COUNT) {
        setCats(userCats ^ catBit(dc.arg));
        Out.printf("[master] node %04X turned %s %s\n", nodes.at(slot).id, categoryLabel(dc.arg),
                   (userCats & catBit(dc.arg)) ? "on" : "off");
      } else if (dc.cmd == DC_DEPLOY && dc.arg <= DEPLOY_PORTABLE) {
        setDeployment(dc.arg);
      } else if (dc.cmd == DC_VIEW) {
        dashView[slot] = DashViewState{dc.arg, dc.offset, dc.id};
      }
      sendConfig(slot);  // show the change right away
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Reporting what this board heard

static void flushReports(uint32_t now) {
  Sighting out[kMaxSightingsPerFrame];
  Event ev;
  bool fire;
  portENTER_CRITICAL(&floodMux);
  fire = flood.poll(ev, now);
  portEXIT_CRITICAL(&floodMux);
#ifdef SS_SUBGHZ
  if (!fire) fire = subghz::pollEvent(ev);
#endif
  if (hasTable()) {
    // Straight into the local table (master: as slot 0).
    const uint8_t slot = runRole == RUN_MASTER ? kLocalSlot : 0;
    while (batch.size()) {
      const size_t n = batch.take(out, kMaxSightingsPerFrame, now);
      ingestSightings(*table, slot, out, n, now);
    }
    for (uint8_t i = 0; i < nPendingNames; ++i) applyName(*table, pendingNames[i], now);
    if (fire) emitEvent(ev, runRole == RUN_MASTER ? kLocalSlot : -1);
  } else {
    uint8_t buf[kMaxPayload];
    while (batch.size()) {
      const size_t n = batch.take(out, kMaxSightingsPerFrame, now);
      espLink.send(coreMac, MSG_SIGHTINGS, myId, buf, encodeSightings(out, n, buf, sizeof(buf)));
      sightingsSent += n;
    }
    for (uint8_t i = 0; i < nPendingNames; ++i)
      espLink.send(coreMac, MSG_NAME, myId, buf, encodeName(pendingNames[i], buf, sizeof(buf)));
    if (fire) espLink.send(coreMac, MSG_EVENT, myId, buf, encodeEvent(ev, buf, sizeof(buf)));
  }
  nPendingNames = 0;
}

// ---------------------------------------------------------------------------
// Status (one JSON line per second). Common head, then role-specific fields.

static void emitStatus(uint32_t now) {
  if (!g_jsonOn) return;
  if (hasTable()) droppedShown += batch.takeDropped();  // a node reports drops in its heartbeat
  Out.printf("{\"t\":\"status\",\"role\":\"%s\",\"setup\":%s,\"fw\":%u,\"hw\":%u,\"mac\":",
                runRoleName(runRole), setupDone ? "true" : "false", SS_FW_VERSION, (unsigned)SS_HW_TYPE);
  jsonMac(myMac);
  Out.printf(",\"ascan\":%s", activeScan ? "true" : "false");
  Out.printf(",\"rst\":\"%s\",\"heap\":%lu,\"heapmin\":%lu,\"losses\":%u", resetWhyName(bootWhy),
             (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getMinFreeHeap() / 1024), linkLosses);
  Out.printf(",\"heap_big\":%lu,\"psram\":%lu,\"psram_free\":%lu", (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned long)(psramTotal() / 1024), (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#ifdef SS_GPS
  Out.printf(",\"gps\":{\"st\":\"%s\",\"sats\":%u}", gpsStateName(gpsState(now)), gps.sats());
#endif
  if (battPct != 255) Out.printf(",\"batt\":%u", battPct);
#ifdef SS_LORA
  {
    const lora::Stats ls = lora::stats();
    Out.printf(",\"lora\":{\"ok\":%s,\"on\":", lora::ready() ? "true" : "false");
    jsonStr(ls.listening);
    Out.printf(",\"pk\":%lu,\"mesh\":%lu,\"read\":%lu,\"wan\":%lu,\"other\":%lu,\"crc\":%lu}", (unsigned long)ls.packets,
               (unsigned long)ls.meshtastic, (unsigned long)ls.mesh_read, (unsigned long)ls.lorawan, (unsigned long)ls.other, (unsigned long)ls.crc_errors);
  }
#endif
  Out.printf(",\"id\":\"%04X\",\"key\":%s,\"ms\":%lu,\"cats\":%u,\"ch\":%u,\"channels\":%u,\"ble_duty\":%u,"
                "\"wifi_frames\":%lu,\"ble_adv\":%lu,\"drops\":%lu",
                myId, espLink.hasKey() || clusterKey.length() ? "true" : "false", (unsigned long)now,
                hasTable() ? userCats : catMask, curChannel, channelCount(cfg.channel_mask), cfg.ble_duty_pct,
                (unsigned long)wifiFrames, (unsigned long)bleAdvs, (unsigned long)(queueDrops + droppedShown));

  if (runRole != RUN_NODE) {
    Out.print(",\"ap\":{\"on\":");
    Out.print(apActive ? "true" : "false");
    Out.print(",\"name\":");
    jsonStr(apName.c_str());
    Out.printf(",\"open\":%s,\"clients\":%u,\"viewers\":%u}", apPass.length() ? "false" : "true",
               portal::stationCount(), portal::viewerCount());
  }
  if (hasTable()) Out.printf(",\"deploy\":\"%s\",\"place\":%u", deploymentName(follow.deployment()), follow.place());
  if (runRole == RUN_NODE) {
    if (runRole == RUN_NODE) { Out.print(",\"label\":"); jsonStr(nodeLabel); }
    Out.printf(",\"unpaired\":%s", unpaired ? "true" : "false");
    if (unpaired) nodePair.status(Out, now);
  Out.printf(",\"paired\":%s,\"slot\":%d,\"pinned\":%s,\"scan\":\"%s\",\"cfg\":%u,\"core\":",
                  paired ? "true" : "false", paired ? (int)nodeSlot : -1, corePinned ? "true" : "false",
                  roleName(cfg.role), cfg.cfg_version);
    if (paired) jsonMac(coreMac); else Out.print("null");
    Out.printf(",\"send_fail\":%lu}\n", (unsigned long)espLink.sendFailures());
    return;
  }

  if (runRole == RUN_MASTER) {
    Out.printf(",\"mode\":\"%s\",\"local\":%s,\"cfg\":%u,\"nodes\":[", assignMode == ASSIGN_MIRROR ? "mirror" : "split",
                  localScan ? "true" : "false", nodes.cfgVersion());
    bool first = true;
    for (int i = 0; i < kMaxNodes; ++i) {
      const NodeInfo& n = nodes.at(i);
      if (!n.used) continue;
      Out.printf("%s{\"slot\":%d,\"id\":\"%04X\",\"mac\":", first ? "" : ",", i, n.id);
      jsonMac(n.mac);
      Out.printf(",\"online\":%s,\"role\":\"%s\",\"cfg\":%u,\"ack\":%s,\"ch\":%u,\"link\":%d,\"lost\":%lu,"
                    "\"frames\":%lu,\"dropped\":%u,\"up\":%lu,\"wifi\":%lu,\"ble\":%lu",
                    n.online ? "true" : "false", roleName(n.cfg.role), n.cfg.cfg_version,
                    n.hb.cfg_version == n.cfg.cfg_version ? "true" : "false", n.hb.channel, n.link_rssi,
                    (unsigned long)n.lost, (unsigned long)n.frames, n.hb.dropped, (unsigned long)n.hb.uptime_s,
                    (unsigned long)n.hb.wifi_frames, (unsigned long)n.hb.ble_adv);
      // What the NODES panel shows: board type, radios, its current job, battery, last heard
      Out.printf(",\"hw\":%u,\"caps\":%u,\"fw\":%u,\"chans\":%u,\"duty\":%u,\"batt\":%u,\"age\":%lu",
                 n.hw, n.caps, n.fw, channelCount(n.cfg.channel_mask), n.cfg.ble_duty_pct, n.hb.batt_pct,
                 (unsigned long)((now - n.last_ms) / 1000));
      Out.printf(",\"gps\":\"%s\",\"sats\":%u", gpsStateName(n.hb.gps), n.hb.gps_sats);
      if (n.hb.heap_kb)
        Out.printf(",\"rst\":\"%s\",\"heap\":%u,\"heapmin\":%u,\"losses\":%u", resetWhyName(n.hb.reset), n.hb.heap_kb,
                   n.hb.heap_min_kb, n.hb.losses);
      if (n.hb.sd) Out.printf(",\"sd\":\"%s\"", sdStateName(n.hb.sd));
      Out.printf(",\"lr\":%u", n.hb.link);
      Out.printf(",\"force\":\"%s\",\"screen\":%d,\"label\":", roleName(n.force_role), n.screen);
      jsonStr(n.label);
      Out.print("}");
      first = false;
    }
    Out.printf("],\"screens\":%s,\"lr\":{\"on\":%s,\"ok\":%s,\"rate\":%u}", nodeScreens ? "true" : "false", linkLr ? "true" : "false",
               espLink.lrCapable() ? "true" : "false", espLink.lrFast() ? 500u : 250u);
    masterPair.status(Out, now);
    Out.printf(",\"rx_dropped\":%lu,\"bad_auth\":%lu,\"bad_frames\":%lu,\"readmits\":%lu,\"loop_max\":%lu,\"slow\":%lu",
               (unsigned long)espLink.rxDropped(), (unsigned long)badAuth, (unsigned long)badFrames, (unsigned long)readmits,
               (unsigned long)loopMaxMs, (unsigned long)slowPasses);
    loopMaxMs = 0;
  }
  logger::status(Out, now);
  emitCounts(*table, now);
  Out.println("}");
}

// ---------------------------------------------------------------------------
// Serial commands

static void jsonAppend(String& j, const char* key, const std::string& v) {
  j += ",\"";
  j += key;
  j += "\":\"";
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') { j += '\\'; j += (char)c; }
    else if (c < 0x20 || c == 0x7F) j += ' ';
    else j += (char)c;
  }
  j += '"';
}

static std::string readChr(NimBLEClient* c, uint16_t svc, uint16_t chr) {
  NimBLERemoteService* s = c->getService(NimBLEUUID(svc));
  if (!s) return "";
  NimBLERemoteCharacteristic* ch = s->getCharacteristic(NimBLEUUID(chr));
  if (!ch || !ch->canRead()) return "";
  const NimBLEAttValue v = ch->readValue();
  return std::string((const char*)v.data(), v.size());
}

static void queryTask(void*) {
  bleStop();
  delay(50);
  String j = "{\"t\":\"query\",\"m\":\"";
  j += queryMacStr;
  j += '"';
  queryName.len = 0;
  NimBLEClient* c = NimBLEDevice::createClient();
  c->setConnectTimeout(6000);
  const NimBLEAddress addr(std::string(queryMacStr), queryAddrType);
  if (!c->connect(addr)) {
    j += ",\"ok\":false}";
  } else {
    j += ",\"ok\":true";
    const std::string name = readChr(c, 0x1800, 0x2A00);
    if (!name.empty()) {
      jsonAppend(j, "name", name);
      const size_t n = name.size() > kMaxName ? kMaxName : name.size();
      memcpy(queryName.name, name.data(), n);
      queryName.name[n] = '\0';
      queryName.len = (uint8_t)n;
    }
    const std::string app = readChr(c, 0x1800, 0x2A01);
    if (app.size() >= 2) j += ",\"appearance\":" + String((uint8_t)app[0] | ((uint8_t)app[1] << 8));
    const std::string mk = readChr(c, 0x180A, 0x2A29), model = readChr(c, 0x180A, 0x2A24),
                      fw = readChr(c, 0x180A, 0x2A26), hw = readChr(c, 0x180A, 0x2A27), sw = readChr(c, 0x180A, 0x2A28);
    if (!mk.empty()) jsonAppend(j, "maker", mk);
    if (!model.empty()) jsonAppend(j, "model", model);
    if (!fw.empty()) jsonAppend(j, "fw", fw);
    if (!hw.empty()) jsonAppend(j, "hw", hw);
    if (!sw.empty()) jsonAppend(j, "sw", sw);
    const std::string batt = readChr(c, 0x180F, 0x2A19);
    if (batt.size() >= 1) j += ",\"battery\":" + String((uint8_t)batt[0]);
    j += ",\"services\":[";
    bool first = true;
    for (NimBLERemoteService* s : c->getServices(true)) {
      j += first ? "\"" : ",\"";
      j += s->getUUID().toString().c_str();
      j += '"';
      first = false;
    }
    j += "]}";
    c->disconnect();
  }
  NimBLEDevice::deleteClient(c);
  queryJson = j;
  queryBusy = false;
  queryDone = true;
  applyBleDuty();  // resume scanning
  vTaskDelete(nullptr);
}

static bool parseMac(const String& m, uint8_t mac[6]) {
  return sscanf(m.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
                &mac[5]) == 6;
}

static uint8_t parseRole(const String& r) {
  if (r == "standalone" || r == "solo") return RUN_STANDALONE;
  if (r == "master" || r == "core") return RUN_MASTER;
  if (r == "node" || r == "slave") return RUN_NODE;
  return 0xFF;
}

// PSRAM on the ESP32-C5. The Arduino core turns PSRAM on after the heap is up, but on C5
// chips before revision 1.2 ESP-IDF first wants a small PSRAM buffer for a memory-barrier
// workaround ("MSPI Timing: Failed to allocate dummy cacheline"), which can't exist yet,
// and on some boards PSRAM then never reaches the heap: the board runs on its ~170 KB of
// internal RAM alone. So check, and finish the job ourselves: start it if needed, test it,
// add it to the heap, then set up that barrier buffer (now that PSRAM is there).
static void psramRescue() {
#ifdef SS_PSRAM_RESCUE
  if (psramTotal()) {   // the core started it; the barrier buffer it couldn't make can be made now
#if ESP_PSRAM_MSPI_MB_WORKAROUND
    psramNote = esp_psram_mspi_mb_init() == ESP_OK ? "ok" : "ok, barrier buffer FAILED";
#else
    psramNote = "ok";
#endif
    psramNote += String(", ") + (unsigned)(psramTotal() / 1024) + " KB";
    return;
  }
  if (!esp_psram_is_initialized()) {
    const esp_err_t e = esp_psram_init();
    if (e != ESP_OK) { psramNote = String("not found (") + esp_err_to_name(e) + ")"; return; }
  }
  const size_t size = esp_psram_get_size();
  if (!ssPsramTest()) { psramNote = "memory test FAILED, not used"; return; }
  const esp_err_t e = esp_psram_extram_add_to_heap_allocator();
  if (e != ESP_OK) { psramNote = String("couldn't add to the heap (") + esp_err_to_name(e) + ")"; return; }
#if ESP_PSRAM_MSPI_MB_WORKAROUND
  const bool mb = esp_psram_mspi_mb_init() == ESP_OK;
#else
  const bool mb = true;
#endif
  psramNote = String("started by SkySentinel: ") + (unsigned)(size / 1024) + " KB" + (mb ? "" : ", barrier buffer FAILED");
#else
  if (psramTotal()) psramNote = "ok";
#endif
}

static void memReport() {
  Out.printf("[mem] free %lu, lowest %lu, largest block %lu, PSRAM %lu KB, device table %u x %u bytes, hit queue %u\n",
             (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned long)(psramTotal() / 1024),
             table ? (unsigned)table->capacity() : 0u, (unsigned)sizeof(Device), (unsigned)uxQueueSpacesAvailable(hitQueue) + (unsigned)uxQueueMessagesWaiting(hitQueue));
  Out.printf("[mem] at boot, KB free after: %s\n", bootMemLog.c_str());
  Out.printf("[mem] PSRAM: %s, %lu KB free\n", psramNote.c_str(), (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void handleLine(String line, uint32_t now) {
  line.trim();
  if (!line.length()) return;
  if (line.startsWith("role ")) {
    String r = line.substring(5);
    r.trim();
    const uint8_t want = parseRole(r);
    if (want == 0xFF) { Out.println("[setup] roles: standalone, master, node"); return; }
    prefs.putUChar("role", want);
    prefs.putBool("setup", true);
    saveAndReboot("role");
  } else if (line == "mem") {
    memReport();
  } else if (line.startsWith("setup ") && line != "setup done" && line != "setup reset") {
    // First-run setup in one step: "setup <role> [key]" ("-" clears the key,
    // leaving it out keeps the current one). Saves everything, reboots once.
    String rest = line.substring(6);
    rest.trim();
    const int sp = rest.indexOf(' ');
    const uint8_t want = parseRole(sp < 0 ? rest : rest.substring(0, sp));
    if (want == 0xFF) { Out.println("[setup] usage: setup standalone|master|node [key]"); return; }
    if (sp > 0) {
      String k = rest.substring(sp + 1);
      k.trim();
      prefs.putString("key", k == "-" ? "" : k);
    }
    prefs.putUChar("role", want);
    prefs.putBool("setup", true);
    saveAndReboot("setup");
  } else if (line == "setup done") {
    setupDone = true;
    prefs.putBool("setup", true);
    Out.println("{\"t\":\"ack\",\"cmd\":\"setup\"}");
  } else if (line == "setup reset") {
    setupDone = false;
    prefs.putBool("setup", false);
    Out.println("{\"t\":\"ack\",\"cmd\":\"setup\"}");
  } else if (line.startsWith("key ")) {
    String k = line.substring(4);
    k.trim();
    prefs.putString("key", k == "-" ? "" : k);
    saveAndReboot("key");
  } else if (line.startsWith("wifi ")) {
    // "wifi on|off <password|-> <name with spaces>": one command, one reboot.
    String rest = line.substring(5);
    rest.trim();
    const int a = rest.indexOf(' ');
    const String state = a < 0 ? rest : rest.substring(0, a);
    if (state != "on" && state != "off") { Out.println("[wifi] usage: wifi on|off <password|-> <name>"); return; }
    if (a > 0) {
      String more = rest.substring(a + 1);
      more.trim();
      const int b = more.indexOf(' ');
      const String pass = b < 0 ? more : more.substring(0, b);
      if (pass != "-" && (pass.length() < 8 || pass.length() > 63)) {
        Out.println("{\"t\":\"error\",\"cmd\":\"wifi\",\"msg\":\"Password must be 8 to 63 characters, or none\"}");
        return;
      }
      prefs.putString("appass", pass == "-" ? "" : pass);
      if (b > 0) {
        String name = more.substring(b + 1);
        name.trim();
        if (name.length() > 32) name = name.substring(0, 32);
        if (name.length()) prefs.putString("apname", name);
      }
    }
    prefs.putBool("ap", state == "on");
    saveAndReboot("wifi");
  } else if (line == "factory") {
    prefs.clear();
    saveAndReboot("factory");
  } else if (line == "reboot") {
    saveAndReboot("reboot");
  } else if (handleCommonCommand(line)) {
    if (line.startsWith("probes ") && runRole == RUN_MASTER) needAssign = true;   // tell the nodes
  } else if (line.startsWith("gps ") || line == "gps") {
    // "gps <node id>": that node's fix and satellites as JSON (the web page's GPS window)
    const uint16_t id = line.length() > 4 ? (uint16_t)strtoul(line.c_str() + 4, nullptr, 16) : myId;
#ifdef SS_GPS
    if (id == myId) {
      static GpsInfo gi;
      fillGpsInfo(gps, now, gi);
      emitGpsInfo(myId, gi, 0);
      return;
    }
#endif
    int slot = -1;
    if (runRole == RUN_MASTER)
      for (int i = 0; i < kMaxNodes; ++i)
        if (nodes.at(i).used && nodes.at(i).id == id) slot = i;
    if (slot < 0 || !gpsInfoMs[slot]) {
      Out.printf("{\"t\":\"gps\",\"id\":\"%04X\",\"none\":true}\n", id);
      return;
    }
    emitGpsInfo(id, gpsInfo[slot], now - gpsInfoMs[slot]);
  } else if (logger::handleCommand(line)) {
  } else if (line == "status") {
    const bool j = g_jsonOn;
    g_jsonOn = true;
    emitStatus(now);
    g_jsonOn = j;
  } else if (hasTable() && line == "cats all") { setCats(kAllCategoriesMask);
  } else if (hasTable() && line == "cats surv") { setCats(kSurveillanceMask);
  } else if (hasTable() && line.startsWith("cats ")) { setCats((uint16_t)strtoul(line.c_str() + 5, nullptr, 16));
  } else if (hasTable() && line == "devices") { dumpDevices(*table, now);
  } else if (hasTable() && line.startsWith("aps ")) { emitAccessPointGroup(*table, strtoul(line.c_str() + 4, nullptr, 10), now);
  } else if (hasTable() && line.startsWith("info ")) { emitDeviceInfo(*table, strtoul(line.c_str() + 5, nullptr, 10), now);
  } else if (runRole == RUN_MASTER && (line == "mode split" || line == "mode mirror")) {
    assignMode = line == "mode mirror" ? ASSIGN_MIRROR : ASSIGN_SPLIT;
    prefs.putUChar("mode", (uint8_t)assignMode);
    needAssign = true;
  } else if (hasTable() && line.startsWith("deploy ")) {
    const String d = line.substring(7);
    const uint8_t m = d == "mobile" ? DEPLOY_MOBILE : d == "portable" ? DEPLOY_PORTABLE : d == "stationary" ? DEPLOY_STATIONARY : 0xFF;
    if (m == 0xFF) { Out.println("[deploy] stationary | mobile | portable"); return; }
    setDeployment(m);
  } else if (line == "scan active" || line == "scan passive") {
    activeScan = line == "scan active";
    prefs.putBool("ascan", activeScan);
    NimBLEScan* scan = NimBLEDevice::getScan();
    bleStop();
    scan->setActiveScan(activeScan);
    applyBleDuty();
    Out.printf("[%s] BLE scan %s\n", runRoleName(runRole), activeScan ? "active (asks devices for details)" : "passive (listen only)");
  } else if (line.startsWith("query ")) {
    // "query AA:BB:CC:DD:EE:FF r|p"
    uint8_t mac[6];
    String rest = line.substring(6);
    rest.trim();
    if (queryBusy) { Out.println("{\"t\":\"query\",\"ok\":false,\"busy\":true}"); return; }
    if (rest.length() < 17 || !parseMac(rest.substring(0, 17), mac)) return;
    formatMac(mac, queryMacStr);
    queryAddrType = rest.endsWith(" p") ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
    memcpy(queryName.addr, mac, 6);
    queryName.kind = KIND_BLE_ADV;
    queryBusy = true;
    if (xTaskCreate(queryTask, "query", 6144, nullptr, 1, nullptr) != pdPASS) queryBusy = false;
  } else if (line == "pair" || line.startsWith("pair ")) {
    // node: "pair" = forget the cluster and wait to be paired again
    // master: "pair start|stop", "pair accept|ignore|blink <mac>"
    String a = line.length() > 4 ? line.substring(5) : String("");
    a.trim();
    if (runRole == RUN_NODE) {
      if (a == "" || a == "again") unpairAndRestart();
      else Out.println("[pair] on a node: \"pair\" forgets its master and waits to be paired again");
    } else if (runRole == RUN_MASTER) {
      uint8_t mac[6];
      if (a == "" || a == "start") masterPair.start(now);
      else if (a == "stop") { masterPair.stop(); Out.println("[pair] pairing closed"); }
      else if (a.startsWith("accept ") && parseMac(a.substring(7), mac)) { if (!masterPair.accept(mac, now)) Out.println("[pair] not ready: bring it closer"); }
      else if (a.startsWith("ignore ") && parseMac(a.substring(7), mac)) masterPair.ignore(mac);
      else if (a.startsWith("blink ") && parseMac(a.substring(6), mac)) masterPair.blink(mac, now);
      else Out.println("[pair] pair start | stop | accept <mac> | ignore <mac> | blink <mac>");
    } else {
      Out.println("[pair] pairing is between a master and its nodes (this board is standalone)");
    }
  } else if (line == "inspect off") {
    inspectOn = false;
  } else if (line.startsWith("inspect ")) {
    uint8_t mac[6];
    if (parseMac(line.substring(8), mac)) {
      memcpy(inspectWant.mac, mac, 6);
      inspectReady = false;
      inspectOn = true;
    }
  } else if (runRole == RUN_MASTER && line.startsWith("node ")) {
    // node <ID> label <text>|-  role auto|ble|wifi|both  screen on|off|auto  identify  reboot  forget
    String rest = line.substring(5);
    rest.trim();
    const int sp = rest.indexOf(' ');
    const uint16_t id = (uint16_t)strtoul((sp < 0 ? rest : rest.substring(0, sp)).c_str(), nullptr, 16);
    int slot = -1;
    for (int i = 0; i < kMaxNodes; ++i)
      if (nodes.at(i).used && nodes.at(i).id == id) slot = i;
    if (slot < 0) { Out.printf("[master] no node %04X\n", id); return; }
    String cmd = sp < 0 ? String("") : rest.substring(sp + 1);
    cmd.trim();
    NodeInfo& n = nodes.at(slot);
    NodeCommand c;
    if (cmd.startsWith("label ")) {
      String l = cmd.substring(6);
      l.trim();
      if (l == "-") l = "";
      if (l.length() > kMaxLabel) l = l.substring(0, kMaxLabel);
      memset(n.label, 0, sizeof(n.label));
      memcpy(n.label, l.c_str(), l.length());
      saveNodePrefs(slot);
      if (n.online) sendConfig(slot);
    } else if (cmd.startsWith("role ")) {
      const String r = cmd.substring(5);
      n.force_role = r == "ble" ? ROLE_BLE : r == "wifi" ? ROLE_WIFI : r == "both" ? ROLE_BOTH : ROLE_AUTO;
      saveNodePrefs(slot);
      needAssign = true;
    } else if (cmd.startsWith("screen ")) {
      const String v = cmd.substring(7);
      n.screen = v == "on" ? 1 : v == "off" ? 0 : -1;
      saveNodePrefs(slot);
      needAssign = true;
    } else if (cmd == "identify") {
      c.cmd = NC_IDENTIFY; c.arg = 10;
      queueCommand(slot, c);
    } else if (cmd == "reboot") {
      c.cmd = NC_REBOOT;
      queueCommand(slot, c);
    } else if (cmd == "sdformat") {
      // Erase and format its SD card. Only boards with a card slot answer; the web page
      // asks for confirmation first. A fresh token per request (the command repeats).
      if (!(n.caps & CAP_DASH)) { Out.printf("[master] node %04X has no SD card slot\n", id); return; }
      static uint8_t token = 0;
      uint8_t t;
      do { t = (uint8_t)esp_random(); } while (t == 0 || t == token);
      token = t;
      c.cmd = NC_SD_FORMAT; c.arg = token;
      queueCommand(slot, c);
    } else if (cmd == "sd ls") {
      // Its SD card's files, through the cluster link ({"t":"sd_ls","node":...})
      if (!n.online) { Out.printf("{\"t\":\"error\",\"msg\":\"Node %04X is offline\"}\n", id); return; }
      xferStart(slot, FO_LIST, nullptr, 0);
      return;
    } else if (cmd.startsWith("sd get ")) {
      // sd get <file> [offset]: streams {"t":"rfile"} base64 pieces, then {"t":"rfile_end"}.
      // The offset resumes a download the page lost part of.
      String a = cmd.substring(7);
      a.trim();
      uint32_t off = 0;
      const int sp2 = a.indexOf(' ');
      if (sp2 > 0) { off = strtoul(a.substring(sp2 + 1).c_str(), nullptr, 10); a = a.substring(0, sp2); }
      if (!fileNameOk(a.c_str())) { Out.println("{\"t\":\"error\",\"msg\":\"Bad file name\"}"); return; }
      if (!n.online) { Out.printf("{\"t\":\"error\",\"msg\":\"Node %04X is offline\"}\n", id); return; }
      xferStart(slot, FO_READ, a.c_str(), off);
      return;
    } else if (cmd == "sd cancel") {
      if (xfer.on && xfer.slot == slot) xferEnd(false, 99);
      return;
    } else if (cmd == "forget") {
      // Tell it to drop the cluster key too (a few tries: it may be off channel)
      NodeCommand u;
      u.cmd = NC_UNPAIR;
      for (int k = 0; k < 3; ++k) { sendCommand(slot, u); delay(30); }
      char key[12];
      nodePrefsKey(n.id, key);
      prefs.remove(key);
      nodes.forget(slot);
      pendingUntil[slot] = 0;
      needAssign = true;
    } else {
      Out.println("[master] node <ID> label <text>|-, role auto|ble|wifi|both, screen on|off|auto, identify, reboot, sdformat, sd ls, sd get <file> [offset], sd cancel, forget");
      return;
    }
    Out.printf("{\"t\":\"ack\",\"cmd\":\"node\",\"id\":\"%04X\"}\n", id);
  } else if (runRole == RUN_MASTER && (line == "link lr on" || line == "link lr off")) {
    const bool want = line == "link lr on";
    if (want && !espLink.lrCapable()) {
      Out.println("{\"t\":\"error\",\"msg\":\"This master's radio can't do Long Range mode\"}");
      return;
    }
    linkLr = want;
    prefs.putBool("linklr", linkLr);
    needAssign = true;   // per-node flags and rates go out with the next CONFIG
    Out.printf("[master] Long Range link %s\n", linkLr ? "on (for nodes that can do it)" : "off");
    Out.println("{\"t\":\"ack\",\"cmd\":\"link lr\"}");
  } else if (runRole == RUN_MASTER && (line == "link lr rate 250" || line == "link lr rate 500")) {
    const bool fast = line.endsWith("500");
    espLink.setLrFast(fast);
    prefs.putBool("lrfast", fast);
    needAssign = true;   // nodes pick it up with the next CONFIG
    Out.printf("[master] Long Range rate %s kbit/s\n", fast ? "500" : "250");
    Out.println("{\"t\":\"ack\",\"cmd\":\"link lr rate\"}");
  } else if (runRole == RUN_MASTER && (line == "screens on" || line == "screens off")) {
    nodeScreens = line == "screens on";
    prefs.putBool("nodescr", nodeScreens);
    needAssign = true;  // goes out with the next CONFIG
#ifdef SS_HAS_DISPLAY
  } else if (runRole != RUN_NODE && (line == "screen on" || line == "screen off")) {
    localScreen = line == "screen on";
    prefs.putBool("screen", localScreen);
    display::setOn(localScreen);
#endif
  } else if (runRole == RUN_MASTER && (line == "local on" || line == "local off")) {
    localScan = line == "local on";
    prefs.putBool("local", localScan);
    applyLocalPlan();
  } else if (runRole == RUN_NODE && line.startsWith("core ")) {
    String m = line.substring(5);
    m.trim();
    uint8_t mac[6];
    if (m == "-") { prefs.remove("core"); saveAndReboot("core"); }
    else if (parseMac(m, mac)) { prefs.putBytes("core", mac, 6); saveAndReboot("core"); }
  } else if (runRole == RUN_NODE && line.startsWith("cats")) {
    Out.println("[node] categories are set on the master");
  } else {
    Out.printf("[%s] unknown command: %s\n", runRoleName(runRole), line.c_str());
  }
}

static void handleSerial(uint32_t now) {
  static String line;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') { if (line.length() < 80) line += c; continue; }
    handleLine(line, now);
    line = "";
  }
}

// ---------------------------------------------------------------------------

static void startPromisc();

void setup() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(8192);  // a whole status burst fits while the page is reading
#endif
  Serial.begin(115200);
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(5);      // never sit and wait on USB (see OutTee::write)
#endif
  delay(200);
  bootWhy = readResetWhy();
  if (bootWhy != RW_NO_MASTER || rtcNoMasterRuns > 100) rtcNoMasterRuns = 0;
  prefs.begin("skysentinel", false);
  pinMode(pairing::bootPin(), INPUT_PULLUP);
  const uint8_t defaultRole = SS_HW_TYPE == HW_MARAUDER_V8 ? RUN_MASTER
                              : (SS_HW_TYPE == HW_HELTEC_TRACKER || SS_HW_TYPE == HW_TEMBED_CC1101) ? RUN_NODE : RUN_STANDALONE;
  runRole = (RunRole)(prefs.isKey("role") ? prefs.getUChar("role", defaultRole) : defaultRole);
  if (runRole > RUN_NODE) runRole = RUN_STANDALONE;
  setupDone = prefs.isKey("setup") && prefs.getBool("setup", false);
  clusterKey = prefs.isKey("key") ? prefs.getString("key", "") : String("");
  // A master always has a key (made here the first time): nobody joins without pairing.
  if (runRole == RUN_MASTER && !clusterKey.length()) {
    uint8_t rnd[16];
    pairing::randomBytes(rnd, sizeof(rnd));
    char k[33];
    clusterKeyFromRandom(rnd, k);
    clusterKey = k;
    prefs.putString("key", clusterKey);
    Serial.println("[pair] made a new cluster key; pair nodes with ADD NODE");
  }
  unpaired = runRole == RUN_NODE && !clusterKey.length();
  corePinned = prefs.isKey("core") && prefs.getBytes("core", pinnedCore, 6) == 6;
  userCats = prefs.isKey("cats") ? prefs.getUShort("cats", kAllCategoriesMask) : kAllCategoriesMask;
  if (prefs.getUChar("catv", 1) < 2) {   // categories added since this board saved its choice start on
    userCats |= catBit(CAT_VEHICLE);
    prefs.putUChar("catv", 2);
    if (prefs.isKey("cats")) prefs.putUShort("cats", userCats);
  }
  assignMode = (AssignMode)(prefs.isKey("mode") ? prefs.getUChar("mode", (uint8_t)ASSIGN_SPLIT) : (uint8_t)ASSIGN_SPLIT);
  localScan = prefs.isKey("local") ? prefs.getBool("local", true) : true;
  activeScan = prefs.isKey("ascan") && prefs.getBool("ascan", false);
  nodeScreens = prefs.isKey("nodescr") ? prefs.getBool("nodescr", true) : true;
  linkLr = prefs.getBool("linklr", false);
  espLink.setLrFast(prefs.getBool("lrfast", true));   // 500 kbit/s unless chosen otherwise (see README, Long Range)
  localScreen = prefs.isKey("screen") ? prefs.getBool("screen", true) : true;
  {
    // Default: the V8 (battery, GPS) is usually carried; dev boards usually sit still.
    const uint8_t defDeploy = SS_HW_TYPE == HW_MARAUDER_V8 ? DEPLOY_MOBILE : DEPLOY_STATIONARY;
    follow.setDeployment(prefs.isKey("deploy") ? prefs.getUChar("deploy", defDeploy) : defDeploy);
  }
  apOn = prefs.isKey("ap") && prefs.getBool("ap", false);
  apPass = prefs.isKey("appass") ? prefs.getString("appass", "") : String("");

  // A master's own radio stays on one channel (fewer hits), and it needs the memory more
  hitQueue = xQueueCreate(runRole == RUN_MASTER ? kQueueDepth / 2 : kQueueDepth, sizeof(RxItem));

  // Free memory after each big piece starts, so a board that runs short can say where it went
  auto bootHeap = [](const char* what) {
    char b[48];
    snprintf(b, sizeof(b), "%s%s %lu", bootMemLog.length() ? ", " : "", what, (unsigned long)(ESP.getFreeHeap() / 1024));
    bootMemLog += b;
    Out.printf("[boot] after %s: free %lu, largest block %lu\n", what, (unsigned long)ESP.getFreeHeap(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  };
  psramRescue();   // before anything allocates: the device table goes there when it works
  bootHeap("start");
  if (!WiFi.mode(WIFI_STA)) Out.println("[wifi] Wi-Fi FAILED to start");
  WiFi.disconnect();
  bootHeap("wifi");
#if SOC_WIFI_SUPPORT_5G
  esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);
  myId = nodeIdFromMac(myMac);
  {
    char def[24];
    snprintf(def, sizeof(def), "SkySentinel-%04X", myId);
    apName = prefs.isKey("apname") ? prefs.getString("apname", def) : String(def);
  }
  apActive = apOn && runRole != RUN_NODE;
  if (apActive) {
    WiFi.mode(WIFI_AP_STA);  // AP for the phone; the STA side stays idle (ESP-NOW rides on it)
    if (portal::begin(apName.c_str(), apPass.c_str(), kHomeChannel, [](const String& l) { handleLine(l, millis()); })) {
      Out.printf("[wifi] access point \"%s\" (%s) on channel %u, page at http://%s/\n", apName.c_str(),
                 apPass.length() ? "password" : "open", kHomeChannel, WiFi.softAPIP().toString().c_str());
    } else {
      Out.println("[wifi] access point FAILED to start");
      apActive = false;
    }
  }

  if (runRole == RUN_STANDALONE) {
    esp_wifi_set_ps(WIFI_PS_NONE);
    setRadioChannel(kHomeChannel);
  } else {
    if (!espLink.begin(clusterKey.c_str(), kHomeChannel)) Out.println("[link] ESP-NOW init FAILED");
    bootHeap("esp-now");
    if (runRole == RUN_NODE && corePinned && !unpaired) espLink.addPeer(pinnedCore);
    masterPair.begin(&espLink, myId);
    if (unpaired) {
      uint8_t caps = CAP_WIFI24 | CAP_BLE;
#if SOC_WIFI_SUPPORT_5G
      caps |= CAP_WIFI5;
#endif
      nodePair.begin(&espLink, myMac, myId, SS_HW_TYPE, caps, SS_FW_VERSION);
    }
  }

  // Management frames (beacons, probes) plus data frames: devices already connected to
  // a network rarely probe, so their traffic is the only way to hear them.
  startPromisc();

  bootHeap("access point + promiscuous");
  NimBLEDevice::init("");
  bootHeap("bluetooth");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // want duplicates: every advert counts as a hit
  scan->setActiveScan(activeScan);               // passive by default: we never transmit
  scan->setMaxResults(0);                        // don't store results
  scan->setDuplicateFilter(false);

  if (hasTable()) {
    table = createDeviceTable();
    table->addClusterMac(myMac);
    if (runRole == RUN_MASTER) g_dashAlerts = &dashAlerts;
    logger::begin(&prefs);
    portal::setLogWriter([](Print& o) { logger::streamCsv(o); });
    if (runRole == RUN_MASTER) logger::setSink(&sendLogRecord);
    bootHeap("device table + log");
    if (apActive) {  // our own AP, if a node hears it, is not a device to report
      uint8_t apMac[6];
      esp_read_mac(apMac, ESP_MAC_WIFI_SOFTAP);
      table->addClusterMac(apMac);
    }
    if (runRole == RUN_MASTER) nodes.reserve(kLocalSlot);
    applyLocalPlan();
  } else {
    nodeDefaultConfig();
  }

#ifdef SS_HAS_DISPLAY
  display::begin();
  if (runRole != RUN_NODE) display::setOn(localScreen);
  battPct = readBattery();
#endif
#ifdef SS_GPS
  gpsBegin();
#endif
#ifdef SS_SUBGHZ
  Out.printf("[subghz] CC1101 band watch %s\n", subghz::begin(&catMask) ? "ready (315 / 345 / 390 / 433.92 / 915 MHz, receive only)" : "FAILED (radio not found)");
#endif
#ifdef SS_LORA
  Out.printf("[lora] 915 MHz listener %s\n", lora::begin(&loraSink, &catMask) ? "ready (Meshtastic + LoRaWAN, receive only)" : "FAILED (radio not found)");
#endif
  char s[18];
  formatMac(myMac, s);
  Out.printf("[%s] SkySentinel %s (id %04X) fw %u, key %s%s%s\n", runRoleName(runRole), s, myId, SS_FW_VERSION,
                clusterKey.length() ? "set" : "none", setupDone ? "" : ", setup not done",
                runRole == RUN_NODE && corePinned ? ", master pinned" : "");
  Out.printf("[%s] started: %s, free heap %lu KB\n", runRoleName(runRole), resetWhyName(bootWhy),
             (unsigned long)(ESP.getFreeHeap() / 1024));
  if (table) Out.printf("[%s] table %u devices, cats 0x%04X\n", runRoleName(runRole), (unsigned)table->capacity(), userCats);
}

static void drainHits(uint32_t now) {
  RxItem it;
  while (xQueueReceive(hitQueue, &it, 0) == pdTRUE) {
    if (!it.note_only) batch.add(it.s, now);
    if ((it.name_len || it.has_ap) && nPendingNames < 8) {
      bool send;
      if (it.what == NAME_POS || it.what == NAME_RID_MOVE || it.what == NAME_RID_OP) {
        // Live values (a drone's position changes every second): every 2 s per device and kind.
        uint8_t key[6];
        memcpy(key, it.s.addr, 6);
        key[0] ^= (uint8_t)(it.what << 4);
        send = liveNames.shouldSend(key, now);
      } else if (it.what != NAME_DEVICE || it.s.kind == KIND_LORA_MESHTASTIC) {
        // Once per (device, text) every few minutes: key on a hash of both. (Phones ask
        // for many networks; a Meshtastic node has an id, a name and a position.)
        uint32_t h = 2166136261u;
        for (int i = 0; i < 6; ++i) h = (h ^ it.s.addr[i]) * 16777619u;
        for (uint8_t i = 0; i < it.name_len; ++i) h = (h ^ (uint8_t)it.name[i]) * 16777619u;
        const uint8_t key[6] = {it.s.addr[0], it.s.addr[1], (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24)};
        send = probeNames.shouldSend(key, now);
      } else {
        send = names.shouldSend(it.s.addr, now);
      }
      if (send) {
        Name& nm = pendingNames[nPendingNames++];
        memcpy(nm.addr, it.s.addr, 6);
        nm.kind = it.s.kind;
        nm.len = it.name_len;
        memcpy(nm.name, it.name, it.name_len + 1);
        nm.what = it.what;
        nm.has_ap = it.has_ap;
        if (it.has_ap) nm.ap = it.ap;
      }
    }
  }
}

// Management frames (beacons, probes) plus data frames (see onWifiData)
static void startPromisc() {
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPromisc);
  esp_wifi_set_promiscuous(true);
}

// Frames to the master that never finish: the Wi-Fi driver drops them without a callback
// (esp-idf issue #19269; Long Range makes it far more likely). Each leaks a transmit
// buffer, and after a few every send is refused. Restarting ESP-NOW or even the whole
// Wi-Fi driver doesn't recover it cleanly (tested: the refusals continue, and memory
// leaks), so restart the board: back in the cluster within a few seconds.
static constexpr uint32_t kTxStuckMs = 1500;
static void checkStuckTx(uint32_t now) {
  static uint32_t since = 0;
  if (!espLink.inFlight()) { since = 0; return; }
  if (!since) { since = now ? now : 1; return; }
  if (now - since < kTxStuckMs) return;
  Out.printf("[link] %lu frames stuck in the Wi-Fi driver for %lu ms (Long Range %s, heap %lu KB), restarting\n",
             (unsigned long)espLink.inFlight(), (unsigned long)(now - since),
             paired && espLink.peerLongRange(coreMac) ? "on" : "off", (unsigned long)(ESP.getFreeHeap() / 1024));
  rtcRestartTag = kSelfRestartTag;
  Serial.flush();
  delay(100);
  ESP.restart();
}

static void hop(uint32_t now) {
  const uint64_t chans = roleWantsWifi() ? cfg.channel_mask : 0;
  // A node doesn't leave the master's channel while frames to it are still going out
  // (leaving with Long Range frames waiting is the suspect for frames that never finish).
  // checkStuckTx() clears the link if they truly hang.
  const bool sending = runRole == RUN_NODE && espLink.inFlight();
  if (chans && !sending && (int32_t)(now - lastHop) >= (int32_t)cfg.dwell_ms) {
    const uint8_t ch = nextChannel(chans, &hopCursor);
    if (ch && ch != curChannel && setRadioChannel(ch)) curChannel = ch;
    lastHop = now;
  } else if (!chans) {
    goHome();
  }
}

// One radio does Wi-Fi and Bluetooth by taking turns, and while BLE scans the node
// can't hear ESP-NOW. Around each exchange with the master: stop BLE, send, listen
// for the reply, then scan again.
static void talkToMaster(void (*send)(), uint32_t listen_ms = kListenWindowMs) {
  const bool scanning = roleWantsBle() && cfg.ble_duty_pct > 0 && !queryBusy;
  if (scanning) { bleHeld = true; bleStop(); }
  send();
  const uint32_t t0 = millis();
  RxPacket p;
  while (millis() - t0 < listen_ms) {
    while (espLink.poll(p)) nodeHandlePacket(p);
    delay(2);
  }
  // Still looking for the master: keep Bluetooth off, so a late WELCOME isn't missed
  if (scanning) { bleHeld = !paired; if (paired) applyBleDuty(); }
}

static void flushAndBeat() {
  const uint32_t now = millis();
  // Until the master's CONFIG arrives, check in every second (each heartbeat gets a
  // CONFIG back), then every 5 s. Heartbeat first, so a busy batch can't crowd it out.
  const uint32_t every = cfg.cfg_version ? kHeartbeatEveryMs : kUnconfiguredBeatMs;
  if (now - lastHeartbeat >= every) { lastHeartbeat = now; sendHeartbeat(); }
  flushReports(now);
}

static void loopNode(uint32_t now) {
  checkStuckTx(now);
  RxPacket p;
  while (espLink.poll(p)) nodeHandlePacket(p);

  if (unpaired) {
    // No cluster key: ask to join every 1.5 s and listen for a master's offer.
    if (nodePair.done()) {
      prefs.putString("key", nodePair.key());
      prefs.putBytes("core", nodePair.master(), 6);
      Out.println("[pair] key saved, restarting into the cluster");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
    goHome();
    static uint32_t lastAsk = 0;
    if (now - lastAsk >= 1500) { lastAsk = now; talkToMaster(&pairRequestNow, 300); }
    batch.clear();
    return;
  }

  // The radio can't send at all (every frame refused before it leaves): waiting out the
  // 20 s and the 60 s after it only stretches the outage. A restart clears it in ~2 s.
  if (espLink.failStreak() >= 12) {
    Out.printf("[node] the radio stopped sending (%lu failures in a row, last %s, in flight %lu, heap %lu KB), restarting\n",
               (unsigned long)espLink.failStreak(), esp_err_to_name(espLink.lastSendError()),
               (unsigned long)espLink.inFlight(), (unsigned long)(ESP.getFreeHeap() / 1024));
    rtcRestartTag = kSelfRestartTag;
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  // Signed: a packet handled during this pass can set lastCoreContact a little after `now`.
  if (paired && (int32_t)(now - lastCoreContact) > (int32_t)kCoreLostMs) {
    ++linkLosses;
    lostSince = now;
    uint8_t realCh = 0;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&realCh, &sc);
    Out.printf("[node] lost master after %lus up (link %d dBm, ch %u (radio on %u), heap %lu KB, link frames in %lu, Wi-Fi frames %lu, not delivered %lu, send failures %lu (%lu in a row, last %s), in flight %lu), re-pairing\n",
               (unsigned long)(now / 1000), masterRssi, curChannel, realCh, (unsigned long)(ESP.getFreeHeap() / 1024),
               (unsigned long)espLink.rxCount(), (unsigned long)wifiFrames, (unsigned long)espLink.notDelivered(),
               (unsigned long)espLink.sendFailures(), (unsigned long)espLink.failStreak(),
               espLink.lastSendError() ? esp_err_to_name(espLink.lastSendError()) : "none", (unsigned long)espLink.inFlight());
    paired = false;
    nodeDefaultConfig();
  }
  if (!paired) {
    if (!lostSince) lostSince = now;
    // Bluetooth scanning shares the radio: while looking for the master, listen only
    // for it, so a WELCOME that comes a little late still lands.
    if (!bleHeld) { bleHeld = true; bleStop(); }
    // Alone too long: something in the radio stack is stuck. A restart has always
    // brought it back, so do it ourselves (reported as "no_master" afterwards).
    {  // while looking: is the radio hearing anything at all? (once a minute)
      static uint32_t lastLook = 0, rx0 = 0, wf0 = 0, na0 = 0;
      if (now - lostSince < 1000) { lastLook = now; rx0 = espLink.rxCount(); wf0 = wifiFrames; na0 = espLink.notDelivered(); }
      else if (now - lastLook >= 60000) {
        uint8_t realCh = 0;
        wifi_second_chan_t sc;
        esp_wifi_get_channel(&realCh, &sc);
        Out.printf("[node] still looking for the master: last minute, link frames in %lu, Wi-Fi frames %lu, hellos not delivered %lu, radio on ch %u\n",
                   (unsigned long)(espLink.rxCount() - rx0), (unsigned long)(wifiFrames - wf0),
                   (unsigned long)(espLink.notDelivered() - na0), realCh);
        lastLook = now; rx0 = espLink.rxCount(); wf0 = wifiFrames; na0 = espLink.notDelivered();
      }
    }
    const uint32_t giveUp = rtcNoMasterRuns < kNoMasterQuickTries ? kNoMasterQuickMs : kNoMasterRebootMs;
    if (now - lostSince > giveUp) {
      ++rtcNoMasterRuns;
      Out.printf("[node] no master for %lu s, restarting (try %lu)\n", (unsigned long)(giveUp / 1000), (unsigned long)rtcNoMasterRuns);
      rtcRestartTag = kSelfRestartTag;
      Serial.flush();
      delay(100);
      ESP.restart();
    }
    // Make sure the radio really is on the home channel (not just our idea of it).
    static uint32_t lastForce = 0;
    if (now - lastForce > 10000) { lastForce = now; setRadioChannel(kHomeChannel); curChannel = kHomeChannel; }
    goHome();
    if (now - lastHello >= kHelloEveryMs) { lastHello = now; talkToMaster(&sendHello, kHelloListenMs); }
    batch.clear();  // nothing to report to yet
    return;
  }
  if (bleHeld && !queryBusy) { bleHeld = false; applyBleDuty(); }   // found the master again: scan as planned
  if (now - lastReport >= cfg.report_ms || batch.full()) {
    goHome();
    const uint32_t every = cfg.cfg_version ? kHeartbeatEveryMs : kUnconfiguredBeatMs;
    talkToMaster(&flushAndBeat, now - lastHeartbeat >= every ? kBeatListenMs : kListenWindowMs);
    lastReport = now;
    lastHop = millis();
    return;
  }
  hop(now);
}

static void loopTable(uint32_t now) {
  phaseMark = millis();
  if (runRole == RUN_MASTER) {
    RxPacket p;
    for (int i = 0; i < 16 && espLink.poll(p); ++i) masterHandlePacket(p, now);
    masterPair.poll(now, clusterKey.c_str());
    bool wasOnline[kMaxNodes];
    for (int i = 0; i < kMaxNodes; ++i) wasOnline[i] = nodes.at(i).used && nodes.at(i).online;
    if (nodes.expire(now, kNodeTimeoutMs)) {
      needAssign = true;
      for (int i = 0; i < kMaxNodes; ++i) {
        const NodeInfo& n = nodes.at(i);
        if (!wasOnline[i] || n.online) continue;
        char t[kDashAlertText + 1];
        snprintf(t, sizeof(t), "Node %04X stopped answering", n.id);
        dashAlerts.note(1, CAT_NONE, t, now);
        Out.printf("[master] node %04X stopped answering (up %lus, link %d dBm, heap %u KB)\n", n.id,
                   (unsigned long)n.hb.uptime_s, n.link_rssi, n.hb.heap_kb);
        if (g_jsonOn) Out.printf("{\"t\":\"node\",\"event\":\"lost\",\"id\":\"%04X\",\"up\":%lu}\n", n.id,
                                 (unsigned long)n.hb.uptime_s);
      }
    }
    if (needAssign) reassign();
    xferTick(now);
  }
  phaseEnd(LP_RADIO);
  if (now - lastReport >= cfg.report_ms || batch.full()) {
    flushReports(now);
    lastReport = now;
  }
  phaseEnd(LP_REPORT);
  if (now - lastMaintain >= 1000) {
    lastMaintain = now;
    table->maintain(now);
    follow.update(*table, now);
#ifdef SS_GPS
    {  // this board's own GPS (a Heltec as the master)
      double la, lo;
      uint32_t t;
      if (gps.state(now) >= GPS_FIX_2D && gps.position(la, lo)) logger::setPosition((int32_t)(la * 1e7), (int32_t)(lo * 1e7), now);
      if (gps.utc(now, t)) logger::setClock(t, now);
    }
#endif
    FollowAlert fa;
    while (follow.poll(fa)) emitFollow(*table, fa);
  }
  pollSpamEvents(*table);
  phaseEnd(LP_MAINTAIN);
  logger::poll(now);
  phaseEnd(LP_LOG);
  if (runRole == RUN_STANDALONE) hop(now);  // a master never leaves the home channel
}

void loop() {
  const uint32_t now = millis();
#ifdef SS_DISPLAY_TEMBED
  display::tick(now);   // jog wheel and buttons: page flips feel instant
#endif
#if !defined(SS_HAS_DISPLAY) && defined(RGB_BUILTIN)
  // A board without a screen talks through its RGB LED:
  //   fast white  identify, or the master's IDENTIFY while pairing ("this one")
  //   slow blue   unpaired: waiting to be paired
  //   slow green  master: pairing open (ADD NODE)
  {
    static uint32_t lastColor = 0xFFFFFFFF;
    uint32_t color = 0;
    if ((int32_t)(identifyUntil - now) > 0 || (unpaired && nodePair.blinking(now))) color = (now / 125) % 2 ? 0xFFFFFF : 0;
    else if (unpaired) color = (now / 600) % 2 ? 0x000040 : 0;
    else if (runRole == RUN_MASTER && masterPair.active(now)) color = (now / 600) % 2 ? 0x004000 : 0;
    if (color != lastColor) {
      lastColor = color;
      rgbLedWrite(RGB_BUILTIN, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
  }
#endif
  // BOOT held 5 s: a node forgets its master and waits to be paired again; a master
  // opens pairing (ADD NODE without the page).
  if (runRole != RUN_STANDALONE) {
    static uint32_t downSince = 0;
    static bool fired = false;
    if (digitalRead(pairing::bootPin()) == LOW) {
      if (!downSince) downSince = now ? now : 1;
      if (!fired && now - downSince > 5000) {
        fired = true;
        if (runRole == RUN_NODE && !unpaired) unpairAndRestart();
        else if (runRole == RUN_MASTER) masterPair.start(now);
      }
    } else {
      downSince = 0;
      fired = false;
    }
  }
  uint32_t t0 = millis();
  handleSerial(now);
#ifdef SS_GPS
  gpsPoll(now);
#endif
  phaseMs[LP_SERIAL] += millis() - t0; t0 = millis();
  if (apActive) portal::loop();
  phaseMs[LP_WEB] += millis() - t0; t0 = millis();
  drainHits(now);
  phaseMs[LP_HITS] += millis() - t0;
  if (runRole == RUN_NODE) loopNode(now);
  else loopTable(now);
  if (queryDone) {
    queryDone = false;
    Out.println(queryJson);
    if (queryName.len && table) table->setName(queryName);
  }
  if (inspectReady && now - inspectLastMs >= 250) {
    InspectBuf h;
    portENTER_CRITICAL(&inspectMux);
    h = inspectHit;
    inspectReady = false;
    portEXIT_CRITICAL(&inspectMux);
    inspectLastMs = now;
    Out.print("{\"t\":\"adv\",\"m\":");
    jsonMac(h.mac);
    Out.printf(",\"r\":%d,\"rnd\":%s,\"cn\":%s,\"p\":\"", h.rssi, h.random ? "true" : "false", h.connectable ? "true" : "false");
    for (uint8_t i = 0; i < h.len; ++i) Out.printf("%02X", h.data[i]);
    Out.println("\"}");
  }
  if (now - lastStatus >= kStatusEveryMs) {
    lastStatus = now;
#ifdef SS_HAS_DISPLAY
    static uint32_t lastBatt = 0;
    if (now - lastBatt >= 30000) { lastBatt = now; battPct = readBattery(); }
    {
      display::Info di;
      di.role = runRole == RUN_NODE ? "NODE" : runRole == RUN_MASTER ? "MASTER" : "STANDALONE";
      di.paired = paired;
      di.master_id = paired ? nodeIdFromMac(coreMac) : 0;
      di.link_rssi = masterRssi;
      di.configured = cfg.cfg_version != 0;
      di.ble_duty = roleWantsBle() ? cfg.ble_duty_pct : 0;
      di.wifi_channels = roleWantsWifi() ? channelCount(cfg.channel_mask) : 0;
#ifdef SS_LORA
      const lora::Stats ls = lora::stats();
      di.lora = lora::ready() && (catMask & catBit(CAT_LORA));
      di.lora_on = ls.listening;
      di.lora_packets = ls.packets;
#endif
      di.sent = runRole == RUN_NODE ? sightingsSent : (table ? table->countNowAll(now) : 0);
      di.session = table ? table->totalAll() : 0;
      di.nodes_online = nodes.onlineCount();
      di.battery_pct = battPct <= 100 ? battPct : 255;
      di.usb = battPct == 254;
      di.gps = gpsState(now);
#ifdef SS_GPS
      di.gps_sats = gps.sats();
#endif
      di.uptime_s = now / 1000;
      di.label = nodeLabel;
      di.identify = (int32_t)(identifyUntil - now) > 0 || (unpaired && nodePair.blinking(now));
      if (unpaired) {
        di.pair = nodePair.hasOffer(now) ? 2 : 1;
        di.pair_code = nodePair.code();
        di.pair_master = nodePair.offerFrom();
      } else if (runRole == RUN_MASTER && masterPair.active(now)) {
        di.pair = 3;
      }
      display::update(di);
    }
#endif
    const uint32_t o0 = millis();
    emitStatus(now);
    if (table) emitDeviceList(*table, now);
    phaseMs[LP_OUTPUT] += millis() - o0;
  }
  {  // the memory report once, 30 s after starting (the logger catches it; boot lines come too early)
    static bool memShown = false;
    if (!memShown && now > 30000) { memShown = true; memReport(); }
  }
  // Master or standalone: the radio always hears something on channel 6 (beacons, the
  // nodes). Nothing at all for 90 s means Wi-Fi didn't start properly (seen once right
  // after flashing, with memory tight). A restart has fixed it, so do that.
  if (runRole != RUN_NODE) {
    static uint32_t lastFrames = 0, lastChange = 0;
    if (wifiFrames != lastFrames || !lastChange) { lastFrames = wifiFrames; lastChange = now ? now : 1; }
    else if (now - lastChange > kRadioDeadMs) {
      Out.printf("[wifi] radio heard nothing for %lu s (free memory %lu), restarting\n", (unsigned long)(kRadioDeadMs / 1000),
                 (unsigned long)ESP.getFreeHeap());
      rtcRestartTag = kRadioRestartTag;
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  }
  if (runRole == RUN_MASTER) {
    const uint32_t pass = millis() - now;
    if (pass > loopMaxMs) loopMaxMs = pass;
    if (pass > kSlowPassMs) {
      ++slowPasses;
      static uint32_t lastSlowLog = 0;
      if (millis() - lastSlowLog > 5000) {
        lastSlowLog = millis();
        char b[160];
        size_t o = snprintf(b, sizeof(b), "[master] slow pass %lu ms:", (unsigned long)pass);
        for (uint8_t i = 0; i < LP_COUNT && o < sizeof(b) - 20; ++i)
          if (phaseMs[i]) o += snprintf(b + o, sizeof(b) - o, " %s %lu", kPhaseNames[i], (unsigned long)phaseMs[i]);
        Out.println(b);
      }
    }
  }
  memset(phaseMs, 0, sizeof(phaseMs));
  delay(1);
}
