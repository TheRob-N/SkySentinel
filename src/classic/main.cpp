// SkySentinel Classic node: an original ESP32 (WROOM-32 / -32E, e.g. ESP32-DevKitC or
// the CYD) added to a cluster to see Classic Bluetooth, which the C5's BLE-only
// radio can't. Always a node: pairs with the master over ESP-NOW like a C5 node.
//
// What it does:
//   Classic   repeated inquiry scans (~13 s each). Finds devices that are
//             discoverable: in pairing mode, or set to always visible (cars, many
//             speakers, older headsets). Reports address, name, Class of Device
//             (headphones, phone, car kit...) and signal. HC-05/HC-06 serial modules,
//             the usual gas-pump skimmer radio, are flagged as skimmers.
//             Inquiry transmits (that's how Classic discovery works); the board
//             itself stays non-discoverable.
//   Wi-Fi     2.4 GHz promiscuous scanning on the channels the master assigns.
//   BLE       none here: the C5s cover BLE, and this radio is busy with the other two.
//
// It can't see Classic devices that are already paired and streaming (a connected
// headset doesn't answer inquiries). That takes a passive sniffer (Ubertooth).
//
// With -DSS_CYD (env:cyd) the same node runs on a CYD and shows the master's
// dashboard on its touch screen (src/common/cyd_ui.*).
//
// Serial (115200): key <text>|-  core <mac>|-  status  reboot  factory  json on|off
//                  CYD only: cyd setup | cyd driver auto|ili9341|st7789 | cyd invert on|off|auto
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include "core_common.h"
#include "espnow_link.h"
#include "pairing.h"
#include "ss_batch.h"
#include "ss_channels.h"
#include "ss_classify.h"
#include "ss_nodes.h"
#include "ss_proto.h"
#ifdef SS_CYD
#include "cyd_gps.h"
#include "cyd_sd.h"
#include "cyd_ui.h"
#include "ss_log.h"
#define SS_NODE_HW HW_CYD
#else
#define SS_NODE_HW HW_ESP32_DEVKIT
#endif

using namespace ss;

// Arduino-ESP32 hands the Bluetooth controller's memory back at startup unless the
// program says it uses Bluetooth (its weak default returns false). Classic inquiry needs it.
extern "C" bool btInUse() { return true; }

#ifndef SS_FW_VERSION
#define SS_FW_VERSION 0x0001
#endif

static constexpr uint32_t kHelloEveryMs = 1000;
static constexpr uint32_t kHeartbeatEveryMs = 5000;
static constexpr uint32_t kCoreLostMs = 20000;
#ifdef SS_CYD
// The dashboard refreshes with every CONFIG, so check in every second and wait a bit
// longer for the answer.
static constexpr uint32_t kListenWindowMs = 150;
static constexpr uint32_t kDashBeatMs = 1000;
#else
static constexpr uint32_t kListenWindowMs = 60;
#endif
static constexpr uint32_t kUnconfiguredBeatMs = 1000;  // check in this often until the master's CONFIG lands
static constexpr uint32_t kStatusEveryMs = 1000;
static constexpr uint8_t kInquiryLen = 10;          // x 1.28 s = 12.8 s per inquiry
static constexpr uint32_t kInquiryGapMs = 2000;     // breather for Wi-Fi / ESP-NOW between inquiries
#ifdef SS_CYD
static constexpr size_t kQueueDepth = 64;   // the CYD's screen, SD card and Classic Bluetooth leave little RAM
#else
static constexpr size_t kQueueDepth = 96;
#endif
// Categories a Classic device can land in; inquiry runs if any is enabled.
static constexpr uint16_t kClassicCats = (1u << CAT_AUDIO) | (1u << CAT_PHONE) | (1u << CAT_WEARABLE) |
                                         (1u << CAT_OTHER_BLE) | (1u << CAT_SKIMMER) | (1u << CAT_VEHICLE);

struct RxItem {
  Sighting s;
  uint8_t name_len;
  char name[kMaxName + 1];
  bool has_ap;         // access point details (beacons, probe responses); no WPS text:
  uint8_t sec, gen, width, util;   // this board is short of RAM
  uint16_t sta;
  uint32_t up_s;
};

