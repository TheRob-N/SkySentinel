#include "core_common.h"

#include <new>

#include <esp_heap_caps.h>

#include "logger.h"
#include "ss_classify.h"

namespace ss {

bool g_jsonOn = true;
volatile uint32_t g_usbWriteMs = 0;
bool g_uiOn = false;
bool g_webOn = false;
OutTee Out;
DashAlerts* g_dashAlerts = nullptr;

static void dashDeviceAlert(const Device& d, const char* prefix) {
  if (!g_dashAlerts) return;
  const uint8_t level = alertLevel(d);
  if (!level) return;
  char label[kDashText + 1], text[kDashAlertText + 1];
  dashLabel(d, label, sizeof(label));
  snprintf(text, sizeof(text), "%s%s", prefix, label);
  g_dashAlerts->raise(level, d.category, text, millis());
}

size_t OutTee::write(const uint8_t* buf, size_t len) {
  // USB plugged in but nobody reading (right after flashing, page closed): the USB
  // driver would wait up to ~2 s per write and stall the radios. If a write doesn't
  // fully go through, stop writing to USB for 2 s, then try again.
  static uint32_t usbSkipUntil = 0;
  if ((int32_t)(millis() - usbSkipUntil) >= 0) {
    g_usbWriteMs = millis() | 1;
    if (Serial.write(buf, len) < len) usbSkipUntil = millis() + 2000;
  }
  if (!sink_ || !g_webOn) { line_ = ""; return len; }
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] == '\n') {
      if (line_.length() && line_[line_.length() - 1] == '\r') line_.remove(line_.length() - 1);
      sink_(line_.c_str(), line_.length());
      line_ = "";
    } else if (line_.length() < 32768) {
      line_ += (char)buf[i];
    }
  }
  return len;
}

static constexpr size_t kDevicesPsram = 2048;
static constexpr size_t kDevicesInternal = 256;

DeviceTable* createDeviceTable() {
  size_t cap = kDevicesPsram;
  Device* store = (Device*)heap_caps_malloc(cap * sizeof(Device), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!store) {
    cap = kDevicesInternal;
    store = (Device*)heap_caps_malloc(cap * sizeof(Device), MALLOC_CAP_8BIT);
  }
  if (!store) return nullptr;
  static DeviceTable* table = nullptr;
  if (!table) table = new DeviceTable(store, cap);
  return table;
}

void ingestSightings(DeviceTable& table, uint8_t slot, const Sighting* s, size_t n, uint32_t now_ms) {
  for (size_t i = 0; i < n; ++i) {
    Device* d = table.find(s[i].addr, radioClass(s[i].kind));
    const uint8_t prevConf = d ? d->conf : 0;
    const auto r = table.ingest(slot, s[i], now_ms, &d);
    if (!d || r == DeviceTable::SELF || r == DeviceTable::REJECTED || d->spoofed || d->alias_of || d->hidden) continue;
    const bool interesting = d->conf >= 50 || (d->flags & SF_STRONG_MATCH);
    if (interesting && (r == DeviceTable::NEW || prevConf < 50)) {
      emitDevice(*d, r == DeviceTable::NEW ? "new" : "upgraded");
      dashDeviceAlert(*d, "New: ");
      logger::device(*d, r != DeviceTable::NEW);
    }
  }
}

void pollSpamEvents(DeviceTable& table) {
  Event ev;
  if (table.pollSpam(ev)) emitEvent(ev, -1);
}

void jsonStr(const char* s) {
  Out.print('"');
  for (; *s; ++s) {
    const char c = *s;
    if (c == '"' || c == '\\') { Out.print('\\'); Out.print(c); }
    else if ((uint8_t)c < 0x20) Out.printf("\\u%04x", (uint8_t)c);
    else Out.print(c);
  }
  Out.print('"');
}

void jsonMac(const uint8_t* mac) {
  char s[18];
  formatMac(mac, s);
  jsonStr(s);
}

