#include "ss_nodes.h"

#include <string.h>

namespace ss {

int NodeRegistry::find(const uint8_t mac[6]) const {
  for (int i = 0; i < kMaxNodes; ++i)
    if (nodes_[i].used && memcmp(nodes_[i].mac, mac, 6) == 0) return i;
  return -1;
}

int NodeRegistry::add(const Hello& h, uint32_t now_ms, bool* is_new) {
  int slot = find(h.mac);
  bool fresh = false;
  if (slot < 0) {
    for (int i = 0; i < kMaxNodes && slot < 0; ++i) if (!nodes_[i].used && !reserved(i)) slot = i;
    if (slot < 0) {  // reuse the longest-offline slot
      for (int i = 0; i < kMaxNodes; ++i)
        if (!nodes_[i].online && !reserved(i) && (slot < 0 || nodes_[i].last_ms < nodes_[slot].last_ms)) slot = i;
    }
    if (slot < 0) return -1;
    nodes_[slot] = NodeInfo();
    nodes_[slot].used = true;
    memcpy(nodes_[slot].mac, h.mac, 6);
    fresh = true;
  }
  NodeInfo& n = nodes_[slot];
  if (!n.online) fresh = true;  // returning after a timeout counts as a topology change
  n.online = true;
  n.id = nodeIdFromMac(h.mac);
  n.hw = h.hw;
  n.caps = h.caps;
  n.fw = h.fw_version;
  n.last_ms = now_ms;
  if (is_new) *is_new = fresh;
  return slot;
}

int NodeRegistry::touch(const uint8_t mac[6], uint32_t now_ms, int8_t rssi) {
  const int slot = find(mac);
  if (slot < 0 || !nodes_[slot].online) return -1;
  nodes_[slot].last_ms = now_ms;
  if (rssi > -127) nodes_[slot].link_rssi = rssi;
  return slot;
}

void NodeRegistry::noteSeq(int slot, uint16_t seq) {
  NodeInfo& n = nodes_[slot];
  ++n.frames;
  if (n.have_seq) {
    const uint16_t gap = (uint16_t)(seq - n.last_seq - 1);
    if (gap < 1000) n.lost += gap;  // larger jumps = node rebooted; don't count
  }
  n.have_seq = true;
  n.last_seq = seq;
}

bool NodeRegistry::expire(uint32_t now_ms, uint32_t timeout_ms) {
  bool changed = false;
  for (auto& n : nodes_) {
    if (n.used && n.online && now_ms - n.last_ms > timeout_ms) {
      n.online = false;
      changed = true;
    }
  }
  return changed;
}

void NodeRegistry::forget(int slot) {
  if (slot >= 0 && slot < kMaxNodes) nodes_[slot] = NodeInfo();
}

uint8_t NodeRegistry::onlineCount() const {
  uint8_t c = 0;
  for (const auto& n : nodes_) if (n.used && n.online) ++c;
  return c;
}

// Shares a plan's Wi-Fi channels among the nodes whose role is ROLE_WIFI:
// 2.4 GHz across all of them, 5 GHz only across 5 GHz-capable ones.
// Nodes that can't do 5 GHz (classic ESP32, ESP32-S3) take 2.4 GHz when there are
// any, leaving the 5 GHz-capable ones free for 5 GHz.
static void splitWifi(NodeInfo* nodes, const int* online, int n, uint64_t mask) {
  int w24[kMaxNodes], w5[kMaxNodes];
  int n24 = 0, n5 = 0, only24 = 0;
  for (int k = 0; k < n; ++k) {
    const int s = online[k];
    if (nodes[s].cfg.role != ROLE_WIFI && nodes[s].cfg.role != ROLE_BOTH) continue;
    if (!(nodes[s].caps & CAP_WIFI5)) ++only24;
  }
  for (int k = 0; k < n; ++k) {
    const int s = online[k];
    if (nodes[s].cfg.role != ROLE_WIFI && nodes[s].cfg.role != ROLE_BOTH) continue;
    const bool has5 = (nodes[s].caps & CAP_WIFI5) != 0;
    nodes[s].cfg.channel_mask = 0;
    if (!has5 || !only24 || !(mask & kMask5)) w24[n24++] = s;
    if (has5) w5[n5++] = s;
  }
  for (int k = 0; k < n24; ++k)
    nodes[w24[k]].cfg.channel_mask = splitChannels(mask & kMask24, (uint8_t)n24, (uint8_t)k);
  for (int k = 0; k < n5; ++k)
    nodes[w5[k]].cfg.channel_mask |= splitChannels(mask & kMask5, (uint8_t)n5, (uint8_t)k);
  for (int k = 0; k < n24; ++k)
    if (channelCount(nodes[w24[k]].cfg.channel_mask) <= 1) nodes[w24[k]].cfg.dwell_ms = 1000;
}

void NodeRegistry::assign(uint16_t category_mask, AssignMode mode) {
  ++cfg_version_;
  if (cfg_version_ == 0) cfg_version_ = 1;  // 0 means "unassigned" on the node
  const ScanPlan plan = planForCategories(category_mask);

  // Nodes pinned to EVERYTHING scan the whole plan on their own (a node in another
  // room, or out at the edge of range); the rest share the work between them.
  int online[kMaxNodes];
  int n = 0;
  for (int i = 0; i < kMaxNodes; ++i)
    if (nodes_[i].used && nodes_[i].online) {
      if (nodes_[i].force_role != ROLE_ALL) { online[n++] = i; continue; }
      NodeInfo& node = nodes_[i];
      node.cfg = Config();
      node.cfg.cfg_version = cfg_version_;
      node.cfg.category_mask = category_mask;
      node.cfg.dwell_ms = plan.dwell_ms;
      node.cfg.report_ms = (node.caps & CAP_DASH) ? kDashReportMs : kNodeReportMs;
      node.cfg.rssi_floor = -95;
      const bool canBle = (node.caps & CAP_BLE) != 0;
      const bool wifi = plan.channel_mask != 0, ble = canBle && plan.ble_duty_pct > 0;
      node.cfg.role = wifi && ble ? ROLE_BOTH : wifi ? ROLE_WIFI : ROLE_BLE;
      node.cfg.ble_duty_pct = ble ? plan.ble_duty_pct : 0;
      node.cfg.channel_mask = (node.caps & CAP_WIFI5) ? plan.channel_mask : (plan.channel_mask & kMask24);
    }
  if (n == 0) return;

  const bool needWifi = plan.channel_mask != 0;
  const bool needBle = plan.ble_duty_pct > 0;
  const bool split = mode == ASSIGN_SPLIT && n > 1;

  // Pick the dedicated BLE node when splitting a two-radio plan. Prefer one without
  // 5 GHz (least useful for Wi-Fi), else the most recently added.
  // ...but only if the others can still cover the Wi-Fi plan (5 GHz needs a 5 GHz node).
  auto coveredWithout = [&](int skip) {
    bool has24 = false, has5 = false;
    for (int k = 0; k < n; ++k) {
      if (online[k] == skip || nodes_[online[k]].force_role == ROLE_BLE) continue;  // pinned to BLE: no Wi-Fi
      has24 = has24 || (nodes_[online[k]].caps & CAP_WIFI24);
      has5 = has5 || (nodes_[online[k]].caps & CAP_WIFI5);
    }
    return has24 && (has5 || !(plan.channel_mask & kMask5));
  };
  int bleNode = -1;
  if (split && needWifi && needBle) {
    for (int k = 0; k < n && bleNode < 0; ++k) {
      const int s = online[k];
      if (nodes_[s].force_role == ROLE_AUTO && (nodes_[s].caps & CAP_BLE) && !(nodes_[s].caps & CAP_WIFI5) && coveredWithout(s)) bleNode = s;
    }
    for (int k = n - 1; k >= 0 && bleNode < 0; --k) {
      const int s = online[k];
      if (nodes_[s].force_role == ROLE_AUTO && (nodes_[s].caps & CAP_BLE) && coveredWithout(s)) bleNode = s;
    }
  }

  for (int k = 0; k < n; ++k) {
    const int s = online[k];
    NodeInfo& node = nodes_[s];
    node.cfg = Config();
    node.cfg.cfg_version = cfg_version_;
    node.cfg.category_mask = category_mask;
    node.cfg.dwell_ms = plan.dwell_ms;
    node.cfg.report_ms = (node.caps & CAP_DASH) ? kDashReportMs : kNodeReportMs;
    node.cfg.rssi_floor = -95;
    const bool canBle = (node.caps & CAP_BLE) != 0;
    const uint64_t myChans = (node.caps & CAP_WIFI5) ? plan.channel_mask : (plan.channel_mask & kMask24);
    if (node.force_role == ROLE_BLE && canBle) {          // pinned by you: Bluetooth only
      node.cfg.role = ROLE_BLE;
      node.cfg.ble_duty_pct = 100;
      node.cfg.channel_mask = 0;
    } else if (node.force_role == ROLE_WIFI) {            // Wi-Fi only
      node.cfg.role = ROLE_WIFI;
      node.cfg.ble_duty_pct = 0;
      node.cfg.channel_mask = myChans;
    } else if (node.force_role == ROLE_BOTH && canBle) {  // both (its share of Wi-Fi when splitting)
      node.cfg.role = ROLE_BOTH;
      node.cfg.ble_duty_pct = plan.ble_duty_pct ? plan.ble_duty_pct : 50;
      node.cfg.channel_mask = myChans;
    } else if (!canBle) {
      // No BLE (classic ESP32 node: its Bluetooth does Classic): Wi-Fi only, 2.4 GHz
      // at most. ROLE_BLE here just means "no Wi-Fi" when the plan has none.
      node.cfg.role = needWifi ? ROLE_WIFI : ROLE_BLE;
      node.cfg.ble_duty_pct = 0;
      node.cfg.channel_mask = (node.caps & CAP_WIFI5) ? plan.channel_mask : (plan.channel_mask & kMask24);
    } else if (s == bleNode) {
      node.cfg.role = ROLE_BLE;
      node.cfg.ble_duty_pct = 100;
    } else if (bleNode >= 0) {
      node.cfg.role = ROLE_WIFI;  // the BLE node covers BLE
    } else {
      node.cfg.role = !needWifi ? ROLE_BLE : (!needBle ? ROLE_WIFI : ROLE_BOTH);
      node.cfg.ble_duty_pct = plan.ble_duty_pct;
      node.cfg.channel_mask = (node.caps & CAP_WIFI5) ? plan.channel_mask : (plan.channel_mask & kMask24);
    }
  }
  if (split && needWifi) splitWifi(nodes_, online, n, plan.channel_mask);
}

const char* roleName(uint8_t role) {
  switch (role) {
    case ROLE_WIFI: return "wifi";
    case ROLE_BLE: return "ble";
    case ROLE_BOTH: return "both";
    case ROLE_ALL: return "all";
    default: return "auto";
  }
}

}  // namespace ss