static EspNowLink espLink;
static Preferences prefs;
static QueueHandle_t hitQueue;
static SightingBatch<96> batch;
static NameTracker<64> names;
static FloodMonitor flood;
static portMUX_TYPE floodMux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t myMac[6];
static uint16_t myId;
static String clusterKey;
static pairing::NodePair nodePair;   // no cluster key yet: asking to join
static bool unpaired = false;
static uint8_t pinnedCore[6];
static bool corePinned = false;
static bool paired = false;
static uint8_t coreMac[6];
static uint32_t lastCoreContact = 0;
static uint8_t nodeSlot = 0xFF;

static Config cfg;
static volatile uint16_t catMask = 0;
static volatile int8_t rssiFloor = -95;
static volatile uint8_t curChannel = kHomeChannel;
static uint8_t hopCursor = kChannelCount - 1;
static volatile uint32_t wifiFrames = 0, classicFound = 0, queueDrops = 0;
static uint32_t lastHop = 0, lastReport = 0, lastHeartbeat = 0, lastHello = 0, lastStatus = 0;

static bool btReady = false;
static volatile bool inquiring = false;
static uint32_t nextInquiry = 0;
static uint32_t inquiries = 0;

static uint32_t identifyUntil = 0;    // master's "which one is this?"
static uint32_t fileBusyUntil = 0;    // a remote SD download is running: don't hop channels
static int8_t linkRssi = -127;        // how well we hear the master

static Name pendingNames[8];
static uint8_t nPendingNames = 0;

// Wi-Fi's receive buffers come from the heap, up to 32 of them (about 1.6 KB each) when a
// burst of frames arrives. On this board Classic Bluetooth, the screen and the SD card
// leave ~16 KB free, so a burst could starve the Bluetooth stack mid-allocation and crash
// it (seen once after ~9 h: memset on a NULL from Bluedroid). Arduino's WiFi sets these
// numbers itself, so the link step routes esp_wifi_init through here (-Wl,--wrap) and
// caps them: a burst now drops frames instead of taking the board down.
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  wifi_init_config_t c = *config;
  // The weekend's log: never under 8 KB free at the once-a-minute samples, yet it crashed,
  // so the squeeze is a sudden burst of more than 8 KB. 4 dynamic buffers caps a burst at
  // ~6.4 KB; 2 static (the minimum) frees ~3 KB for good. Frames over that are dropped.
  c.static_rx_buf_num = 2;
  c.dynamic_rx_buf_num = 4;
  c.dynamic_tx_buf_num = 8;
  c.ampdu_rx_enable = 0;
  c.ampdu_tx_enable = 0;
  return __real_esp_wifi_init(&c);
}

static bool wantsWifi() { return cfg.role == ROLE_WIFI || cfg.role == ROLE_BOTH; }

static void enqueue(const Sighting& s, const char* name, uint8_t name_len, const ApMeta* ap = nullptr) {
  RxItem it;
  it.s = s;
  it.name_len = name_len;
  it.has_ap = ap != nullptr;
  if (ap) { it.sec = ap->sec; it.gen = ap->gen; it.width = ap->width; it.util = ap->util; it.sta = ap->sta; it.up_s = ap->up_s; }
  if (name_len) memcpy(it.name, name, name_len);
  it.name[name_len] = '\0';
  if (xQueueSend(hitQueue, &it, 0) != pdTRUE) queueDrops = queueDrops + 1;
}

// ---------------------------------------------------------------------------
// Wi-Fi (same as a C5 node, 2.4 GHz only)

static void onPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  wifiFrames = wifiFrames + 1;
  int len = pkt->rx_ctrl.sig_len - 4;
  if (len < 24) return;
  const int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
  WifiFrame f;
  if (!parseWifiFrame(pkt->payload, (size_t)len, f)) return;
  const uint8_t ch = curChannel;
  if (f.kind == KIND_WIFI_OTHER) {
    if (catMask & catBit(CAT_PENTEST)) {
      portENTER_CRITICAL(&floodMux);
      flood.onFrame(f, ch, millis());
      portEXIT_CRITICAL(&floodMux);
    }
    return;
  }
  if (!wantsWifi() || rssi < rssiFloor || macIsMulticast(f.addr2)) return;
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
    if (f.tsf && f.tsf < 157680000ull * 1000000ull) ap.up_s = (uint32_t)(f.tsf / 1000000ull);
    enqueue(s, hasName ? f.ssid : nullptr, hasName ? f.ssid_len : 0, &ap);
  } else {   // (networks phones look for: the other boards send those; no RAM here)
    enqueue(s, nullptr, 0);
  }
}