void emitDevice(const Device& d, const char* why) {
  if (!g_jsonOn) return;
  Out.print("{\"t\":\"dev\",\"why\":"); jsonStr(why);
  Out.printf(",\"id\":%lu,\"mac\":", (unsigned long)d.id); jsonMac(d.addr);
  Out.print(",\"cat\":"); jsonStr(categoryName(d.category));
  Out.print(",\"sig\":"); jsonStr(sigName(d.sig_id));
  Out.printf(",\"conf\":%u,\"strong\":%s,\"rssi\":%d,\"ch\":%u,\"rot\":%u,\"fp\":\"%08lx\",\"random\":%s",
                d.conf, (d.flags & SF_STRONG_MATCH) ? "true" : "false", d.rssi_last, d.channel, d.rotations,
                (unsigned long)d.fp, (d.flags & SF_RANDOM_ADDR) ? "true" : "false");
  if (d.company != 0xFFFF) Out.printf(",\"company\":%u", d.company);
  if (d.name[0]) { Out.print(",\"name\":"); jsonStr(d.name); }
  if (d.spoofed) Out.print(",\"spoofed\":true");
  if (d.alias_of) Out.printf(",\"alias_of\":%lu", (unsigned long)d.alias_of);
  if (d.aliases) Out.printf(",\"aliases\":%u", d.aliases);
  {
    int8_t br;
    uint8_t cnt;
    const int b = bestNode(d, millis(), 60000, &br, &cnt);
    if (b >= 0) Out.printf(",\"nb\":[%d,%d,%u]", b, br, cnt);
  }
  Out.print(",\"nodes\":[");
  bool first = true;
  for (uint8_t i = 0; i < kMaxNodes; ++i) {
    if (d.node_rssi[i] == -127) continue;
    Out.printf("%s[%u,%d]", first ? "" : ",", i, d.node_rssi[i]);
    first = false;
  }
  Out.println("]}");
}

void emitEvent(const Event& ev, int slot) {
  logger::event(ev);
  if (g_dashAlerts) {
    char text[kDashAlertText + 1];
    if (ev.type == EV_BLE_SPAM) snprintf(text, sizeof(text), "BLE spam: %u fake devices", ev.count);
    else if (ev.type == EV_WIFI_SPAM) snprintf(text, sizeof(text), "Wi-Fi spam: %u fake networks", ev.count);
    else if (ev.type == EV_EVIL_TWIN) snprintf(text, sizeof(text), "EVIL TWIN: 1 radio, %u networks", ev.count);
    else if (ev.type == EV_RF_JAM) snprintf(text, sizeof(text), "JAMMING on %u.%02u MHz", ev.count / 100, ev.count % 100);
    else snprintf(text, sizeof(text), "%s flood on ch %u", ev.type == EV_DEAUTH_FLOOD ? "Deauth" : "Disassoc", ev.channel);
    g_dashAlerts->raise(2, CAT_PENTEST, text, millis());
  }
  if (!g_jsonOn) return;
  if (ev.type == EV_RF_JAM) {
    Out.printf("{\"t\":\"event\",\"event\":\"rf_jam\",\"slot\":%d,\"mhz\":\"%u.%02u\",\"db\":%u}\n", slot, ev.count / 100,
               ev.count % 100, ev.dst[0]);
    return;
  }
  if (ev.type == EV_BLE_SPAM || ev.type == EV_WIFI_SPAM) {
    Out.printf("{\"t\":\"event\",\"event\":\"%s\",", ev.type == EV_BLE_SPAM ? "ble_spam" : "wifi_spam");
    Out.printf("\"slot\":%d,\"count\":%u,\"rssi\":%d,\"src\":",
                  slot, ev.count, (int8_t)ev.channel);
    jsonMac(ev.src);
    Out.println("}");
    return;
  }
  Out.printf("{\"t\":\"event\",\"event\":\"%s\",\"slot\":%d,\"ch\":%u,\"count\":%u,\"src\":",
                ev.type == EV_EVIL_TWIN ? "evil_twin" : ev.type == EV_DEAUTH_FLOOD ? "deauth_flood" : "disassoc_flood",
                slot, ev.channel, ev.count);
  jsonMac(ev.src);
  Out.print(",\"bssid\":");
  jsonMac(ev.bssid);
  Out.println("}");
}

