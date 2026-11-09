// Core-side node registry and work assignment. Pure C++ (shared with a future SBC master).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_channels.h"
#include "ss_devices.h"
#include "ss_proto.h"

namespace ss {

// How often a node sends its sightings to the master (it sends sooner if its batch fills).
// Was 1 s. Every send is a chance for esp-idf #19269 (a frame stuck in the Wi-Fi driver,
// node restarts), and Bluetooth now steps aside briefly for each send, which costs an S3
// part of its listening (20 pauses/min at 3 s: ~160 adverts/min vs ~400 without). 5 s:
// 12 pauses/min; sightings arrive up to 4 s later.
constexpr uint16_t kNodeReportMs = 5000;
// A dashboard (CYD) checks in with each report and gets its screen data back in reply, so
// it keeps 1 s: at 3 s its screen fell back to "Waiting for dashboard data" now and then.
constexpr uint16_t kDashReportMs = 1000;

struct NodeInfo {
  bool used = false;
  bool online = false;
  uint8_t mac[6] = {0};
  uint16_t id = 0;
  uint8_t hw = HW_UNKNOWN;
  uint8_t caps = 0;
  uint16_t fw = 0;
  uint32_t last_ms = 0;
  int8_t link_rssi = -127;   // how well the Core hears this node
  bool have_seq = false;
  uint16_t last_seq = 0;
  uint32_t frames = 0;
  uint32_t lost = 0;         // frames missing from the sequence
  Heartbeat hb;
  Config cfg;                // what we assigned
  // Set by you on the master (kept across reboots by the master, keyed by node id):
  uint8_t force_role = ROLE_AUTO;  // ROLE_BLE / ROLE_WIFI / ROLE_BOTH pin the job; ROLE_ALL = everything, on its own
  int8_t screen = -1;              // -1 follow the master's setting, 0 off, 1 on
  uint8_t lora_mode = 0;           // LoRa boards: ss_lora.h LoraMode (0 auto)
  uint8_t lora_slot = 0;           //   Meshtastic frequency slot, 1-based (0 = the preset's default)
  int8_t link_mode = -1;           // link to the master: -1 follow the master's LR setting, 0 normal,
                                   //   1 Long Range 250 kbit/s, 2 Long Range 500 kbit/s
  char label[kMaxLabel + 1] = {0};
};

// How work is spread across nodes.
enum AssignMode : uint8_t {
  ASSIGN_SPLIT = 0,   // max coverage: one node on BLE, the others split Wi-Fi channels
  ASSIGN_MIRROR = 1,  // every node runs the same plan, so RSSI can be compared (direction finding)
};

class NodeRegistry {
 public:
  // Registers (or refreshes) a node from its HELLO. Returns slot, or -1 if full.
  int add(const Hello& h, uint32_t now_ms, bool* is_new = nullptr);
  // Slot for a known MAC, or -1. Marks it seen.
  int touch(const uint8_t mac[6], uint32_t now_ms, int8_t rssi = -127);
  int find(const uint8_t mac[6]) const;
  // Sequence tracking for loss stats.
  void noteSeq(int slot, uint16_t seq);
  // Marks nodes silent for longer than timeout as offline. True if any changed.
  bool expire(uint32_t now_ms, uint32_t timeout_ms);

  // Drops a node from the list (it can join again by saying hello).
  void forget(int slot);
  // Recomputes every online node's Config. Bumps the config version.
  void assign(uint16_t category_mask, AssignMode mode);

  // Keeps a slot out of use (the master's own radio reports as slot 0).
  void reserve(int slot, bool on = true) {
    if (slot < 0 || slot >= kMaxNodes) return;
    if (on) reserved_ |= (uint8_t)(1u << slot); else reserved_ &= (uint8_t)~(1u << slot);
  }
  bool reserved(int slot) const { return (reserved_ >> slot) & 1u; }

  NodeInfo& at(int slot) { return nodes_[slot]; }
  const NodeInfo& at(int slot) const { return nodes_[slot]; }
  uint8_t onlineCount() const;
  uint8_t cfgVersion() const { return cfg_version_; }

 private:
  NodeInfo nodes_[kMaxNodes];
  uint8_t cfg_version_ = 0;
  uint8_t reserved_ = 0;
};

const char* roleName(uint8_t role);

}  // namespace ss