// ---------------------------------------------------------------------------
// Classic Bluetooth inquiry (Bluedroid GAP)

static void onGap(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  if (event == ESP_BT_GAP_DISC_RES_EVT) {
    uint32_t cod = 0;
    int8_t rssi = -127;
    char name[kMaxName + 1] = {0};
    uint8_t nameLen = 0;
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
      const esp_bt_gap_dev_prop_t& p = param->disc_res.prop[i];
      if (p.type == ESP_BT_GAP_DEV_PROP_COD && p.len >= 4) {
        cod = *(uint32_t*)p.val;
      } else if (p.type == ESP_BT_GAP_DEV_PROP_RSSI) {
        rssi = *(int8_t*)p.val;
      } else if (p.type == ESP_BT_GAP_DEV_PROP_BDNAME && !nameLen && p.len > 0) {
        nameLen = (uint8_t)(p.len > (int)kMaxName ? kMaxName : p.len);
        memcpy(name, p.val, nameLen);
      } else if (p.type == ESP_BT_GAP_DEV_PROP_EIR && !nameLen) {
        uint8_t l = 0;
        uint8_t* n = esp_bt_gap_resolve_eir_data((uint8_t*)p.val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &l);
        if (!n) n = esp_bt_gap_resolve_eir_data((uint8_t*)p.val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &l);
        if (n && l) {
          nameLen = (uint8_t)(l > kMaxName ? kMaxName : l);
          memcpy(name, n, nameLen);
        }
      }
    }
    name[nameLen] = '\0';
    classicFound = classicFound + 1;
    if (rssi < rssiFloor && rssi != -127) return;
    const Match m = classifyClassic(cod, name);
    if (!(catMask & catBit(m.category))) return;
    enqueue(makeClassicSighting(param->disc_res.bda, m, rssi), name, nameLen);
  } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
    if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
      inquiring = false;
    } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
      inquiring = true;
    }
  }
}

static bool startClassic() {
  if (!btStartMode(BT_MODE_CLASSIC_BT)) return false;  // controller in Classic-only mode
  if (esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) return false;
  esp_bt_gap_register_callback(onGap);
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);  // we look, we don't show
  return true;
}

static void pollInquiry(uint32_t now) {
  if (!btReady || inquiring) return;
  if (!(catMask & kClassicCats)) return;
  if ((int32_t)(now - nextInquiry) < 0) return;
  if ((int32_t)(fileBusyUntil - now) > 0) return;   // a download has the radio
  if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, kInquiryLen, 0) == ESP_OK) {
    inquiring = true;
    ++inquiries;
  }
  nextInquiry = now + kInquiryLen * 1280 + kInquiryGapMs;
}

// ---------------------------------------------------------------------------
// Link to the master (same protocol as the C5 nodes)

static void applyConfig(const Config& c) {
  cfg = c;
  catMask = c.category_mask;
  rssiFloor = c.rssi_floor;
  Out.printf("[classic] config v%u wifi=%s channels=%u cats=0x%04X classic=%s\n", c.cfg_version,
             wantsWifi() ? "yes" : "no", channelCount(c.channel_mask), c.category_mask,
             (c.category_mask & kClassicCats) ? "on" : "off");
}

static void defaultConfig() {
  Config c;
  c.cfg_version = 0;
  c.role = ROLE_WIFI;
  c.category_mask = kAllCategoriesMask;
  c.channel_mask = 0;
  c.ble_duty_pct = 0;
  c.report_ms = 1000;
  applyConfig(c);
}

static void goHome() {
  if (curChannel != kHomeChannel) {
    setRadioChannel(kHomeChannel);
    curChannel = kHomeChannel;
  }
}