void emitCounts(const DeviceTable& table, uint32_t now_ms) {
  Out.print(",\"now\":{");
  for (uint8_t c = 1; c < CAT_COUNT; ++c)
    Out.printf("%s\"%s\":%u", c > 1 ? "," : "", categoryName(c), (unsigned)table.countNow(c, now_ms));
  Out.print("},\"total\":{");
  for (uint8_t c = 1; c < CAT_COUNT; ++c)
    Out.printf("%s\"%s\":%lu", c > 1 ? "," : "", categoryName(c), (unsigned long)table.total(c));
  Out.printf("},\"devices_now\":%u,\"devices_total\":%lu,\"table\":%u,\"capacity\":%u,\"stitches\":%lu,"
                "\"spoofed\":%lu,\"spam_active\":%s",
                (unsigned)table.countNowAll(now_ms), (unsigned long)table.totalAll(), (unsigned)table.used(),
                (unsigned)table.capacity(), (unsigned long)table.stitches(), (unsigned long)table.spoofedTotal(),
                table.spamActive(now_ms) ? "true" : "false");
}

bool g_probesOn = false;
uint32_t g_listWindowMs = 60000;
// Allocated on first use: only a board with a device table files notes, and the
// CYD (an ESP32 with Classic Bluetooth) has no RAM to spare for it.
using Notes = NoteRing<192>;
static Notes* g_notes = nullptr;
static Notes* notes() {
  if (!g_notes) g_notes = new (std::nothrow) Notes();
  return g_notes;
}

void applyName(DeviceTable& table, const Name& n, uint32_t now_ms) {
  Device* d = table.setName(n);
  if (!d) return;
  Notes* nt = notes();
  if (!nt) return;
  if (n.what == NAME_PROBE) {
    if (g_probesOn) nt->add(d->id, Notes::TAG_PROBE, n.name, now_ms);
  } else if (n.what == NAME_POS) {
    nt->add(d->id, Notes::TAG_POS, n.name, now_ms, true);
  } else if (n.what == NAME_LINK) {
    nt->add(d->id, Notes::TAG_LINK, n.name, now_ms, true);
  } else if (n.what == NAME_BATT) {
    nt->add(d->id, Notes::TAG_BATT, n.name, now_ms, true);
  } else if (n.what == NAME_MESH) {
    nt->add(d->id, Notes::TAG_MESH, n.name, now_ms, true);
  } else if (n.what == NAME_RID_ID) {
    nt->add(d->id, Notes::TAG_RID, n.name, now_ms, true);
  } else if (n.what == NAME_RID_MOVE) {
    nt->add(d->id, Notes::TAG_MOVE, n.name, now_ms, true);
  } else if (n.what == NAME_RID_OP) {
    nt->add(d->id, Notes::TAG_OP, n.name, now_ms, true);
  } else if (n.what == NAME_RID_TEXT) {
    nt->add(d->id, Notes::TAG_TEXT, n.name, now_ms, true);
  } else if (n.has_ap && n.ap.wps[0]) {
    nt->add(d->id, Notes::TAG_WPS, n.ap.wps, now_ms, true);
  }
}

