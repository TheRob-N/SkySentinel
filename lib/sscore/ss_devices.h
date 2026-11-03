// Core-side device table: merges sightings from every node into logical devices,
// stitches rotating random addresses back together by fingerprint, keeps
// per-node signal strength (for sector direction later), and keeps the
// now / session counts the radar screen shows.
//
// Pure C++: runs on the V8 today and on an SBC master later.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ss_proto.h"

namespace ss {

constexpr uint8_t kMaxNodes = 8;
constexpr uint8_t kMaxClusterMacs = 16;

struct Device {
  bool used = false;
  uint32_t id = 0;           // stable logical id (survives address rotation)
  uint8_t addr[6] = {0};     // current address
  uint8_t radio = 0;         // radioClass()
  uint8_t kind = KIND_NONE;
  uint8_t category = CAT_NONE;
  uint8_t counted_cat = CAT_NONE;  // category this device was counted under in totals
  uint16_t sig_id = 0;
  uint8_t conf = 0;
  uint8_t flags = 0;
  uint16_t company = 0xFFFF;
  uint32_t fp = 0;
  uint8_t channel = 0;
  int8_t rssi_last = -127;
  int8_t rssi_max = -127;
  uint32_t first_ms = 0;     // first time this logical device was seen (any address)
  uint32_t addr_first_ms = 0;// first time the *current* address was seen
  uint32_t last_ms = 0;
  uint32_t hits = 0;
  uint8_t rotations = 0;     // address changes stitched into this device
  bool stitch_done = false;  // new-address stitch check finished
  int8_t node_rssi[kMaxNodes];
  uint32_t node_ms[kMaxNodes];
  char name[kMaxName + 1] = {0};
  // Access point details (Wi-Fi APs only; ap_sec 0 = none heard)
  uint8_t ap_sec = 0, ap_gen = 0, ap_width = 0, ap_util = 255;
  uint16_t ap_sta = 0xFFFF;
  uint32_t ap_up_s = 0, ap_up_ms = 0;   // AP uptime as of ap_up_ms (beacon clock)
  // Core-side judgements (not on the wire):
  bool spoofed = false;      // part of a BLE spam burst: one transmitter faking many devices
  bool hidden = false;       // only here for spam detection (its category is switched off)
  uint32_t alias_of = 0;     // id of the AP this BSSID belongs to (mesh / multi-SSID router)
  uint8_t aliases = 0;       // extra BSSIDs grouped under this AP
  // Following (mobile / portable): distinct places this device was heard in.
  uint16_t last_place = 0;
  uint8_t places = 0;
  bool follow_alerted = false;
};

// One transmitter faking many devices (Flipper "BLE Spam" and friends): a burst of
// new random addresses of easily spoofed types, all at nearly the same signal.
struct SpamPolicy {
  uint32_t window_ms = 5000;   // look back this far
  uint8_t min_burst = 8;       // new spoofable addresses within the window...
  int8_t rssi_band = 6;        // ...whose RSSI fits inside this many dB...
  uint8_t min_types = 2;       // ...spanning this many spoofable types (or min_burst_one_type of one)
  uint8_t min_burst_one_type = 12;
  uint32_t hold_ms = 15000;    // stay in "spam active" this long after the last burst
  int8_t sweep_db = 10;        // once found, fakes within +/- this of the burst's centre are swept up
                               // (real captures fade ~+/-7 dB around one transmitter)
};

struct StitchPolicy {
  uint32_t quiet_ms = 3000;     // old address must have been silent this long
  uint32_t handoff_ms = 4000;   // |new.first - old.last| must be within this
  uint32_t window_ms = 20000;   // only try for this long after a new address appears
  int8_t rssi_window = 12;      // dB
  int8_t margin = 6;            // dB lead needed to choose among several candidates
  // Wi-Fi probes: a phone searches in bursts every 30 s to a few minutes and picks a new
  // random address for each burst, so the new address shows up long after the old one
  // went quiet. Same fingerprint and RSSI rules, much longer hand-off.
  uint32_t probe_quiet_ms = 2000;
  uint32_t probe_handoff_ms = 180000;
};

class DeviceTable {
 public:
  enum Result : uint8_t { REJECTED = 0, NEW, UPDATED, SELF };

  DeviceTable(Device* storage, size_t capacity, StitchPolicy policy = StitchPolicy(),
              SpamPolicy spam = SpamPolicy());

  // Sightings from our own cluster (node / Core radios) are ignored.
  void addClusterMac(const uint8_t mac[6]);
  bool isClusterMac(const uint8_t mac[6]) const;

  Result ingest(uint8_t node_slot, const Sighting& s, uint32_t now_ms, Device** out = nullptr);
  // Sets the device's name and AP details. A probed network (NAME_PROBE) is not the
  // device's name: returns the device so the caller can file it (NoteRing), else null.
  Device* setName(const Name& n);

  // Run about once a second: performs address-rotation stitching.
  // Returns the number of merges done.
  size_t maintain(uint32_t now_ms);

  // Returns true (once per burst) when a BLE spam burst was detected; `out` describes it.
  bool pollSpam(Event& out);
  // Wi-Fi beacon flood (Flipper / Marauder "beacon spam", Pwnagotchi-style junk): a burst
  // of brand-new networks at nearly the same signal. They're marked spoofed like BLE spam.
  bool wifiSpamActive(uint32_t now_ms) const {
    return wspam_active_ && (int32_t)(now_ms - wspam_last_ms_) <= (int32_t)spam_pol_.hold_ms;
  }
  // Sighting times come from (now - age) per node batch, so they are not strictly
  // increasing: compare as signed differences.
  bool spamActive(uint32_t now_ms) const {
    return spam_active_ && (int32_t)(now_ms - spam_last_ms_) <= (int32_t)spam_pol_.hold_ms;
  }
  uint32_t spoofedTotal() const { return spoofed_; }