static void sendHello() {
  {  // told to use Long Range: call for a lost master in both modes, in turns
    static bool lr = false;
    const bool want = (cfg.node_flags & NF_LONG_RANGE) && espLink.lrCapable();
    lr = want ? !lr : false;
    espLink.setBroadcastLongRange(lr);
  }
  Hello h;
  memcpy(h.mac, myMac, 6);
  h.fw_version = SS_FW_VERSION;
  h.hw = SS_NODE_HW;
  h.caps = CAP_WIFI24 | CAP_BT_CLASSIC;
#ifdef SS_CYD
  h.caps |= CAP_DISPLAY | CAP_DASH;
#endif
  h.role_pref = ROLE_AUTO;
  uint8_t buf[32];
  h.auth = espLink.authTag(buf, helloAuthInput(h, buf, sizeof(buf)));
  // Broadcast, signed with the cluster key: the master can't decrypt a node it hasn't met yet.
  espLink.send(nullptr, MSG_HELLO, myId, buf, encodeHello(h, buf, sizeof(buf)));
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
  hb.ble_adv = classicFound;  // this node's Bluetooth count is Classic inquiry results
  hb.link = (espLink.lrCapable() ? HL_LR_CAPABLE : 0) | (paired && espLink.peerLongRange(coreMac) ? HL_LR_ON : 0);
#ifdef SS_CYD
  hb.batt_pct = cyd::batteryPercent();
  hb.sd = cydsd::state();
  cydgps::fill(hb, millis());
#endif
  uint8_t buf[64];
  espLink.send(coreMac, MSG_HEARTBEAT, myId, buf, encodeHeartbeat(hb, buf, sizeof(buf)));
#ifdef SS_CYD
  // GPS details for the web page's GPS window, every 5 s
  static uint32_t lastGps = 0;
  static GpsInfo gi;
  if (millis() - lastGps >= 5000 && cydgps::info(gi, millis())) {
    lastGps = millis();
    uint8_t gb[kMaxPayload];
    espLink.send(coreMac, MSG_GPS, myId, gb, encodeGpsInfo(gi, gb, sizeof(gb)));
  }
#endif
}