void emitDeviceInfo(const DeviceTable& table, uint32_t id, uint32_t now_ms) {
  Out.printf("{\"t\":\"info\",\"id\":%lu,\"probes_on\":%s", (unsigned long)id, g_probesOn ? "true" : "false");
  if (g_notes) g_notes->forEach(id, Notes::TAG_WPS, now_ms, [&](const char* t, uint32_t) { Out.print(",\"wps\":"); jsonStr(t); });
  if (g_notes) g_notes->forEach(id, Notes::TAG_POS, now_ms, [&](const char* t, uint32_t age) {
    Out.print(",\"pos\":"); jsonStr(t); Out.printf(",\"pos_age\":%lu", (unsigned long)(age / 1000));
  });
  if (g_notes) {
    g_notes->forEach(id, Notes::TAG_LINK, now_ms, [&](const char* t, uint32_t age) {
      Out.print(",\"link\":"); jsonStr(t); Out.printf(",\"link_age\":%lu", (unsigned long)(age / 1000));
    });
    g_notes->forEach(id, Notes::TAG_BATT, now_ms, [&](const char* t, uint32_t) { Out.printf(",\"batt\":%d", atoi(t)); });
    const struct { uint8_t tag; const char* key; } kRid[] = {
        {Notes::TAG_RID, "rid"}, {Notes::TAG_MOVE, "move"}, {Notes::TAG_OP, "op"}, {Notes::TAG_TEXT, "rtext"}};
    for (const auto& k : kRid)
      g_notes->forEach(id, k.tag, now_ms, [&](const char* t, uint32_t age) {
        Out.printf(",\"%s\":", k.key); jsonStr(t); Out.printf(",\"%s_age\":%lu", k.key, (unsigned long)(age / 1000));
      });
    g_notes->forEach(id, Notes::TAG_MESH, now_ms, [&](const char* t, uint32_t age) {
      Out.print(",\"mesh\":"); jsonStr(t); Out.printf(",\"mesh_age\":%lu", (unsigned long)(age / 1000));
    });
    // An access point: the clients last heard talking to it
    char mac[18] = "";
    table.forEach([&](const Device& d) { if (d.id == id) formatMac(d.addr, mac); });
    if (mac[0]) {
      Out.print(",\"clients\":[");
      bool f1 = true;
      g_notes->forEachTag(Notes::TAG_LINK, now_ms, [&](uint32_t cid, const char* t, uint32_t age) {
        if (strcmp(t, mac) != 0 || age > 600000) return;
        Out.printf("%s[%lu,%lu]", f1 ? "" : ",", (unsigned long)cid, (unsigned long)(age / 1000));
        f1 = false;
      });
      Out.print("]");
    }
  }
  // Every radio that has heard it: [slot, rssi, seconds ago]
  Out.print(",\"heard\":[");
  {
    bool f = true;
    table.forEach([&](const Device& d) {
      if (d.id != id) return;
      for (uint8_t i = 0; i < kMaxNodes; ++i) {
        if (d.node_rssi[i] == -127 || !d.node_ms[i]) continue;
        Out.printf("%s[%u,%d,%lu]", f ? "" : ",", i, d.node_rssi[i], (unsigned long)((now_ms - d.node_ms[i]) / 1000));
        f = false;
      }
    });
  }
  Out.print("]");
  Out.print(",\"probes\":[");
  bool first = true;
  if (g_notes) g_notes->forEach(id, Notes::TAG_PROBE, now_ms, [&](const char* t, uint32_t age) {
    Out.print(first ? "[" : ",[");
    first = false;
    jsonStr(t);
    Out.printf(",%lu]", (unsigned long)(age / 1000));
  });
  Out.println("]}");
}

void emitAccessPointGroup(const DeviceTable& table, uint32_t id, uint32_t now_ms) {
  Out.printf("{\"t\":\"aps\",\"id\":%lu,\"list\":[", (unsigned long)id);
  bool first = true;
  table.forEach([&](const Device& d) {
    if (d.id != id && d.alias_of != id) return;
    Out.print(first ? "{\"m\":" : ",{\"m\":");
    first = false;
    jsonMac(d.addr);
    Out.print(",\"n\":");
    jsonStr(d.name);
    Out.printf(",\"ch\":%u,\"r\":%d,\"a\":%lu,\"h\":%lu,\"p\":%s}", d.channel, d.rssi_last,
                  (unsigned long)((now_ms - d.last_ms) / 1000), (unsigned long)d.hits, d.id == id ? "true" : "false");
  });
  Out.println("]}");
}