  Device* find(const uint8_t addr[6], uint8_t radio);
  Device* byId(uint32_t id);

  size_t countNow(uint8_t category, uint32_t now_ms, uint32_t window_ms = 30000) const;
  size_t countNowAll(uint32_t now_ms, uint32_t window_ms = 30000) const;
  uint32_t total(uint8_t category) const { return category < CAT_COUNT ? totals_[category] : 0; }
  uint32_t totalAll() const;
  size_t used() const;
  size_t capacity() const { return cap_; }
  uint32_t stitches() const { return stitches_; }

  template <typename F>
  void forEach(F f) const {
    for (size_t i = 0; i < cap_; ++i) if (dev_[i].used) f(dev_[i]);
  }
  template <typename F>
  void forEachMutable(F f) {
    for (size_t i = 0; i < cap_; ++i) if (dev_[i].used) f(dev_[i]);
  }

 private:
  Device* alloc(uint32_t now_ms);
  void retire(Device& d);
  void merge(Device& older, Device& newer);
  bool stitchable(const Device& d) const;
  void uncount(Device& d);
  void groupAccessPoint(Device& d);
  void noteSpoofable(Device& d, uint32_t now_ms);
  void collapseFlipper(Device& d, uint32_t now_ms);

  struct SpamHit { uint32_t id; uint32_t ms; int8_t rssi; uint16_t sig; };
  void noteNewNetwork(Device& d, uint32_t now_ms);
  static constexpr uint8_t kWspamRing = 40;
  SpamHit wspam_[kWspamRing] = {};
  uint8_t wspam_head_ = 0;
  bool wspam_active_ = false, wspam_pending_ = false;
  uint32_t wspam_last_ms_ = 0;
  int8_t wspam_lo_ = 0, wspam_hi_ = 0;
  Event wspam_event_{};
  static constexpr uint8_t kSpamRing = 48;

  Device* dev_;
  size_t cap_;
  StitchPolicy pol_;
  uint32_t next_id_ = 1;
  uint32_t totals_[CAT_COUNT] = {0};
  uint32_t stitches_ = 0;
  uint8_t cluster_[kMaxClusterMacs][6] = {};
  uint8_t n_cluster_ = 0;
  SpamPolicy spam_pol_;
  SpamHit spam_[kSpamRing] = {};
  uint8_t spam_head_ = 0;
  bool spam_active_ = false;
  uint32_t spam_last_ms_ = 0;
  int8_t spam_lo_ = 0, spam_hi_ = 0;  // RSSI band of the current burst
  bool spam_pending_ = false;
  Event spam_event_{};
  uint32_t spoofed_ = 0;
};

// Signatures a phone-sized gadget can fake to trigger pairing popups.
bool isSpoofableSig(uint16_t sig_id);

// Short texts filed by device id, newest kept: the networks a phone asked for by name
// (its probe requests) and an access point's WPS make / model. Memory only, never logged.
template <size_t N>
class NoteRing {
 public:
  enum : uint8_t { TAG_PROBE = 1, TAG_WPS = 2, TAG_POS = 3, TAG_LINK = 4, TAG_BATT = 5, TAG_MESH = 6, TAG_RID = 7, TAG_MOVE = 8, TAG_OP = 9, TAG_TEXT = 10 };
  // unique: one note per (id, tag), replaced. Otherwise one per (id, tag, text).
  void add(uint32_t id, uint8_t tag, const char* text, uint32_t now_ms, bool unique = false) {
    if (!text || !text[0]) return;
    size_t match = N, empty = N, oldest = 0;
    for (size_t i = 0; i < N; ++i) {
      const E& e = e_[i];
      if (e.id == id && e.tag == tag && (unique || strncmp(e.text, text, sizeof(e.text)) == 0)) { match = i; break; }
      if (!e.id) { if (empty == N) empty = i; continue; }
      if (now_ms - e.ms > now_ms - e_[oldest].ms) oldest = i;
    }
    const size_t slot = match != N ? match : empty != N ? empty : oldest;
    E& e = e_[slot];
    e.id = id; e.tag = tag; e.ms = now_ms;
    strncpy(e.text, text, sizeof(e.text) - 1);
    e.text[sizeof(e.text) - 1] = '\0';
  }
  // Calls fn(text, age_ms) for each note on this device / tag.
  template <typename F>
  void forEach(uint32_t id, uint8_t tag, uint32_t now_ms, F fn) const {
    for (size_t i = 0; i < N; ++i)
      if (e_[i].id == id && e_[i].tag == tag) fn(e_[i].text, now_ms - e_[i].ms);
  }
  // Every note with this tag: fn(device id, text, age_ms).
  template <typename F>
  void forEachTag(uint8_t tag, uint32_t now_ms, F fn) const {
    for (size_t i = 0; i < N; ++i)
      if (e_[i].id && e_[i].tag == tag) fn(e_[i].id, e_[i].text, now_ms - e_[i].ms);
  }
  void clear() { for (auto& e : e_) e = E(); }
  void clearTag(uint8_t tag) { for (auto& e : e_) if (e.tag == tag) e = E(); }

 private:
  struct E { uint32_t id = 0; uint8_t tag = 0; uint32_t ms = 0; char text[kMaxName + 1] = {0}; };
  E e_[N];
};

}  // namespace ss