static void flushReports(uint32_t now) {
  Sighting out[kMaxSightingsPerFrame];
  Event ev;
  bool fire;
  portENTER_CRITICAL(&floodMux);
  fire = flood.poll(ev, now);
  portEXIT_CRITICAL(&floodMux);
  uint8_t buf[kMaxPayload];
  while (batch.size()) {
    const size_t n = batch.take(out, kMaxSightingsPerFrame, now);
    espLink.send(coreMac, MSG_SIGHTINGS, myId, buf, encodeSightings(out, n, buf, sizeof(buf)));
  }
  for (uint8_t i = 0; i < nPendingNames; ++i)
    espLink.send(coreMac, MSG_NAME, myId, buf, encodeName(pendingNames[i], buf, sizeof(buf)));
  if (fire) espLink.send(coreMac, MSG_EVENT, myId, buf, encodeEvent(ev, buf, sizeof(buf)));
  nPendingNames = 0;
#ifdef SS_CYD
  DashCommand dc;  // taps on the screen
  while (cyd::takeCommand(dc)) espLink.send(coreMac, MSG_DASH_CMD, myId, buf, encodeDashCommand(dc, buf, sizeof(buf)));
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

static void handlePacket(const RxPacket& p) {
  Header h;
  const uint8_t* pl;
  size_t n;
  if (!decodeFrame(p.data, p.len, h, &pl, &n)) return;
  if (unpaired) { nodePair.handle(p, h, pl, n, millis()); return; }  // only pairing until we have a key
  if (h.type == MSG_PAIR_REQ || h.type == MSG_PAIR_OFFER || h.type == MSG_PAIR_KEY) return;
  if (corePinned && memcmp(p.src, pinnedCore, 6) != 0) return;
  if (paired && memcmp(p.src, coreMac, 6) == 0) linkRssi = p.rssi;
  if (h.type == MSG_WELCOME) {
    Welcome w;
    if (!decodeWelcome(pl, n, w)) return;
    if (espLink.hasKey()) {
      uint8_t buf[32];
      if (w.auth != espLink.authTag(buf, welcomeAuthInput(w, myMac, buf, sizeof(buf)))) return;
    }
    if (!paired || memcmp(coreMac, p.src, 6) != 0) {
      memcpy(coreMac, p.src, 6);
      espLink.addPeer(coreMac);
      paired = true;
      char s[18];
      formatMac(coreMac, s);
      Out.printf("[classic] paired with master %s as node %u\n", s, w.node_index);
    }
    nodeSlot = w.node_index;
    lastCoreContact = millis();
  } else if (h.type == MSG_COMMAND) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    NodeCommand c;
    if (!decodeCommand(pl, n, c)) return;
    if (c.cmd == NC_REBOOT) {
      Out.println("[classic] reboot requested by the master");
      Serial.flush();
      delay(100);
      ESP.restart();
    } else if (c.cmd == NC_IDENTIFY) {
      static uint32_t started = 0;
      // The master repeats a request for ~16 s: one flash per request.
      if (!started || millis() - started > 20000) {
        started = millis();
        identifyUntil = millis() + (uint32_t)(c.arg ? c.arg : 10) * 1000;
        Out.println("[classic] identify requested by the master");
      }
    } else if (c.cmd == NC_UNPAIR) {
      Out.println("[classic] the master forgot this node");
      unpairAndRestart();
#ifdef SS_CYD
    } else if (c.cmd == NC_SD_FORMAT) {
      // The master repeats commands for a while: format once per request token.
      static uint8_t lastToken = 0;
      if (c.arg && c.arg != lastToken) {
        lastToken = c.arg;
        Out.println("[classic] SD card format requested from the master's page");
        cydsd::requestFormat();
      }
#endif
    }
#ifdef SS_CYD
  } else if (h.type == MSG_DASH) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    Dash d;
    if (decodeDash(pl, n, d)) cyd::onDash(d, millis());
  } else if (h.type == MSG_DASH_VIEW) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    cyd::onView(pl, n, millis());
  } else if (h.type == MSG_LOG) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    LogRec r;
    if (decodeLogRec(pl, n, r)) cydsd::onRecord(r);
  } else if (h.type == MSG_FILE_REQ) {
    // A download from the master's page: answer, and stay on its channel while it runs
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    FileReq q;
    if (!decodeFileReq(pl, n, q)) return;
    goHome();
    fileBusyUntil = millis() + 3000;
    serveFileReq(q, cydsd::files(), [](const uint8_t* b, size_t len, void*) {
      espLink.send(coreMac, MSG_FILE, myId, b, len);
    }, nullptr);
    lastCoreContact = millis();
#endif
  } else if (h.type == MSG_CONFIG) {
    if (!paired || memcmp(coreMac, p.src, 6) != 0) return;
    Config c;
    if (!decodeConfig(pl, n, c)) return;
    lastCoreContact = millis();
    uint8_t a[kMaxConfigSize], b[kMaxConfigSize];
    const size_t la = encodeConfig(c, a, sizeof(a)), lb = encodeConfig(cfg, b, sizeof(b));
    if (la != lb || memcmp(a, b, la) != 0) applyConfig(c);
    espLink.setLrFast((c.node_flags & NF_LR_FAST) != 0);
    espLink.setPeerLongRange(coreMac, (c.node_flags & NF_LONG_RANGE) != 0);
  }
}

// ---------------------------------------------------------------------------
// Status + commands (same JSON shape as a C5 node, so the web UI shows it)