void emitFollow(const DeviceTable& table, const FollowAlert& a) {
  table.forEach([&](const Device& d) {
    if (d.id != a.device_id) return;
    logger::follow(d, a);
    if (g_dashAlerts) {
      char label[kDashText + 1], text[kDashAlertText + 1];
      dashLabel(d, label, sizeof(label));
      snprintf(text, sizeof(text), "%s: %s", a.lingering ? "LINGERING" : "FOLLOWING", label);
      g_dashAlerts->raise(2, d.category, text, millis());
    }
    if (!g_jsonOn) return;
    Out.printf("{\"t\":\"event\",\"event\":\"%s\",\"id\":%lu,\"mac\":", a.lingering ? "lingering" : "following",
               (unsigned long)d.id);
    jsonMac(d.addr);
    Out.printf(",\"cat\":\"%s\",\"sig\":", categoryName(d.category));
    jsonStr(sigName(d.sig_id));
    if (d.name[0]) { Out.print(",\"name\":"); jsonStr(d.name); }
    Out.printf(",\"places\":%u,\"min\":%lu,\"rssi\":%d,\"k\":\"%s\"}\n", a.places,
               (unsigned long)(a.with_us_ms / 60000), d.rssi_last,
               d.radio == 1 ? "w" : (d.radio == 2 ? "b" : (d.radio == 3 ? "c" : (d.radio == 4 ? "l" : "o"))));
  });
}

bool handleCommonCommand(const String& line) {
  if (line == "ui on") { g_uiOn = true; g_jsonOn = true; Out.println("{\"t\":\"ack\",\"cmd\":\"ui on\"}"); return true; }
  if (line == "ui off") { g_uiOn = false; return true; }
  if (line == "json on") { g_jsonOn = true; return true; }
  if (line == "json off") { g_jsonOn = false; return true; }
  if (line.startsWith("list window ")) {
    // How far back the web page's LIVE DEVICES list goes: 30 s to 2 hours (the table
    // keeps the most recent few hundred devices, so a busy place shortens it anyway).
    long s = line.substring(12).toInt();
    if (s < 30) s = 30;
    if (s > 7200) s = 7200;
    g_listWindowMs = (uint32_t)s * 1000;
    return true;
  }
  if (line == "probes on" || line == "probes off") {
    g_probesOn = line == "probes on";
    if (!g_probesOn && g_notes) g_notes->clearTag(Notes::TAG_PROBE);
    Out.printf("[core] networks phones look for: %s\n", g_probesOn ? "collected (memory only)" : "off, forgotten");
    return true;
  }
  return false;
}