static void emitStatus(uint32_t now) {
  if (!g_jsonOn) return;
#ifdef SS_CYD
  const char* board = "cyd";
#else
  const char* board = "classic";
#endif
  Out.printf("{\"t\":\"status\",\"role\":\"node\",\"board\":\"%s\",\"setup\":true,\"fw\":%u,\"hw\":%u,\"mac\":",
             board, SS_FW_VERSION, (unsigned)SS_NODE_HW);
  jsonMac(myMac);
  Out.printf(",\"id\":\"%04X\",\"key\":%s,\"ms\":%lu,\"cats\":%u,\"ch\":%u,\"channels\":%u,\"ble_duty\":0,"
             "\"wifi_frames\":%lu,\"ble_adv\":%lu,\"classic\":%lu,\"inquiries\":%lu,\"bt\":%s,\"drops\":%lu",
             myId, clusterKey.length() ? "true" : "false", (unsigned long)now, catMask, curChannel,
             channelCount(cfg.channel_mask), (unsigned long)wifiFrames, (unsigned long)classicFound,
             (unsigned long)classicFound, (unsigned long)inquiries, btReady ? "true" : "false",
             (unsigned long)queueDrops);
  Out.printf(",\"heap\":%lu,\"heap_min\":%lu,\"heap_big\":%lu,\"rx_drop\":%lu,\"not_acked\":%lu,\"contact_ms\":%ld",
             (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
             (unsigned long)espLink.rxDropped(), (unsigned long)espLink.notDelivered(), paired ? (long)(now - lastCoreContact) : -1L);
  Out.printf(",\"paired\":%s,\"slot\":%d,\"pinned\":%s,\"scan\":\"%s\",\"cfg\":%u,\"core\":", paired ? "true" : "false",
             paired ? (int)nodeSlot : -1, corePinned ? "true" : "false",
             wantsWifi() ? "wifi + classic" : "classic", cfg.cfg_version);
  if (paired) jsonMac(coreMac); else Out.print("null");
  Out.print(",\"label\":");
  jsonStr(cfg.label);
  Out.printf(",\"unpaired\":%s", unpaired ? "true" : "false");
  if (unpaired) nodePair.status(Out, now);
#ifdef SS_CYD
  cyd::describe(Out);
#endif
  Out.printf(",\"send_fail\":%lu}\n", (unsigned long)espLink.sendFailures());
}

static void saveAndReboot(const char* why) {
  Out.printf("{\"t\":\"ack\",\"cmd\":\"%s\",\"reboot\":true}\n", why);
  Serial.flush();
  delay(300);
  ESP.restart();
}

static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  uint8_t mac[6];
  if (line.startsWith("clock ")) {
    // The web page's clock: only the master logs, nothing to do here
  } else if (line.startsWith("key ")) {
    String k = line.substring(4);
    k.trim();
    prefs.putString("key", k == "-" ? "" : k);
    saveAndReboot("key");
  } else if (line.startsWith("core ")) {
    String m = line.substring(5);
    m.trim();
    if (m == "-") { prefs.remove("core"); saveAndReboot("core"); }
    else if (sscanf(m.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
      prefs.putBytes("core", mac, 6);
      saveAndReboot("core");
    }
  } else if (line == "pair" || line == "pair again") {
    unpairAndRestart();
  } else if (line == "factory") {
    prefs.clear();
    saveAndReboot("factory");
  } else if (line == "reboot") {
    saveAndReboot("reboot");
  } else if (line == "status") {
    const bool j = g_jsonOn;
    g_jsonOn = true;
    emitStatus(millis());
    g_jsonOn = j;
  } else if (handleCommonCommand(line)) {
#ifdef SS_CYD
  } else if (cyd::handleCommand(line)) {
  } else if (cydsd::handleCommand(line)) {
  } else if (cydgps::handleCommand(line)) {
#endif
  } else if (line.startsWith("role") || line.startsWith("setup") || line.startsWith("wifi") || line.startsWith("cats")) {
    Out.println("[classic] this board is always a Classic node; categories and work come from the master");
  } else {
    Out.printf("[classic] unknown command: %s\n", line.c_str());
  }
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') { if (line.length() < 80) line += c; continue; }
    handleLine(line);
    line = "";
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  prefs.begin("skysentinel", false);
  clusterKey = prefs.isKey("key") ? prefs.getString("key", "") : String("");
  corePinned = prefs.isKey("core") && prefs.getBytes("core", pinnedCore, 6) == 6;
  unpaired = !clusterKey.length();
  pinMode(pairing::bootPin(), INPUT_PULLUP);
#if !defined(SS_CYD) && defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  hitQueue = xQueueCreate(kQueueDepth, sizeof(RxItem));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_read_mac(myMac, ESP_MAC_WIFI_STA);
  myId = nodeIdFromMac(myMac);
  if (!espLink.begin(clusterKey.c_str(), kHomeChannel, 16)) Out.println("[classic] ESP-NOW init FAILED");
  if (corePinned && !unpaired) espLink.addPeer(pinnedCore);
  if (unpaired) nodePair.begin(&espLink, myMac, myId, SS_NODE_HW, CAP_WIFI24 | CAP_BT_CLASSIC
#ifdef SS_CYD
                               | CAP_DISPLAY | CAP_DASH
#endif
                               , SS_FW_VERSION);

  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPromisc);
  esp_wifi_set_promiscuous(true);

  btReady = startClassic();
  defaultConfig();
#ifdef SS_CYD
  cyd::begin(&prefs);
  cyd::bootStep("Radios", btReady ? "Wi-Fi + Classic BT" : "Classic BT FAILED", btReady ? 1 : 0);
  cydsd::begin();
  {
    const uint8_t sd = cydsd::state();
    char b[24];
    if (sd == SD_LOGGING) snprintf(b, sizeof(b), "logging, %lu MB", (unsigned long)cydsd::cardMB());
    else snprintf(b, sizeof(b), "%s", sd == SD_NO_CARD ? "no card" : sd == SD_UNFORMATTED ? "needs format" : sdStateName(sd));
    cyd::bootStep("SD card", b, sd == SD_LOGGING ? 1 : 0);
  }
  cydgps::begin(&prefs);
  if (unpaired) cyd::bootStep("Cluster", "not paired yet", 0);
#endif
  char s[18];
  formatMac(myMac, s);
  Out.printf("[classic] SkySentinel Classic node %s (id %04X) fw %u, key %s, Classic BT %s\n", s, myId,
             SS_FW_VERSION, clusterKey.length() ? "set" : "none", btReady ? "ready" : "FAILED");
  {
    const esp_reset_reason_t r = esp_reset_reason();
    const char* why = r == ESP_RST_POWERON ? "power on" : r == ESP_RST_SW ? "restart" : r == ESP_RST_PANIC ? "CRASH"
                    : (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? "WATCHDOG"
                    : r == ESP_RST_BROWNOUT ? "BROWNOUT (power dipped)" : r == ESP_RST_EXT ? "reset button" : "other";
    Out.printf("[classic] started: %s, free heap %lu bytes (largest block %lu)\n", why,
               (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  }
}

void loop() {
  const uint32_t now = millis();
  handleSerial();

  RxItem it;
  while (xQueueReceive(hitQueue, &it, 0) == pdTRUE) {
    batch.add(it.s, now);
    if (it.name_len && nPendingNames < 8 && names.shouldSend(it.s.addr, now)) {
      Name& nm = pendingNames[nPendingNames++];
      memcpy(nm.addr, it.s.addr, 6);
      nm.kind = it.s.kind;
      nm.len = it.name_len;
      memcpy(nm.name, it.name, it.name_len + 1);
      nm.what = NAME_DEVICE;
      nm.has_ap = it.has_ap;
      nm.ap = ApMeta();
      if (it.has_ap) { nm.ap.sec = it.sec; nm.ap.gen = it.gen; nm.ap.width = it.width; nm.ap.util = it.util; nm.ap.sta = it.sta; nm.ap.up_s = it.up_s; }
    }
  }

  RxPacket p;
  while (espLink.poll(p)) handlePacket(p);
  if (!unpaired) pollInquiry(now);  // unpaired: keep the radio free to hear the master's offer
#ifdef SS_CYD
  {
    cyd::NodeState st;
    st.my_id = myId;
    st.paired = paired;
    st.master_id = paired ? nodeIdFromMac(coreMac) : 0;
    st.link_rssi = linkRssi;
    st.configured = cfg.cfg_version != 0;
    st.classic_found = classicFound;
    st.inquiries = inquiries;
    st.wifi_frames = wifiFrames;
    st.wifi_channels = wantsWifi() ? channelCount(cfg.channel_mask & kMask24) : 0;
    st.classic_on = btReady && (catMask & kClassicCats);
    st.label = cfg.label;
    st.identify = (int32_t)(identifyUntil - now) > 0 || (unpaired && nodePair.blinking(now));
    st.screen_off = paired && cfg.cfg_version && (cfg.node_flags & NF_DISPLAY_OFF);
    st.unpaired = unpaired;
    st.pair_offer = unpaired && nodePair.hasOffer(now);
    st.pair_code = nodePair.code();
    st.pair_master = nodePair.offerFrom();
    cydsd::loop(now);
    cydgps::loop(now);
    st.sd = cydsd::state();
    st.gps = cydgps::state(now);
    st.gps_sats = cydgps::sats();
    st.sd_saved = cydsd::saved();
    cyd::loop(st, now);
    if (cyd::pendingCommand()) lastReport = 0;  // a tap: send it at the next pass
    if (cyd::wantsUnpair()) unpairAndRestart();
  }
#else
  // DevKit: BOOT held 5 s = forget the master and pair again. The blue LED (GPIO 2):
  // slow blink while unpaired, fast while identifying.
  {
    static uint32_t downSince = 0;
    if (digitalRead(pairing::bootPin()) == LOW) {
      if (!downSince) downSince = now ? now : 1;
      if (!unpaired && now - downSince > 5000) unpairAndRestart();
    } else {
      downSince = 0;
    }
#ifdef LED_BUILTIN
    const bool fast = (int32_t)(identifyUntil - now) > 0 || (unpaired && nodePair.blinking(now));
    digitalWrite(LED_BUILTIN, fast ? ((now / 125) % 2) : unpaired ? ((now / 600) % 2) : LOW);
#endif
  }
#endif

  if (unpaired) {
    if (nodePair.done()) {
      prefs.putString("key", nodePair.key());
      prefs.putBytes("core", nodePair.master(), 6);
      Out.println("[pair] key saved, restarting into the cluster");
      Serial.flush();
      delay(200);
      ESP.restart();
    }
    goHome();
    nodePair.poll(now);
    if (now - lastStatus >= kStatusEveryMs) { lastStatus = now; emitStatus(now); }
    batch.clear();
    delay(1);
    return;
  }

  if (now - lastStatus >= kStatusEveryMs) { lastStatus = now; emitStatus(now); }

  // Signed: a packet handled during this pass can set lastCoreContact a little after `now`.
  if (paired && (int32_t)(now - lastCoreContact) > (int32_t)kCoreLostMs) {
    Out.printf("[classic] lost master (nothing for %lu s; send failures %lu, not delivered %lu, rx dropped %lu, free heap %lu, min %lu, queue drops %lu), re-pairing\n",
               (unsigned long)((now - lastCoreContact) / 1000), (unsigned long)espLink.sendFailures(),
               (unsigned long)espLink.notDelivered(), (unsigned long)espLink.rxDropped(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
               (unsigned long)queueDrops);
    paired = false;
    defaultConfig();
  }
  if (!paired) {
    goHome();
    if (now - lastHello >= kHelloEveryMs) { lastHello = now; sendHello(); }
    batch.clear();
    delay(1);
    return;
  }
  if (now - lastReport >= cfg.report_ms || batch.full()) {
    goHome();
#ifdef SS_CYD
    const uint32_t every = kDashBeatMs;
#else
    const uint32_t every = cfg.cfg_version ? kHeartbeatEveryMs : kUnconfiguredBeatMs;
#endif
    // Heartbeat first: the master answers it with the config that keeps this node
    // paired, so a busy batch of reports must never crowd it out.
    if (now - lastHeartbeat >= every) { lastHeartbeat = now; sendHeartbeat(); }
    flushReports(now);
    lastReport = now;
    lastHop = now + kListenWindowMs;
    delay(1);
    return;
  }
  const uint64_t chans = wantsWifi() ? (cfg.channel_mask & kMask24) : 0;
  // Don't leave the master's channel while frames to it are still going out.
  const bool sending = (espLink.inFlight() && (int32_t)(now - lastHop) < (int32_t)(cfg.dwell_ms + 400))
                       || (int32_t)(fileBusyUntil - now) > 0;
  if (chans && !sending && (int32_t)(now - lastHop) >= (int32_t)cfg.dwell_ms) {
    const uint8_t ch = nextChannel(chans, &hopCursor);
    if (ch && ch != curChannel && setRadioChannel(ch)) curChannel = ch;
    lastHop = now;
  } else if (!chans) {
    goHome();
  }
  delay(1);
}