void emitDeviceList(const DeviceTable& table, uint32_t now_ms, size_t max_devices) {
  if (!g_jsonOn || !(g_uiOn || g_webOn)) return;
  // Pick the top N by (confidence, recency) without allocating: repeated passes over
  // a small "already sent" threshold would be O(n^2); instead do a simple selection
  // of the cut-off score first.
  auto score = [&](const Device& d) -> uint32_t {
    const uint32_t age = now_ms - d.last_ms;
    return ((uint32_t)d.conf << 16) | (age > 0xFFFF ? 0 : 0xFFFF - age);
  };
  auto listed = [&](const Device& d) {
    return !d.hidden && !d.alias_of && !d.spoofed && (int32_t)(now_ms - d.last_ms) <= (int32_t)g_listWindowMs;
  };
  size_t eligible = 0;
  table.forEach([&](const Device& d) { if (listed(d)) ++eligible; });
  uint32_t cutoff = 0;
  if (eligible > max_devices) {
    // Binary search the score threshold that keeps at most max_devices.
    uint32_t lo = 0, hi = 0xFFFFFFFFu;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      size_t n = 0;
      table.forEach([&](const Device& d) { if (listed(d) && score(d) > mid) ++n; });
      if (n > max_devices) lo = mid + 1; else hi = mid;
    }
    cutoff = lo;
  }
  Out.printf("{\"t\":\"devs\",\"ms\":%lu,\"n\":%u,\"win\":%lu,\"d\":[", (unsigned long)now_ms, (unsigned)eligible,
             (unsigned long)(g_listWindowMs / 1000));
  bool first = true;
  table.forEach([&](const Device& d) {
    if (!listed(d) || (eligible > max_devices && score(d) <= cutoff)) return;
    Out.printf("%s{\"i\":%lu,\"m\":", first ? "" : ",", (unsigned long)d.id);
    jsonMac(d.addr);
    Out.printf(",\"c\":\"%s\",\"s\":", categoryName(d.category));
    jsonStr(sigName(d.sig_id));
    Out.printf(",\"f\":%u,\"r\":%d,\"rx\":%d,\"a\":%lu,\"o\":%u,\"k\":\"%s\",\"h\":%lu,\"ch\":%u,\"x\":%u",
                  d.conf, d.rssi_last, d.rssi_max, (unsigned long)((now_ms - d.last_ms) / 1000), d.rotations,
                  d.radio == 1 ? "w" : (d.radio == 2 ? "b" : (d.radio == 3 ? "c" : (d.radio == 4 ? "l" : "o"))), (unsigned long)d.hits, d.channel, d.flags);
    if (d.aliases) Out.printf(",\"al\":%u", d.aliases);
    {  // the node that hears it best: [slot, rssi, how many nodes heard it in the last minute]
      int8_t br;
      uint8_t cnt;
      const int b = bestNode(d, now_ms, 60000, &br, &cnt);
      if (b >= 0) Out.printf(",\"nb\":[%d,%d,%u]", b, br, cnt);
    }
    if (d.company != 0xFFFF) Out.printf(",\"co\":%u", d.company);
    if (d.places > 1) Out.printf(",\"pl\":%u", d.places);
    if (d.follow_alerted) Out.print(",\"fo\":1");
    if (d.name[0]) { Out.print(",\"n\":"); jsonStr(d.name); }
    if (d.ap_sec)
      Out.printf(",\"ap\":[%u,%u,%u,%u,%u,%lu]", d.ap_sec, d.ap_gen, d.ap_width, d.ap_sta, d.ap_util,
                 (unsigned long)(d.ap_up_s ? d.ap_up_s + (now_ms - d.ap_up_ms) / 1000 : 0));
    Out.print(",\"nr\":[");
    bool f2 = true;
    for (uint8_t i = 0; i < kMaxNodes; ++i) {
      if (d.node_rssi[i] == -127) continue;
      Out.printf("%s[%u,%d]", f2 ? "" : ",", i, d.node_rssi[i]);
      f2 = false;
    }
    Out.print("]}");
    first = false;
  });
  Out.println("]}");
}

void dumpDevices(const DeviceTable& table, uint32_t now_ms) {
  table.forEach([&](const Device& d) {
    if (now_ms - d.last_ms > 60000 || (d.hidden && !d.spoofed)) return;
    char m[18];
    formatMac(d.addr, m);
    const char* tag = d.spoofed ? " [SPOOFED]" : (d.alias_of ? " [same AP]" : "");
    Out.printf("  #%-5lu %s %-10s %-34s conf %3u rssi %4d rot %u %s%s\n", (unsigned long)d.id, m,
                  categoryName(d.category), sigName(d.sig_id), d.conf, d.rssi_last, d.rotations, d.name, tag);
  });
}

}  // namespace ss
