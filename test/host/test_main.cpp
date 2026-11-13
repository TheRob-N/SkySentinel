// Host-side tests for lib/sscore. Build and run:  make -C test/host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>
#include <algorithm>

#include "ss_batch.h"
#include "ss_channels.h"
#include "ss_classify.h"
#include "ss_devices.h"
#include "ss_nodes.h"
#include "ss_lora.h"
#include "ss_subghz.h"
#include "ss_follow.h"
#include "ss_dash.h"
#include "ss_crypto.h"
#include "ss_gps.h"
#include "ss_log.h"
#include "ss_pair.h"
#include "ss_touch.h"
#include "ss_proto.h"
#include "ss_version.h"
#include "ss_fileserve.h"
#include "ss_linkchan.h"

using namespace ss;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (cond) { ++g_pass; }                                                  \
    else { ++g_fail; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

// ---------------------------------------------------------------------------
// helpers

struct Adv {
  std::vector<uint8_t> b;
  Adv& ad(uint8_t type, std::initializer_list<uint8_t> data) {
    b.push_back((uint8_t)(data.size() + 1));
    b.push_back(type);
    b.insert(b.end(), data.begin(), data.end());
    return *this;
  }
  Adv& name(const char* n) {
    b.push_back((uint8_t)(strlen(n) + 1));
    b.push_back(0x09);
    b.insert(b.end(), n, n + strlen(n));
    return *this;
  }
  Adv& mfg(uint16_t company, std::vector<uint8_t> data) {
    b.push_back((uint8_t)(data.size() + 3));
    b.push_back(0xFF);
    b.push_back((uint8_t)company);
    b.push_back((uint8_t)(company >> 8));
    b.insert(b.end(), data.begin(), data.end());
    return *this;
  }
};

static BleAdv parseAdv(const Adv& adv, std::initializer_list<uint8_t> addr, uint8_t type, int8_t rssi = -60) {
  BleAdv a;
  memcpy(a.addr, addr.begin(), 6);
  a.addr_type = type;
  a.rssi = rssi;
  parseBleAdv(adv.b.data(), adv.b.size(), a);
  return a;
}

static std::vector<uint8_t> appleData(uint8_t type, uint8_t len, uint8_t fill) {
  std::vector<uint8_t> v{type, len};
  for (uint8_t i = 0; i < len; ++i) v.push_back((uint8_t)(fill + i * 7));
  return v;
}

struct Frame {
  std::vector<uint8_t> b;
  Frame(uint8_t subtype, std::initializer_list<uint8_t> a1, std::initializer_list<uint8_t> a2, uint16_t seq = 0) {
    b.push_back((uint8_t)(subtype << 4));  // type 0 (mgmt)
    b.push_back(0);
    b.push_back(0); b.push_back(0);
    b.insert(b.end(), a1.begin(), a1.end());
    b.insert(b.end(), a2.begin(), a2.end());
    b.insert(b.end(), a2.begin(), a2.end());  // bssid = transmitter
    b.push_back((uint8_t)(seq << 4)); b.push_back((uint8_t)(seq >> 4));
    if (subtype == 8 || subtype == 5) for (int i = 0; i < 12; ++i) b.push_back(0);  // fixed fields
  }
  Frame& ie(uint8_t id, std::vector<uint8_t> d) {
    b.push_back(id); b.push_back((uint8_t)d.size());
    b.insert(b.end(), d.begin(), d.end());
    return *this;
  }
  Frame& ssid(const char* s) { return ie(0, std::vector<uint8_t>(s, s + strlen(s))); }
};

static const std::initializer_list<uint8_t> kBcast = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------------------

static void testCrcAndFrames() {
  puts("crc + frames");
  CHECK_EQ(crc16((const uint8_t*)"123456789", 9), 0x29B1);

  uint8_t frame[kMaxFrame], payload[kMaxPayload];
  Hello h;
  const uint8_t mac[6] = {0x24, 0x58, 0x7C, 0x11, 0x22, 0x33};
  memcpy(h.mac, mac, 6);
  h.fw_version = 0x0102; h.hw = HW_C5_DEVKIT; h.caps = CAP_WIFI24 | CAP_WIFI5 | CAP_BLE; h.role_pref = ROLE_BLE;
  size_t pl = 0;
  h.auth = 0xA1B2C3D4;
  CHECK_EQ(helloAuthInput(h, payload, sizeof(payload)), 11u);
  pl = encodeHello(h, payload, sizeof(payload));
  CHECK_EQ(pl, 15u);
  size_t fl = encodeFrame(MSG_HELLO, nodeIdFromMac(mac), 77, payload, pl, frame, sizeof(frame));
  CHECK_EQ(fl, kHeaderSize + 15 + kCrcSize);

  Header hdr;
  const uint8_t* p = nullptr;
  size_t n = 0;
  CHECK(decodeFrame(frame, fl, hdr, &p, &n));
  CHECK_EQ(hdr.type, MSG_HELLO);
  CHECK_EQ(hdr.seq, 77);
  CHECK_EQ(hdr.node_id, 0x2233);
  Hello h2;
  CHECK(decodeHello(p, n, h2));
  CHECK(memcmp(h2.mac, mac, 6) == 0);
  CHECK_EQ(h2.fw_version, 0x0102);
  CHECK_EQ(h2.role_pref, ROLE_BLE);
  CHECK_EQ(h2.auth, 0xA1B2C3D4u);

  frame[10] ^= 0x01;  // corrupt payload
  CHECK(!decodeFrame(frame, fl, hdr, &p, &n));
  frame[10] ^= 0x01;
  frame[0] = 0x00;    // bad magic (CRC also wrong)
  CHECK(!decodeFrame(frame, fl, hdr, &p, &n));
  CHECK(!decodeFrame(frame, 5, hdr, &p, &n));

  Config c;
  c.cfg_version = 9; c.role = ROLE_WIFI; c.category_mask = kSurveillanceMask;
  c.channel_mask = kMaskAll; c.dwell_ms = 150; c.report_ms = 800; c.rssi_floor = -88; c.ble_duty_pct = 35;
  c.node_flags = NF_DISPLAY_OFF;
  pl = encodeConfig(c, payload, sizeof(payload));
  CHECK_EQ(pl, 22u);  // 18 + node_flags + empty name + LoRa mode/slot
  Config c2;
  CHECK(decodeConfig(payload, pl, c2));
  CHECK_EQ(c2.node_flags, NF_DISPLAY_OFF);
  {
    Config named = c;
    strcpy(named.label, "Truck rear");
    uint8_t nb[kMaxConfigSize];
    const size_t nl = encodeConfig(named, nb, sizeof(nb));
    CHECK_EQ(nl, 32u);                                       // 18 + flags + len + 10 + LoRa 2
    Config back;
    CHECK(decodeConfig(nb, nl, back));
    CHECK(strcmp(back.label, "Truck rear") == 0 && back.node_flags == NF_DISPLAY_OFF);
    CHECK(!decodeConfig(nb, nl - 3, back));                  // name cut short
    Config longest = c;
    memset(longest.label, 'x', kMaxLabel);
    CHECK_EQ(encodeConfig(longest, nb, sizeof(nb)), kMaxConfigSize);
  }
  {
    Config old;  // 18-byte config from an older master: flags default to 0
    CHECK(decodeConfig(payload, 18, old));
    CHECK_EQ(old.node_flags, 0);
    CHECK_EQ(old.ble_duty_pct, 35);
  }
  CHECK_EQ(c2.channel_mask, kMaskAll);
  CHECK_EQ(c2.rssi_floor, -88);
  CHECK_EQ(c2.category_mask, kSurveillanceMask);
  CHECK(!decodeConfig(payload, 17, c2));  // truncated

  {
    NodeCommand cmd;
    cmd.cmd = NC_LABEL; cmd.arg = 0;
    strcpy(cmd.text, "Truck rear"); cmd.len = 10;
    uint8_t cb[32];
    const size_t cl = encodeCommand(cmd, cb, sizeof(cb));
    CHECK_EQ(cl, 13u);
    NodeCommand back;
    CHECK(decodeCommand(cb, cl, back));
    CHECK(back.cmd == NC_LABEL && back.len == 10 && strcmp(back.text, "Truck rear") == 0);
    CHECK(!decodeCommand(cb, cl - 1, back));              // truncated text
    cb[2] = 99;                                           // length beyond the limit
    CHECK(!decodeCommand(cb, cl, back));
  }

  Heartbeat hb;
  hb.uptime_s = 123456; hb.cfg_version = 9; hb.channel = 149; hb.dropped = 3; hb.wifi_frames = 1000000; hb.ble_adv = 42;
  hb.gps = 4; hb.gps_sats = 11;
  hb.reset = RW_CRASH; hb.heap_kb = 143; hb.heap_min_kb = 61; hb.losses = 3;
  hb.has_pos = true; hb.lat_e7 = 417443636; hb.lon_e7 = -773006866; hb.utc = 1793376000; hb.sd = SD_LOGGING;
  hb.tx_wait_ms = 1730; hb.stuck_ms = 5012; hb.wants = HBW_ALERTS;
  hb.release = ssReleaseCode("2.1.3"); hb.build = 2611110843u;
  pl = encodeHeartbeat(hb, payload, sizeof(payload));
  CHECK_EQ(pl, 53u);
  Heartbeat hb2;
  CHECK(decodeHeartbeat(payload, pl, hb2));
  CHECK(hb2.tx_wait_ms == 1730 && hb2.stuck_ms == 5012 && hb2.wants == HBW_ALERTS);
  CHECK(hb2.release == ((2 << 10) | (1 << 5) | 3) && hb2.build == 2611110843u);
  CHECK_EQ(ssReleaseCode("2.10.0"), (2 << 10) | (10 << 5));
  CHECK_EQ(ssReleaseCode("3.0"), 3 << 10);
  {  // a node from this morning stops after `wants`: no firmware fields
    Heartbeat mid;
    CHECK(decodeHeartbeat(payload, 47, mid));
    CHECK(mid.wants == HBW_ALERTS && mid.release == 0 && mid.build == 0);
  }
  {  // a 2.1.2 node stops after the link byte: the driver fields and wants read as 0
    Heartbeat old;
    CHECK(decodeHeartbeat(payload, 42, old));
    CHECK(old.sd == SD_LOGGING && old.tx_wait_ms == 0 && old.stuck_ms == 0 && old.wants == 0);
  }
  CHECK(hb2.has_pos && hb2.lat_e7 == 417443636 && hb2.lon_e7 == -773006866 && hb2.utc == 1793376000u);
  CHECK(hb2.sd == SD_LOGGING && strcmp(sdStateName(hb2.sd), "logging") == 0);
  {
    Heartbeat h27;                                        // last week's nodes: health, no position
    CHECK(decodeHeartbeat(payload, 27, h27));
    CHECK(!h27.has_pos && h27.utc == 0 && h27.sd == SD_NONE && h27.losses == 3);
  }
  CHECK(hb2.gps == 4 && hb2.gps_sats == 11);
  CHECK(hb2.reset == RW_CRASH && hb2.heap_kb == 143 && hb2.heap_min_kb == 61 && hb2.losses == 3);
  CHECK(strcmp(resetWhyName(RW_NO_MASTER), "no_master") == 0);
  {
    Heartbeat mid;                                        // a node from yesterday: GPS, no health
    CHECK(decodeHeartbeat(payload, 20, mid));
    CHECK(mid.gps == 4 && mid.reset == 0 && mid.losses == 0);
  }
  {
    Heartbeat old;                                        // an older node: no GPS bytes
    CHECK(decodeHeartbeat(payload, 18, old));
    CHECK(old.gps == 0 && old.gps_sats == 0);
    CHECK(!decodeHeartbeat(payload, 17, old));
  }
  CHECK_EQ(hb2.wifi_frames, 1000000u);
  CHECK_EQ(hb2.channel, 149);
  CHECK_EQ(hb2.temp_c, -128);

  Name nm;
  memcpy(nm.addr, mac, 6); nm.kind = KIND_BLE_ADV; nm.len = 9; memcpy(nm.name, "Axon Body", 9);
  pl = encodeName(nm, payload, sizeof(payload));
  Name nm2;
  CHECK(decodeName(payload, pl, nm2));
  CHECK(strcmp(nm2.name, "Axon Body") == 0);
  CHECK(nm2.what == NAME_DEVICE && !nm2.has_ap);
  {
    Name ap;
    memcpy(ap.addr, mac, 6); ap.kind = KIND_WIFI_BEACON; ap.len = 4; memcpy(ap.name, "Home", 4);
    ap.has_ap = true; ap.ap.sec = SEC_WPA3; ap.ap.gen = 6; ap.ap.width = 80; ap.ap.util = 40; ap.ap.sta = 12;
    strcpy(ap.ap.wps, "NETGEAR R7000");
    ap.ap.up_s = 259200;
    uint8_t b[kMaxPayload];
    size_t l = encodeName(ap, b, sizeof(b));
    Name ap2;
    CHECK(decodeName(b, l, ap2));
    CHECK(ap2.has_ap && ap2.ap.sec == SEC_WPA3 && ap2.ap.gen == 6 && ap2.ap.width == 80 && ap2.ap.sta == 12 && ap2.ap.util == 40);
    CHECK(strcmp(ap2.ap.wps, "NETGEAR R7000") == 0 && strcmp(ap2.name, "Home") == 0);
    CHECK_EQ(ap2.ap.up_s, 259200u);
    CHECK(!decodeName(b, l - 8, ap2));   // cut inside the WPS text
    Name pr;
    memcpy(pr.addr, mac, 6); pr.kind = KIND_WIFI_PROBE_REQ; pr.len = 7; memcpy(pr.name, "Starbux", 7); pr.what = NAME_PROBE;
    l = encodeName(pr, b, sizeof(b));
    CHECK(decodeName(b, l, ap2));
    CHECK(ap2.what == NAME_PROBE && !ap2.has_ap && strcmp(ap2.name, "Starbux") == 0);
  }
  payload[7] = 200;  // bogus length
  CHECK(!decodeName(payload, pl, nm2));

  {
    NoteRing<4> ring;
    ring.add(7, NoteRing<4>::TAG_PROBE, "Home", 1000);
    ring.add(7, NoteRing<4>::TAG_PROBE, "Work", 2000);
    ring.add(7, NoteRing<4>::TAG_PROBE, "Home", 3000);   // refresh, no duplicate
    ring.add(7, NoteRing<4>::TAG_WPS, "TP-Link", 3000, true);
    ring.add(7, NoteRing<4>::TAG_WPS, "TP-Link Archer", 3100, true);   // replaces
    int n = 0, w = 0;
    ring.forEach(7, NoteRing<4>::TAG_PROBE, 4000, [&](const char*, uint32_t) { ++n; });
    ring.forEach(7, NoteRing<4>::TAG_WPS, 4000, [&](const char* t, uint32_t) { ++w; CHECK(strcmp(t, "TP-Link Archer") == 0); });
    CHECK(n == 2 && w == 1);
    ring.add(9, NoteRing<4>::TAG_PROBE, "A", 5000);
    ring.add(9, NoteRing<4>::TAG_PROBE, "B", 5001);   // full: drops the oldest ("Work" at 2000)
    n = 0;
    ring.forEach(7, NoteRing<4>::TAG_PROBE, 6000, [&](const char* t, uint32_t) { ++n; CHECK(strcmp(t, "Home") == 0); });
    CHECK_EQ(n, 1);
  }

  Event ev;
  ev.type = EV_DEAUTH_FLOOD; ev.channel = 11; ev.count = 300;
  pl = encodeEvent(ev, payload, sizeof(payload));
  CHECK_EQ(pl, 22u);
  Event ev2;
  CHECK(decodeEvent(payload, pl, ev2));
  CHECK_EQ(ev2.count, 300);

  Welcome w;
  memcpy(w.core_mac, mac, 6); w.node_index = 3; w.core_ms = 99999; w.auth = 7;
  CHECK_EQ(welcomeAuthInput(w, mac, payload, sizeof(payload)), 17u);
  pl = encodeWelcome(w, payload, sizeof(payload));
  CHECK_EQ(pl, 15u);
  Welcome w2;
  CHECK(decodeWelcome(payload, pl, w2));
  CHECK_EQ(w2.core_ms, 99999u);
  CHECK_EQ(w2.auth, 7u);
}

static void testSightings() {
  puts("sightings codec");
  CHECK_EQ(kMaxSightingsPerFrame, 9u);
  Sighting s[10];
  for (int i = 0; i < 10; ++i) {
    s[i].addr[5] = (uint8_t)i; s[i].kind = KIND_BLE_ADV; s[i].rssi_max = (int8_t)(-40 - i);
    s[i].rssi_last = (int8_t)(-50 - i); s[i].hits = (uint8_t)(i + 1); s[i].category = CAT_TRACKER;
    s[i].sig_id = SIG_TILE; s[i].conf = 85; s[i].company = 0x004C; s[i].fp = 0xDEADBEEF + i; s[i].age_ms = 1234;
  }
  uint8_t payload[kMaxPayload];
  CHECK_EQ(encodeSightings(s, 10, payload, sizeof(payload)), 0u);
  const size_t pl = encodeSightings(s, 9, payload, sizeof(payload));
  CHECK_EQ(pl, 1 + 9 * kSightingSize);
  uint8_t frame[kMaxFrame];
  CHECK(encodeFrame(MSG_SIGHTINGS, 1, 2, payload, pl, frame, sizeof(frame)) > 0);

  Sighting out[9];
  size_t count = 0;
  CHECK(decodeSightings(payload, pl, out, 9, &count));
  CHECK_EQ(count, 9u);
  CHECK_EQ(out[8].addr[5], 8);
  CHECK_EQ(out[8].rssi_max, -48);
  CHECK_EQ(out[8].fp, 0xDEADBEEFu + 8);
  CHECK_EQ(out[8].age_ms, 1234);
  CHECK(!decodeSightings(payload, pl - 3, out, 9, &count));  // truncated
}

static void testCobs() {
  puts("cobs");
  srand(7);
  for (int trial = 0; trial < 500; ++trial) {
    uint8_t in[kMaxFrame], enc[cobsMaxEncoded(kMaxFrame)], dec[kMaxFrame];
    const size_t n = 1 + (size_t)(rand() % kMaxFrame);
    for (size_t i = 0; i < n; ++i) in[i] = (uint8_t)(trial % 3 == 0 ? (rand() % 2) : rand());  // lots of zeros
    const size_t e = cobsEncode(in, n, enc, sizeof(enc));
    bool nozero = e > 0;
    for (size_t i = 0; i < e; ++i) nozero = nozero && enc[i] != 0;
    CHECK(nozero);
    const size_t d = cobsDecode(enc, e, dec, sizeof(dec));
    CHECK(d == n && memcmp(in, dec, n) == 0);
  }
  // 254+ non-zero bytes (block boundary)
  uint8_t in[250], enc[cobsMaxEncoded(250)], dec[250];
  for (int i = 0; i < 250; ++i) in[i] = 0xAA;
  size_t e = cobsEncode(in, 250, enc, sizeof(enc));
  CHECK_EQ(cobsDecode(enc, e, dec, sizeof(dec)), 250u);

  // Stream: garbage, two frames, a corrupt frame, another frame.
  CobsDeframer df;
  std::vector<uint8_t> stream = {0x13, 0x37, 0x00};
  uint8_t f1[] = {1, 0, 2, 0, 0, 3};
  uint8_t f2[] = {0, 0, 0};
  e = cobsEncode(f1, sizeof(f1), enc, sizeof(enc)); stream.insert(stream.end(), enc, enc + e); stream.push_back(0);
  e = cobsEncode(f2, sizeof(f2), enc, sizeof(enc)); stream.insert(stream.end(), enc, enc + e); stream.push_back(0);
  stream.push_back(0x05); stream.push_back(0x01); stream.push_back(0);  // code says 4 bytes, only 1: corrupt
  e = cobsEncode(f1, sizeof(f1), enc, sizeof(enc)); stream.insert(stream.end(), enc, enc + e); stream.push_back(0);
  int frames = 0;
  for (uint8_t b : stream) {
    if (df.feed(b)) {
      ++frames;
      if (frames == 2) CHECK(df.frameLen() == 3 && df.frame()[0] == 0);
      else CHECK(df.frameLen() == sizeof(f1) && memcmp(df.frame(), f1, sizeof(f1)) == 0);
    }
  }
  CHECK_EQ(frames, 3);
  CHECK(df.errors() >= 1);
}

static void testChannels() {
  puts("channels + scan plans");
  CHECK_EQ(channelCount(kMaskAll), 38);
  CHECK_EQ(channelCount(kMask24), 13);
  CHECK_EQ(channelIndex(6), 5);
  CHECK_EQ(channelIndex(165), 37);
  CHECK_EQ(channelIndex(14), -1);

  ScanPlan p = planForCategories(catBit(CAT_BODYCAM));
  CHECK_EQ(p.channel_mask, 0u);
  CHECK_EQ(p.ble_duty_pct, 100);
  p = planForCategories(catBit(CAT_BODYCAM) | catBit(CAT_TRACKER) | catBit(CAT_AUDIO));
  CHECK_EQ(p.channel_mask, 0u);
  p = planForCategories(catBit(CAT_DRONE));
  CHECK_EQ(p.channel_mask, kMaskCh6);
  CHECK_EQ(p.ble_duty_pct, 70);
  CHECK_EQ(p.dwell_ms, 1000);
  p = planForCategories(catBit(CAT_FLOCK) | catBit(CAT_DRONE));
  CHECK_EQ(p.channel_mask, kMask24);
  CHECK_EQ(p.ble_duty_pct, 50);
  p = planForCategories(kAllCategoriesMask);
  CHECK_EQ(p.channel_mask, kMaskAll);
  CHECK_EQ(p.ble_duty_pct, 35);
  p = planForCategories(catBit(CAT_OTHER_WIFI));
  CHECK_EQ(p.ble_duty_pct, 0);
  p = planForCategories(catBit(CAT_LORA));
  CHECK(p.channel_mask == 0 && p.ble_duty_pct == 0);
  p = planForCategories(0);
  CHECK(p.channel_mask == 0 && p.ble_duty_pct == 0);

  for (uint8_t nodes = 1; nodes <= 6; ++nodes) {
    uint64_t uni = 0;
    bool disjoint = true;
    for (uint8_t i = 0; i < nodes; ++i) {
      const uint64_t m = splitChannels(kMaskAll, nodes, i);
      disjoint = disjoint && (uni & m) == 0;
      uni |= m;
    }
    CHECK(disjoint);
    CHECK_EQ(uni, kMaskAll);
  }
  // Each of 2 nodes gets some 2.4 and some 5 GHz
  CHECK((splitChannels(kMaskAll, 2, 0) & kMask24) && (splitChannels(kMaskAll, 2, 0) & kMask5));
  CHECK((splitChannels(kMaskAll, 2, 1) & kMask24) && (splitChannels(kMaskAll, 2, 1) & kMask5));
  // Fewer channels than nodes: nobody idles
  CHECK_EQ(splitChannels(kMaskCh6, 3, 2), kMaskCh6);

  uint8_t cur = kChannelCount - 1, seen = 0;
  uint64_t visited = 0;
  for (int i = 0; i < 13; ++i) { const uint8_t ch = nextChannel(kMask24, &cur); visited |= 1ull << channelIndex(ch); ++seen; }
  CHECK_EQ(visited, kMask24);
  CHECK_EQ(nextChannel(0, &cur), 0);
}

static void testBle() {
  puts("ble parse + classify");
  // AirTag-style Find My, separated from owner
  Adv tag; tag.mfg(0x004C, appleData(0x12, 0x19, 3));
  BleAdv a = parseAdv(tag, {0xD1, 0x22, 0x33, 0x44, 0x55, 0x66}, SS_ADDR_RANDOM);
  Match m = classifyBle(a);
  CHECK_EQ(m.category, CAT_TRACKER);
  CHECK_EQ(m.sig_id, SIG_FINDMY_SEPARATED);
  CHECK(m.strong);

  Adv near; near.mfg(0x004C, appleData(0x12, 0x02, 3));
  CHECK_EQ(classifyBle(parseAdv(near, {0x51, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_FINDMY_NEAR_OWNER);

  Adv pods; pods.ad(0x01, {0x1A}).mfg(0x004C, appleData(0x07, 0x19, 9));
  m = classifyBle(parseAdv(pods, {0x71, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
  CHECK_EQ(m.category, CAT_AUDIO);
  CHECK_EQ(m.sig_id, SIG_APPLE_AUDIO);

  {
    // Model names: AirPods Pro 2 (0x1420) in a proximity-pairing advert
    Adv pro2; pro2.mfg(0x004C, {0x07, 0x19, 0x01, 0x14, 0x20, 0x55, 0x31, 0x00});
    BleAdv pa = parseAdv(pro2, {0x71, 1, 2, 3, 4, 6}, SS_ADDR_RANDOM);
    char nm[kMaxName + 1];
    CHECK_EQ(bleModelName(pa, nm), 21);
    CHECK(strcmp(nm, "AirPods Pro (2nd gen)") == 0);
    Adv unknownModel; unknownModel.mfg(0x004C, {0x07, 0x19, 0x01, 0x77, 0x77});
    CHECK_EQ(bleModelName(parseAdv(unknownModel, {0x71, 1, 2, 3, 4, 7}, SS_ADDR_RANDOM), nm), 0);
    CHECK_EQ(bleModelName(parseAdv(tag, {0xD1, 0x22, 0x33, 0x44, 0x55, 0x66}, SS_ADDR_RANDOM), nm), 0);

    // Self-reported type (GAP appearance 0x0941 earbud, 0x0943 headphones, 0x00C2 smartwatch)
    Adv buds; buds.ad(0x19, {0x41, 0x09}).mfg(0x0075, {1, 2, 3});  // Samsung company, says earbuds
    m = classifyBle(parseAdv(buds, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
    CHECK(m.category == CAT_AUDIO && m.sig_id == SIG_SAYS_EARBUDS);
    Adv cans; cans.ad(0x19, {0x43, 0x09});
    CHECK_EQ(classifyBle(parseAdv(cans, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_SAYS_HEADPHONES);
    Adv watch; watch.ad(0x19, {0xC2, 0x00});
    m = classifyBle(parseAdv(watch, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
    CHECK(m.category == CAT_WEARABLE && m.sig_id == SIG_SAYS_WATCH);
    Adv kbd; kbd.ad(0x19, {0xC1, 0x03});  // 0x03C1 keyboard
    CHECK_EQ(classifyBle(parseAdv(kbd, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_SAYS_INPUT);
    // Appearance is part of the fingerprint (earbuds vs headphones of the same make differ)
    CHECK(parseAdv(buds, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM).fp != parseAdv(cans, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM).fp);

    // LE Audio (Published Audio Capabilities 0x1850)
    Adv lea; lea.ad(0x03, {0x50, 0x18});
    CHECK_EQ(classifyBle(parseAdv(lea, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_LE_AUDIO);

    // Swift Pair, LE only: name right after id/sub/rssi
    Adv sp0; sp0.mfg(0x0006, {0x03, 0x00, 0x80, 'J', 'a', 'b', 'r', 'a'});
    BleAdv s0 = parseAdv(sp0, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM);
    CHECK_EQ(classifyBle(s0).sig_id, SIG_SWIFT_PAIR);
    CHECK_EQ(bleModelName(s0, nm), 5);
    CHECK(strcmp(nm, "Jabra") == 0);
    // Swift Pair with Classic: BR/EDR address, Class of Device 0x240404 (audio/video, headset), name
    Adv sp1; sp1.mfg(0x0006, {0x03, 0x01, 0x80, 1, 2, 3, 4, 5, 6, 0x04, 0x04, 0x24, 'W', 'H', '-', '1', '0', '0', '0'});
    BleAdv s1 = parseAdv(sp1, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM);
    m = classifyBle(s1);
    CHECK(m.category == CAT_AUDIO && m.sig_id == SIG_SWIFT_PAIR_AUDIO);
    CHECK_EQ(bleModelName(s1, nm), 7);
    CHECK(strcmp(nm, "WH-1000") == 0);
    // Non-text where the name should be: no name rather than garbage
    Adv spBad; spBad.mfg(0x0006, {0x03, 0x00, 0x80, 0x01, 0x02});
    CHECK_EQ(bleModelName(parseAdv(spBad, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM), nm), 0);
    // Filed by maker: Raycon (0x10AE) = audio, Oura (0x02B2) = wearable, unknown maker = other
    Adv ray; ray.mfg(0x10AE, {1, 2});
    m = classifyBle(parseAdv(ray, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
    CHECK(m.category == CAT_AUDIO && m.sig_id == SIG_AUDIO_MAKER);
    Adv ring; ring.mfg(0x02B2, {1, 2});
    m = classifyBle(parseAdv(ring, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
    CHECK(m.category == CAT_WEARABLE && m.sig_id == SIG_WEARABLE_MAKER);
    Adv other; other.mfg(0x5A00, {1, 2});
    CHECK_EQ(classifyBle(parseAdv(other, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_GENERIC_BLE);
    // A device that says what it is beats a maker guess
    Adv rayWatch; rayWatch.ad(0x19, {0xC2, 0x00}).mfg(0x10AE, {1});
    CHECK_EQ(classifyBle(parseAdv(rayWatch, {0x55, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_SAYS_WATCH);
    // Classic Bluetooth by Class of Device (major class << 8 | minor << 2)
    CHECK_EQ(classifyClassic(0x240404, "").sig_id, SIG_CLASSIC_HEADSET);       // audio, wearable headset
    CHECK_EQ(classifyClassic(0x240418, "Raycon E25").sig_id, SIG_CLASSIC_HEADPHONES);  // minor 6
    CHECK_EQ(classifyClassic(0x240418, "").category, CAT_AUDIO);
    CHECK_EQ(classifyClassic(0x200420, "").sig_id, SIG_CAR_STEREO);     // minor 8
    CHECK_EQ(classifyClassic(0x5A020C, "Pixel").category, CAT_PHONE);          // phone, smartphone
    CHECK_EQ(classifyClassic(0x000704, "").category, CAT_WEARABLE);            // wearable, watch
    CHECK_EQ(classifyClassic(0x1F00, "").sig_id, SIG_CLASSIC_DEVICE);          // uncategorized
    // Skimmer radios: HC-05 / HC-06 default names, whatever class they claim
    m = classifyClassic(0x001F00, "HC-05");
    CHECK(m.category == CAT_SKIMMER && m.sig_id == SIG_CLASSIC_SERIAL);
    CHECK_EQ(classifyClassic(0x000000, "linvor").category, CAT_SKIMMER);
    {
      const uint8_t mac[6] = {0x98, 0xD3, 0x31, 0xF5, 0x12, 0x34};
      const Sighting cs = makeClassicSighting(mac, classifyClassic(0x240418, ""), -61);
      CHECK_EQ(cs.kind, KIND_BT_CLASSIC);
      CHECK_EQ(cs.flags & SF_RANDOM_ADDR, 0);
      CHECK_EQ(radioClass(cs.kind), 3);
    }
    // Swift Pair is fakeable (Flipper "Windows" spam), so it counts toward spam bursts
    CHECK(isSpoofableSig(SIG_SWIFT_PAIR) && isSpoofableSig(SIG_SWIFT_PAIR_AUDIO));
  }

  Adv phone1; phone1.ad(0x01, {0x1A}).mfg(0x004C, appleData(0x10, 0x05, 1));
  Adv phone2; phone2.ad(0x01, {0x1A}).mfg(0x004C, appleData(0x10, 0x05, 200));  // rolling bytes differ
  BleAdv p1 = parseAdv(phone1, {0x41, 9, 9, 9, 9, 1}, SS_ADDR_RANDOM);
  BleAdv p2 = parseAdv(phone2, {0x7E, 8, 8, 8, 8, 2}, SS_ADDR_RANDOM);
  CHECK_EQ(classifyBle(p1).category, CAT_PHONE);
  CHECK_EQ(p1.fp, p2.fp);                                       // stable across rotation
  CHECK(p1.fp != parseAdv(pods, {0x41, 9, 9, 9, 9, 1}, SS_ADDR_RANDOM).fp);  // differs by model

  Adv smart; smart.ad(0x16, {0x5A, 0xFD, 0x01, 0x02});
  CHECK_EQ(classifyBle(parseAdv(smart, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_SMARTTAG);
  Adv tile; tile.ad(0x03, {0xED, 0xFE});
  CHECK_EQ(classifyBle(parseAdv(tile, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_TILE);
  Adv fmdn; fmdn.ad(0x16, {0xAA, 0xFE, 0x41, 0x00});
  CHECK_EQ(classifyBle(parseAdv(fmdn, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_GOOGLE_FMDN);
  Adv tile2; tile2.ad(0x03, {0x84, 0xFD});
  CHECK_EQ(classifyBle(parseAdv(tile2, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_TILE);
  Adv chip1; chip1.ad(0x03, {0x33, 0xFE});
  CHECK_EQ(classifyBle(parseAdv(chip1, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_CHIPOLO);
  Adv chip2; chip2.ad(0x16, {0x65, 0xFE, 0x01, 0x02});
  CHECK_EQ(classifyBle(parseAdv(chip2, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category, CAT_TRACKER);
  Adv chip3; chip3.mfg(0x08C3, {0x01, 0x02, 0x03});
  CHECK_EQ(classifyBle(parseAdv(chip3, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_CHIPOLO);
  Adv eddyUrl; eddyUrl.ad(0x16, {0xAA, 0xFE, 0x10, 0x00});  // normal Eddystone URL: not a tracker
  CHECK(classifyBle(parseAdv(eddyUrl, {0xC1, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category != CAT_TRACKER);

  // Raven: two version UUIDs = strong, one = weak, a nearby non-Raven value = nothing
  Adv raven; raven.ad(0x03, {0x00, 0x31, 0x00, 0x32, 0x0A, 0x18});
  m = classifyBle(parseAdv(raven, {0x00, 1, 2, 3, 4, 5}, SS_ADDR_PUBLIC));
  CHECK(m.category == CAT_FLOCK && m.sig_id == SIG_RAVEN && m.strong);
  Adv raven1; raven1.ad(0x03, {0x00, 0x31});
  m = classifyBle(parseAdv(raven1, {0x00, 1, 2, 3, 4, 5}, SS_ADDR_PUBLIC));
  CHECK(m.sig_id == SIG_RAVEN_PARTIAL && !m.strong && m.conf < 50);
  Adv shield; shield.ad(0x03, {0x50, 0x31}).name("SHIELD");  // 0x3150: the old range-match false positive
  CHECK(classifyBle(parseAdv(shield, {0x4A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category != CAT_FLOCK);

  // Axon OUI only counts on a public address
  Adv empty; empty.ad(0x01, {0x06});
  CHECK_EQ(classifyBle(parseAdv(empty, {0x00, 0x25, 0xDF, 1, 2, 3}, SS_ADDR_PUBLIC)).category, CAT_BODYCAM);
  CHECK(classifyBle(parseAdv(empty, {0x00, 0x25, 0xDF, 1, 2, 3}, SS_ADDR_RANDOM)).category != CAT_BODYCAM);
  Adv axon; axon.name("Axon Body 4");
  CHECK_EQ(classifyBle(parseAdv(axon, {0x7A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_AXON_NAME);

  // Flock BLE OUI on a random address is not Flock
  CHECK_EQ(classifyBle(parseAdv(empty, {0x18, 0x7F, 0x88, 1, 2, 3}, SS_ADDR_PUBLIC)).category, CAT_FLOCK);
  CHECK(classifyBle(parseAdv(empty, {0x18, 0x7F, 0x88, 1, 2, 3}, SS_ADDR_RANDOM)).category != CAT_FLOCK);

  // Ring OUIs were in the "Flock BLE" list: still ALPR-category, but weak and labeled Ring
  m = classifyBle(parseAdv(empty, {0x18, 0x7F, 0x88, 1, 2, 3}, SS_ADDR_PUBLIC));
  CHECK(m.sig_id == SIG_RING_OUI && !m.strong && m.conf < 40);
  // Apple / HPE entries are gone from it
  CHECK(classifyBle(parseAdv(empty, {0xCC, 0x66, 0x0A, 1, 2, 3}, SS_ADDR_PUBLIC)).category != CAT_FLOCK);
  CHECK(classifyBle(parseAdv(empty, {0xF4, 0x03, 0x43, 1, 2, 3}, SS_ADDR_PUBLIC)).category != CAT_FLOCK);
  // Vendor OUIs: public only
  CHECK_EQ(classifyBle(parseAdv(empty, {0x98, 0x59, 0x49, 1, 2, 3}, SS_ADDR_PUBLIC)).category, CAT_GLASSES);
  CHECK(classifyBle(parseAdv(empty, {0x98, 0x59, 0x49, 1, 2, 3}, SS_ADDR_RANDOM)).category != CAT_GLASSES);
  CHECK_EQ(classifyBle(parseAdv(empty, {0x48, 0x1C, 0xB9, 1, 2, 3}, SS_ADDR_PUBLIC)).sig_id, SIG_DJI_OUI);
  // Glasses by maker ID (random address, the normal case)
  struct { uint16_t co; uint16_t sig; bool strong; } glasses[] = {
    {0x0D53, SIG_RAYBAN_META, true}, {0x03C2, SIG_SNAP_SPECTACLES, true}, {0x058E, SIG_META_MFGR, true},
    {0x01AB, SIG_META_INC, false},   {0x060C, SIG_VUZIX, true},
  };
  for (auto& g : glasses) {
    Adv a; a.mfg(g.co, {0x01, 0x02});
    m = classifyBle(parseAdv(a, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
    CHECK(m.category == CAT_GLASSES && m.sig_id == g.sig && m.strong == g.strong);
  }
  Adv rbm; rbm.ad(0x03, {0x5F, 0xFD});
  CHECK_EQ(classifyBle(parseAdv(rbm, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_META_UUID);
  Adv snapUuid; snapUuid.ad(0x16, {0x45, 0xFE, 0x01});
  CHECK_EQ(classifyBle(parseAdv(snapUuid, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_SNAP_SPECTACLES);
  // Body cams by UUID / maker; Motorola phones (0x0008) are not police gear
  Adv axu; axu.ad(0x03, {0x81, 0xFC});
  m = classifyBle(parseAdv(axu, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
  CHECK(m.category == CAT_BODYCAM && m.sig_id == SIG_AXON_UUID && m.strong);
  Adv taser; taser.mfg(0x034D, {0x00});
  CHECK_EQ(classifyBle(parseAdv(taser, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_TASER_MFGR);
  Adv moto; moto.mfg(0x04EC, {0x00});
  m = classifyBle(parseAdv(moto, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM));
  CHECK(m.category == CAT_BODYCAM && !m.strong);
  Adv motoPhone; motoPhone.mfg(0x0008, {0x00});
  CHECK(classifyBle(parseAdv(motoPhone, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category != CAT_BODYCAM);
  Adv djiCo; djiCo.mfg(0x08AA, {0x00});
  CHECK_EQ(classifyBle(parseAdv(djiCo, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_DJI_MFGR);
  // Remote ID still wins over a maker ID in the same advert
  Adv ridDji; ridDji.ad(0x16, {0xFA, 0xFF, 0x0D, 0x00}).mfg(0x08AA, {0x00});
  CHECK_EQ(classifyBle(parseAdv(ridDji, {0x5A, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).sig_id, SIG_REMOTE_ID_BLE);

  Adv rid; rid.ad(0x16, {0xFA, 0xFF, 0x0D, 0x00});
  CHECK_EQ(classifyBle(parseAdv(rid, {0xC2, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category, CAT_DRONE);
  Adv flip; flip.name("Flipper Bob");
  CHECK_EQ(classifyBle(parseAdv(flip, {0xC2, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category, CAT_PENTEST);
  Adv garmin; garmin.mfg(0x0087, {1, 2, 3});
  CHECK_EQ(classifyBle(parseAdv(garmin, {0xC2, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category, CAT_WEARABLE);
  Adv hm10; hm10.name("HMSoft");
  CHECK_EQ(classifyBle(parseAdv(hm10, {0xC2, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM)).category, CAT_SKIMMER);

  // Garbage / truncated input never crashes and never over-reads
  uint8_t junk[31];
  srand(3);
  for (int t = 0; t < 2000; ++t) {
    for (auto& b : junk) b = (uint8_t)rand();
    BleAdv j;
    parseBleAdv(junk, (size_t)(rand() % 32), j);
    classifyBle(j);
  }
  const uint8_t trunc[] = {0x10, 0xFF, 0x4C, 0x00};
  BleAdv t;
  CHECK(!parseBleAdv(trunc, sizeof(trunc), t));

  // Sighting record carries flags
  Sighting s = makeBleSighting(a, classifyBle(a), 37);
  CHECK(s.flags & SF_RANDOM_ADDR);
  CHECK(s.flags & SF_STATIC_RAND);  // 0xD1 = 11xxxxxx
  CHECK(s.flags & SF_STRONG_MATCH);
  CHECK_EQ(s.company, 0x004C);
}

static void testWifi() {
  puts("wifi parse + classify");
  const std::vector<uint8_t> rates = {0x02, 0x04, 0x0B, 0x16};
  const std::vector<uint8_t> ht(26, 0x2D);
  Frame probeA(4, kBcast, {0xDA, 0xA1, 0x19, 0x5E, 0x62, 0x07}, 100);
  probeA.ssid("HomeNet").ie(1, rates).ie(45, ht).ie(221, {0x00, 0x50, 0xF2, 0x08, 0x00});
  Frame probeB(4, kBcast, {0x5E, 0x11, 0x22, 0x33, 0x44, 0x55}, 2000);
  probeB.ssid("").ie(1, rates).ie(45, ht).ie(221, {0x00, 0x50, 0xF2, 0x08, 0x01});
  Frame probeC(4, kBcast, {0x5E, 0x11, 0x22, 0x33, 0x44, 0x55}, 2000);
  probeC.ssid("").ie(1, {0x02, 0x04}).ie(45, ht);

  WifiFrame fa, fb, fc;
  CHECK(parseWifiFrame(probeA.b.data(), probeA.b.size(), fa));
  CHECK(parseWifiFrame(probeB.b.data(), probeB.b.size(), fb));
  CHECK(parseWifiFrame(probeC.b.data(), probeC.b.size(), fc));
  CHECK_EQ(fa.kind, KIND_WIFI_PROBE_REQ);
  CHECK_EQ(fa.seq, 100);
  CHECK(strcmp(fa.ssid, "HomeNet") == 0);
  CHECK(fa.fp != 0);
  CHECK_EQ(fa.fp, fb.fp);  // same capabilities, different MAC/SSID/vendor payload
  CHECK(fa.fp != fc.fp);   // different capabilities
  Match m = classifyWifi(fa);
  CHECK_EQ(m.category, CAT_PHONE);
  CHECK_EQ(m.sig_id, SIG_PROBE_RANDOM);

  Frame flock(8, kBcast, {0x02, 0x00, 0x00, 0x00, 0x00, 0x01});
  flock.ssid("FLOCK-S3-1F2A").ie(3, {6});
  WifiFrame ff;
  CHECK(parseWifiFrame(flock.b.data(), flock.b.size(), ff));
  CHECK_EQ(ff.kind, KIND_WIFI_BEACON);
  CHECK_EQ(ff.ds_channel, 6);
  CHECK_EQ(ff.fp, 0u);
  m = classifyWifi(ff);
  CHECK(m.category == CAT_FLOCK && m.strong);
  // Heard while tuned to 9 (a strong router leaks next door): filed under the channel its beacon states
  CHECK_EQ(makeWifiSighting(ff, m, 9, -50).channel, 6);
  {
    WifiFrame nods = ff;
    nods.ds_channel = 0;
    CHECK_EQ(makeWifiSighting(nods, m, 9, -50).channel, 9);   // no DS element: the tuned channel
  }
  {  // the table keeps a router on its beacon channel when its other frames turn up next door
    static Device st[4];
    DeviceTable t(st, 4);
    Sighting b = makeWifiSighting(ff, m, 9, -50);
    Device* d = nullptr;
    t.ingest(0, b, 1000, &d);
    CHECK(d && d->channel == 6);
    Sighting data = b;
    data.kind = KIND_WIFI_DATA;
    data.channel = 8;
    t.ingest(1, data, 2000);
    CHECK_EQ(d->channel, 6);
  }

  // Real-world form (flock-you WiGLE export): mixed case, name ends in the BSSID
  Frame flock2(8, kBcast, {0x70, 0xC9, 0x4E, 0x7F, 0x68, 0xFF});
  flock2.ssid("Flock-7F68FF").ie(3, {1});
  WifiFrame ff2;
  CHECK(parseWifiFrame(flock2.b.data(), flock2.b.size(), ff2));
  m = classifyWifi(ff2);
  CHECK(m.category == CAT_FLOCK && m.sig_id == SIG_FLOCK_SSID && m.conf == 95);
  Frame flock3(8, kBcast, {0x02, 0x11, 0x22, 0x33, 0x44, 0x55});
  flock3.ssid("Flock-7F68FF").ie(3, {1});
  WifiFrame ff3;
  parseWifiFrame(flock3.b.data(), flock3.b.size(), ff3);
  m = classifyWifi(ff3);
  CHECK(m.category == CAT_FLOCK && m.conf == 85);   // name only
  Frame flock4(8, kBcast, {0x02, 0x11, 0x22, 0x33, 0x44, 0x55});
  flock4.ssid("Flockhart Home").ie(3, {1});
  WifiFrame ff4;
  parseWifiFrame(flock4.b.data(), flock4.b.size(), ff4);
  CHECK(classifyWifi(ff4).category != CAT_FLOCK);

  // Access point details
  CHECK_EQ(ff4.sec, SEC_OPEN);
  CHECK_EQ(ff4.gen, 0);
  CHECK_EQ(ff4.width, 20);
  CHECK_EQ(ff4.sta, 0xFFFF);
  auto rsnIe = [](std::vector<uint8_t> akms) {
    std::vector<uint8_t> d = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, (uint8_t)akms.size(), 0};
    for (uint8_t a : akms) { d.push_back(0x00); d.push_back(0x0F); d.push_back(0xAC); d.push_back(a); }
    d.push_back(0); d.push_back(0);
    return d;
  };
  Frame apx(8, kBcast, {0x3C, 0x84, 0x6A, 0x01, 0x02, 0x03});
  apx.ssid("Neyhard").ie(3, {36}).ie(48, rsnIe({2, 8})).ie(45, ht).ie(61, {36, 0x05, 0, 0, 0})
    .ie(191, std::vector<uint8_t>(12, 0)).ie(192, {1, 42, 0}).ie(11, {7, 0, 128, 0, 0})
    .ie(255, {35, 1, 2, 3});
  std::vector<uint8_t> wps = {0x00, 0x50, 0xF2, 0x04, 0x10, 0x4A, 0, 1, 0x10,
                              0x10, 0x21, 0, 7, 'N', 'E', 'T', 'G', 'E', 'A', 'R',
                              0x10, 0x23, 0, 4, 'R', '7', '0', '0',
                              0x10, 0x24, 0, 4, 'R', '7', '0', '0'};
  apx.ie(221, wps);
  WifiFrame wa;
  CHECK(parseWifiFrame(apx.b.data(), apx.b.size(), wa));
  CHECK_EQ(wa.sec, SEC_WPA2_3);
  CHECK_EQ(wa.gen, 6);
  CHECK_EQ(wa.width, 80);
  CHECK_EQ(wa.sta, 7);
  CHECK_EQ(wa.util, 128);
  CHECK(strcmp(wa.wps, "NETGEAR R700") == 0);
  CHECK(strcmp(apSecName(wa.sec), "WPA2/WPA3") == 0);
  Frame ent(8, kBcast, {0x3C, 0x84, 0x6A, 0x01, 0x02, 0x04});
  ent.ssid("corp").ie(48, rsnIe({1}));
  WifiFrame we;
  CHECK(parseWifiFrame(ent.b.data(), ent.b.size(), we));
  CHECK_EQ(we.sec, SEC_ENTERPRISE);
  Frame wep(8, kBcast, {0x3C, 0x84, 0x6A, 0x01, 0x02, 0x05});
  wep.b[24 + 10] = 0x11;  // capability: ESS + privacy
  wep.ssid("old");
  WifiFrame ww;
  CHECK(parseWifiFrame(wep.b.data(), wep.b.size(), ww));
  CHECK_EQ(ww.sec, SEC_WEP);
  Frame w7(8, kBcast, {0x3C, 0x84, 0x6A, 0x01, 0x02, 0x06});
  w7.ssid("be").ie(48, rsnIe({8})).ie(255, {108, 0}).ie(192, {2, 50, 0});
  WifiFrame w7f;
  CHECK(parseWifiFrame(w7.b.data(), w7.b.size(), w7f));
  CHECK_EQ(w7f.sec, SEC_WPA3);
  CHECK_EQ(w7f.gen, 7);
  CHECK_EQ(w7f.width, 160);
  CHECK_EQ(fa.sec, SEC_UNKNOWN);   // probe requests carry no AP details

  // Data frames: a client talking to its access point
  {
    std::vector<uint8_t> d = {0x08, 0x01, 0, 0,  0x3C, 0x84, 0x6A, 0x01, 0x02, 0x03,  0xDA, 0x11, 0x22, 0x33, 0xC1, 0x42,
                              0x3C, 0x84, 0x6A, 0x01, 0x02, 0x03, 0x10, 0x00};
    WifiFrame wd;
    CHECK(parseWifiFrame(d.data(), d.size(), wd));
    CHECK(wd.kind == KIND_WIFI_DATA && wd.to_ds && !wd.from_ds);
    Match md = classifyWifi(wd);
    CHECK(md.category == CAT_PHONE && md.sig_id == SIG_WIFI_CLIENT_PRIVATE);   // GrapheneOS / iOS private address
    const uint8_t apple[3] = {0x00, 0x03, 0x93};   // Apple's first OUI
    memcpy(d.data() + 10, apple, 3);
    parseWifiFrame(d.data(), d.size(), wd);
    md = classifyWifi(wd);
    CHECK(md.category == CAT_PHONE && md.sig_id == SIG_PHONE_MAKER_WIFI);
    CHECK_EQ(makeWifiSighting(wd, md, 6, -50).company, 0x004C);
    CHECK(strcmp(ouiMakerName(ouiMaker(wd.addr2)), "Apple") == 0);
    d[10] = 0x00; d[11] = 0x11; d[12] = 0x22;   // unknown maker
    parseWifiFrame(d.data(), d.size(), wd);
    CHECK_EQ(classifyWifi(wd).sig_id, SIG_WIFI_CLIENT);
    // Tesla, car hotspot, OBD dongle
    BleAdv t; memcpy(t.name, "S1a2b3c4d5e6f7a8bC", 18); t.name_len = 18; t.name[18] = 0;
    CHECK_EQ(classifyBle(t).sig_id, SIG_TESLA_BLE);
    BleAdv t2; memcpy(t2.name, "Sxa2b3c4d5e6f7a8bC", 18); t2.name_len = 18; t2.name[18] = 0;
    CHECK(classifyBle(t2).category != CAT_VEHICLE);
    Frame car(8, kBcast, {0x00, 0x11, 0x22, 0x01, 0x02, 0x07});
    car.ssid("myChevrolet 4821");
    WifiFrame fcar;
    parseWifiFrame(car.b.data(), car.b.size(), fcar);
    CHECK(classifyWifi(fcar).category == CAT_VEHICLE);
    Frame notcar(8, kBcast, {0x00, 0x11, 0x22, 0x01, 0x02, 0x08});
    notcar.ssid("Kiara's iPhone");
    WifiFrame fn;
    parseWifiFrame(notcar.b.data(), notcar.b.size(), fn);
    CHECK(classifyWifi(fn).category != CAT_VEHICLE);
    CHECK_EQ(classifyClassic(0x001F00, "OBDII").sig_id, SIG_OBD_DONGLE);
    // Microsoft Connected Devices beacon: a Windows laptop
    // Remote ID: a message pack with Basic ID, Location and System (operator position)
    {
      auto put32 = [](std::vector<uint8_t>& v, size_t at, int32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = (uint8_t)(x >> (8 * i)); };
      auto put16 = [](std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8); };
      std::vector<uint8_t> basic(25, 0), loc(25, 0), sys(25, 0), self(25, 0);
      basic[0] = 0x02; basic[1] = 0x12; memcpy(&basic[2], "1581F5FKD229400BB", 17);
      loc[0] = 0x12; loc[1] = 0x20 | 0x02; loc[2] = 90;      // airborne, east half: 90 + 180 = 270 degrees
      loc[3] = 40;                                          // 40 * 0.25 = 10 m/s
      put32(loc, 5, 418002681); put32(loc, 9, -772516868);
      put16(loc, 15, (uint16_t)((450 + 1000) * 2));          // 450 m GPS altitude
      put16(loc, 17, (uint16_t)((120 + 1000) * 2));          // 120 m above takeoff
      sys[0] = 0x42; put32(sys, 2, 417990000); put32(sys, 6, -772500000);
      self[0] = 0x32; memcpy(&self[2], "Roof survey", 11);
      std::vector<uint8_t> ble = {0x0D, 7, 0xF2, 25, 4};
      for (auto* m : {&basic, &loc, &sys, &self}) ble.insert(ble.end(), m->begin(), m->end());
      RemoteId r;
      CHECK(decodeRemoteIdBle(ble.data(), ble.size(), r));
      CHECK(r.has_id && strcmp(r.id, "1581F5FKD229400BB") == 0 && r.id_type == 1 && r.ua_type == 2);
      CHECK(r.has_loc && r.lat_e7 == 418002681 && r.lon_e7 == -772516868);
      CHECK(r.alt_m == 450 && r.height_m == 120 && r.speed_cms == 1000 && r.dir == 270 && r.status == 2);
      CHECK(r.has_op && r.op_lat_e7 == 417990000 && r.op_lon_e7 == -772500000);
      CHECK(strcmp(r.text, "Roof survey") == 0);
      std::vector<uint8_t> wifi(ble.begin() + 1, ble.end());   // Wi-Fi: counter, then the pack
      CHECK(decodeRemoteIdWifi(wifi.data(), wifi.size(), r) && r.has_loc && r.has_id);
      CHECK(!decodeRemoteIdBle(ble.data(), 20, r));
      ble[4] = 9;   // claims more messages than sent
      CHECK(!decodeRemoteIdBle(ble.data(), ble.size(), r));
      srand(11);
      for (int t = 0; t < 2000; ++t) {
        uint8_t junk[120];
        const size_t l = (size_t)(rand() % 121);
        for (size_t i = 0; i < l; ++i) junk[i] = (uint8_t)rand();
        if (l) junk[0] = 0x0D;
        decodeRemoteIdBle(junk, l, r);
        decodeRemoteIdWifi(junk, l, r);
      }
    }
    // Phone hotspots, router uptime
    {
      Frame hs(8, kBcast, {0x02, 0x11, 0x22, 0x33, 0x44, 0x56});
      hs.ssid("Rob's iPhone");
      WifiFrame fh;
      parseWifiFrame(hs.b.data(), hs.b.size(), fh);
      CHECK_EQ(classifyWifi(fh).sig_id, SIG_PHONE_HOTSPOT);
      Frame ap2(8, kBcast, {0x00, 0x11, 0x22, 0x33, 0x44, 0x57});
      ap2.ssid("Neighbors");
      const uint64_t us = 3ull * 86400 * 1000000;   // up 3 days
      for (int i = 0; i < 8; ++i) ap2.b[24 + i] = (uint8_t)(us >> (8 * i));
      WifiFrame fu;
      parseWifiFrame(ap2.b.data(), ap2.b.size(), fu);
      CHECK_EQ(fu.tsf, us);
    }
    Adv msa; msa.mfg(0x0006, {0x01, 0x2F, 0x00, 0x00});
    BleAdv ms = parseAdv(msa, {0xC2, 1, 2, 3, 4, 5}, SS_ADDR_RANDOM);
    char nm[kMaxName + 1];
    CHECK(bleModelName(ms, nm) && strcmp(nm, "Windows laptop") == 0);
  }

  Frame drone(8, kBcast, {0x60, 0x60, 0x1F, 0x00, 0x00, 0x01});
  drone.ssid("").ie(221, {0xFA, 0x0B, 0xBC, 0x0D, 0x01, 0x02});
  WifiFrame fd;
  parseWifiFrame(drone.b.data(), drone.b.size(), fd);
  CHECK(fd.remote_id);
  CHECK_EQ(classifyWifi(fd).category, CAT_DRONE);

  // DJI DroneID vendor IE, drone hotspot names, vendor OUIs on Wi-Fi
  Frame dji(8, kBcast, {0x02, 0x00, 0x00, 0x00, 0x00, 0x09});
  dji.ssid("").ie(221, {0x26, 0x37, 0x12, 0x58, 0x62, 0x13, 0x10});
  WifiFrame fdj;
  parseWifiFrame(dji.b.data(), dji.b.size(), fdj);
  m = classifyWifi(fdj);
  CHECK(m.category == CAT_DRONE && m.sig_id == SIG_DJI_DRONEID && m.strong);
  for (const char* name : {"TELLO-5A1B2C", "tello-5a1b2c", "Anafi-001122", "Skydio-X2"}) {
    Frame t(8, kBcast, {0x02, 0x00, 0x00, 0x00, 0x00, 0x0A});
    t.ssid(name);
    WifiFrame ft;
    parseWifiFrame(t.b.data(), t.b.size(), ft);
    CHECK_EQ(classifyWifi(ft).sig_id, SIG_DRONE_SSID);
  }
  {
    Frame t(4, kBcast, {0x02, 0x00, 0x00, 0x00, 0x00, 0x0B});  // a phone probing for a Tello: not a drone
    t.ssid("TELLO-5A1B2C");
    WifiFrame ft;
    parseWifiFrame(t.b.data(), t.b.size(), ft);
    CHECK(classifyWifi(ft).category != CAT_DRONE);
  }
  struct { uint8_t oui[3]; uint8_t cat; uint16_t sig; } wouis[] = {
    {{0xB4, 0x1E, 0x52}, CAT_FLOCK, SIG_FLOCK_OWN_OUI},  {{0x00, 0x23, 0xBD}, CAT_BODYCAM, SIG_DIGITAL_ALLY_OUI},
    {{0x00, 0x1D, 0x96}, CAT_BODYCAM, SIG_WATCHGUARD_OUI}, {{0x38, 0x1D, 0x14}, CAT_DRONE, SIG_SKYDIO_OUI},
    {{0x60, 0x60, 0x1F}, CAT_DRONE, SIG_DJI_OUI},        {{0x98, 0x59, 0x49}, CAT_GLASSES, SIG_LUXOTTICA_OUI},
  };
  for (auto& w : wouis) {
    Frame t(8, kBcast, {w.oui[0], w.oui[1], w.oui[2], 0x01, 0x02, 0x03});
    t.ssid("whatever");
    WifiFrame ft;
    parseWifiFrame(t.b.data(), t.b.size(), ft);
    m = classifyWifi(ft);
    CHECK(m.category == w.cat && m.sig_id == w.sig);
    CHECK(sigName(m.sig_id)[0] != 0);
  }
  {
    Frame t(8, kBcast, {0x00, 0x01, 0x21, 0x01, 0x02, 0x03});  // WatchGuard Technologies (firewalls): not a camera
    t.ssid("Office");
    WifiFrame ft;
    parseWifiFrame(t.b.data(), t.b.size(), ft);
    CHECK(classifyWifi(ft).category != CAT_BODYCAM);
  }

  Frame pwn(8, kBcast, {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD});
  pwn.ssid("");
  WifiFrame fp;
  parseWifiFrame(pwn.b.data(), pwn.b.size(), fp);
  CHECK_EQ(classifyWifi(fp).sig_id, SIG_PWNAGOTCHI);

  Frame pine(8, kBcast, {0x00, 0x13, 0x37, 0xA5, 0x01, 0x02});
  pine.ssid("FreeWiFi");
  WifiFrame fpi;
  parseWifiFrame(pine.b.data(), pine.b.size(), fpi);
  CHECK_EQ(classifyWifi(fpi).sig_id, SIG_PINEAPPLE);

  Frame esp(8, kBcast, {0xA4, 0xCF, 0x12, 0x01, 0x02, 0x03});
  esp.ssid("ESP_123");
  WifiFrame fe;
  parseWifiFrame(esp.b.data(), esp.b.size(), fe);
  m = classifyWifi(fe);
  CHECK(m.sig_id == SIG_FLOCK_WIFI_OUI_GENERIC && m.conf <= 20 && m.category == CAT_OTHER_WIFI);

  {
    // Liteon (most of the Flock Wi-Fi OUI list): a laptop probing is weak, an AP is a maybe
    Frame lap(4, kBcast, {0xC0, 0x35, 0x32, 0x0D, 0x3D, 0xF9});
    lap.ssid("HomeWiFi");
    WifiFrame fl;
    parseWifiFrame(lap.b.data(), lap.b.size(), fl);
    m = classifyWifi(fl);
    CHECK(m.sig_id == SIG_FLOCK_WIFI_OUI_GENERIC && m.conf < 40 && !m.strong && m.category == CAT_OTHER_WIFI);
    Frame cam(8, kBcast, {0xC0, 0x35, 0x32, 0x0D, 0x3D, 0xF9});
    cam.ssid("Unnamed").ie(3, {6});
    WifiFrame fc2;
    parseWifiFrame(cam.b.data(), cam.b.size(), fc2);
    m = classifyWifi(fc2);
    CHECK(m.sig_id == SIG_FLOCK_WIFI_OUI && m.conf >= 40 && m.conf < 50);
  }

  Frame ap(8, kBcast, {0x00, 0x11, 0x22, 0x33, 0x44, 0x55});
  ap.ssid("Neighbors");
  WifiFrame fap;
  parseWifiFrame(ap.b.data(), ap.b.size(), fap);
  CHECK_EQ(classifyWifi(fap).category, CAT_OTHER_WIFI);

  // Deauth flood
  Frame deauth(12, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11}, {0x22, 0x22, 0x22, 0x22, 0x22, 0x22});
  deauth.b.push_back(7); deauth.b.push_back(0);
  WifiFrame fde;
  CHECK(parseWifiFrame(deauth.b.data(), deauth.b.size(), fde));
  CHECK_EQ(fde.kind, KIND_WIFI_OTHER);
  FloodMonitor fm(15, 10000);
  Event ev;
  for (int i = 0; i < 10; ++i) fm.onFrame(fde, 11, 1000 + i);
  CHECK(!fm.poll(ev, 1100));
  for (int i = 0; i < 20; ++i) fm.onFrame(fde, 11, 1200 + i);
  CHECK(fm.poll(ev, 1300));
  CHECK(ev.type == EV_DEAUTH_FLOOD && ev.channel == 11 && ev.count >= 15);
  CHECK(!fm.poll(ev, 1301));
  for (int i = 0; i < 20; ++i) fm.onFrame(fde, 11, 3000 + i);
  CHECK(!fm.poll(ev, 3100));  // inside hold-off
  for (int i = 0; i < 20; ++i) fm.onFrame(fde, 11, 12000 + i);
  CHECK(fm.poll(ev, 12100));

  // Truncated / junk frames
  uint8_t junk[64];
  srand(5);
  for (int t = 0; t < 2000; ++t) {
    for (auto& b : junk) b = (uint8_t)rand();
    WifiFrame j;
    parseWifiFrame(junk, (size_t)(rand() % 65), j);
    classifyWifi(j);
  }
  WifiFrame tiny;
  CHECK(!parseWifiFrame(junk, 10, tiny));
}

static void testBatch() {
  puts("batching");
  SightingBatch<4> b;
  Sighting s;
  s.addr[5] = 1; s.kind = KIND_BLE_ADV; s.rssi_max = s.rssi_last = -70; s.hits = 1; s.conf = 10; s.category = CAT_OTHER_BLE;
  for (int i = 0; i < 5; ++i) { s.rssi_last = s.rssi_max = (int8_t)(-70 + i); CHECK(b.add(s, 1000 + i)); }
  CHECK_EQ(b.size(), 1u);
  // Same address over Wi-Fi is a different entry
  Sighting w = s; w.kind = KIND_WIFI_PROBE_REQ;
  CHECK(b.add(w, 1000));
  CHECK_EQ(b.size(), 2u);
  // Upgrade match within the interval
  Sighting up = s; up.conf = 80; up.category = CAT_TRACKER; up.sig_id = SIG_TILE;
  b.add(up, 1010);
  Sighting s3 = s; s3.addr[5] = 3; b.add(s3, 1000);
  Sighting s4 = s; s4.addr[5] = 4; b.add(s4, 1000);
  Sighting s5 = s; s5.addr[5] = 5;
  CHECK(!b.add(s5, 1000));
  CHECK_EQ(b.takeDropped(), 1);
  CHECK_EQ(b.takeDropped(), 0);

  Sighting out[8];
  const size_t n = b.take(out, 2, 1500);
  CHECK_EQ(n, 2u);
  CHECK_EQ(out[0].category, CAT_TRACKER);  // highest confidence first
  CHECK_EQ(out[0].hits, 6);
  CHECK_EQ(out[0].rssi_max, -66);
  CHECK_EQ(out[0].age_ms, 490);
  CHECK_EQ(b.size(), 2u);
  CHECK_EQ(b.take(out, 8, 1500), 2u);
  CHECK_EQ(b.size(), 0u);

  NameTracker<2> nt(1000);
  const uint8_t a1[6] = {1, 1, 1, 1, 1, 1}, a2[6] = {2, 2, 2, 2, 2, 2}, a3[6] = {3, 3, 3, 3, 3, 3};
  CHECK(nt.shouldSend(a1, 0));
  CHECK(!nt.shouldSend(a1, 500));
  CHECK(nt.shouldSend(a1, 1500));
  CHECK(nt.shouldSend(a2, 1600));
  CHECK(nt.shouldSend(a3, 1700));  // evicts a1 (oldest)
  CHECK(!nt.shouldSend(a2, 1800));
}

static Sighting mkS(uint8_t last, int8_t rssi, uint32_t fp, uint8_t cat = CAT_PHONE, uint8_t conf = 50) {
  Sighting s;
  const uint8_t addr[6] = {0x5A, 0x3F, 0xC2, 0x9B, 0x14, last};
  memcpy(s.addr, addr, 6);
  s.kind = KIND_BLE_ADV; s.rssi_last = s.rssi_max = rssi; s.hits = 1; s.category = cat; s.conf = conf;
  s.flags = SF_RANDOM_ADDR; s.fp = fp;
  return s;
}

static void testDevices() {
  puts("device table + rotation stitching");
  static Device store[8];
  {
    DeviceTable t(store, 8);
    Device* d = nullptr;
    CHECK_EQ(t.ingest(0, mkS(1, -60, 0xAAAA), 1000, &d), DeviceTable::NEW);
    const uint32_t id = d->id;
    for (uint32_t ms = 2000; ms <= 10000; ms += 1000) t.ingest(0, mkS(1, -60, 0xAAAA), ms);
    CHECK_EQ(t.countNow(CAT_PHONE, 10000), 1u);
    CHECK_EQ(t.total(CAT_PHONE), 1u);

    // Address rotates at ~10.5 s: new address, same fingerprint, similar RSSI.
    CHECK_EQ(t.ingest(0, mkS(2, -62, 0xAAAA), 10500), DeviceTable::NEW);
    CHECK_EQ(t.total(CAT_PHONE), 2u);  // looks like two until stitched
    for (uint32_t ms = 11500; ms <= 12500; ms += 1000) t.ingest(0, mkS(2, -62, 0xAAAA), ms);
    CHECK_EQ(t.maintain(12500), 0u);  // old address only quiet 2.5 s
    t.ingest(0, mkS(2, -61, 0xAAAA), 13500);
    CHECK_EQ(t.maintain(13500), 1u);
    CHECK_EQ(t.used(), 1u);
    const uint8_t a2[6] = {0x5A, 0x3F, 0xC2, 0x9B, 0x14, 2};
    Device* nd = t.find(a2, 2);
    CHECK(nd != nullptr);
    CHECK_EQ(nd->id, id);                // same logical device
    CHECK_EQ(nd->rotations, 1);
    CHECK_EQ(nd->first_ms, 1000u);
    CHECK_EQ(t.total(CAT_PHONE), 1u);    // counted once
    CHECK_EQ(t.stitches(), 1u);
    CHECK_EQ(t.maintain(14500), 0u);     // nothing left to do
  }
  {
    // Two same-model phones, both still talking: no merge.
    DeviceTable t(store, 8);
    for (uint32_t ms = 1000; ms <= 10000; ms += 1000) t.ingest(0, mkS(1, -60, 0xBBBB), ms);
    for (uint32_t ms = 5000; ms <= 10000; ms += 1000) t.ingest(0, mkS(2, -61, 0xBBBB), ms);
    CHECK_EQ(t.maintain(10000), 0u);
    CHECK_EQ(t.used(), 2u);
  }
  {
    // Two quiet candidates with similar RSSI: ambiguous, no merge.
    DeviceTable t(store, 8);
    t.ingest(0, mkS(1, -60, 0xCCCC), 1000); t.ingest(0, mkS(1, -60, 0xCCCC), 9000);
    t.ingest(0, mkS(2, -63, 0xCCCC), 1000); t.ingest(0, mkS(2, -63, 0xCCCC), 9500);
    t.ingest(0, mkS(3, -61, 0xCCCC), 10000); t.ingest(0, mkS(3, -61, 0xCCCC), 13500);
    CHECK_EQ(t.maintain(13500), 0u);
    CHECK_EQ(t.used(), 3u);
  }
  {
    // Two quiet candidates, one clearly closer in RSSI: merge with it.
    DeviceTable t(store, 8);
    Device* d1 = nullptr;
    t.ingest(0, mkS(1, -60, 0xDDDD), 1000, &d1); t.ingest(0, mkS(1, -60, 0xDDDD), 9000);
    const uint32_t id1 = d1->id;
    t.ingest(0, mkS(2, -70, 0xDDDD), 1000); t.ingest(0, mkS(2, -70, 0xDDDD), 9500);
    t.ingest(0, mkS(3, -61, 0xDDDD), 10000); t.ingest(0, mkS(3, -61, 0xDDDD), 13500);
    CHECK_EQ(t.maintain(13500), 1u);
    const uint8_t a3[6] = {0x5A, 0x3F, 0xC2, 0x9B, 0x14, 3};
    CHECK_EQ(t.find(a3, 2)->id, id1);
  }
  {
    // Wi-Fi probes: a phone's next search burst comes 45 s later from a new random address.
    DeviceTable t(store, 8);
    auto probe = [](uint8_t last, int8_t rssi, uint32_t fp) {
      Sighting s = mkS(last, rssi, fp);
      s.kind = KIND_WIFI_PROBE_REQ;
      return s;
    };
    Device* d1 = nullptr;
    t.ingest(0, probe(1, -55, 0x1234), 1000, &d1); t.ingest(0, probe(1, -55, 0x1234), 1400);
    const uint32_t id1 = d1->id;
    t.ingest(0, probe(2, -58, 0x1234), 46000); t.ingest(0, probe(2, -57, 0x1234), 46300);
    CHECK_EQ(t.countNow(CAT_PHONE, 46300, 60000), 2u);     // looks like two until stitched
    CHECK_EQ(t.maintain(48500), 1u);
    CHECK_EQ(t.countNow(CAT_PHONE, 48500, 60000), 1u);
    const uint8_t a2[6] = {0x5A, 0x3F, 0xC2, 0x9B, 0x14, 2};
    CHECK_EQ(t.find(a2, 1)->id, id1);
    // Too long a gap (5 min): treated as a different device
    t.ingest(0, probe(3, -57, 0x1234), 350000);
    CHECK_EQ(t.maintain(353000), 0u);
    // A BLE address with the same 45 s gap does not stitch (BLE rotates seamlessly)
    DeviceTable b(store, 8);
    b.ingest(0, mkS(1, -60, 0x7777), 1000); b.ingest(0, mkS(2, -60, 0x7777), 46000);
    CHECK_EQ(b.maintain(48500), 0u);
  }
  {
    // Two people with the same phone model, both searching: ambiguous, no merge.
    DeviceTable t(store, 8);
    auto probe = [](uint8_t last, int8_t rssi) { Sighting s = mkS(last, rssi, 0x5555); s.kind = KIND_WIFI_PROBE_REQ; return s; };
    t.ingest(0, probe(1, -55), 1000); t.ingest(0, probe(2, -57), 5000);
    t.ingest(0, probe(3, -56), 40000);
    CHECK_EQ(t.maintain(43000), 0u);
  }
  {
    // Static-random and public addresses never stitch.
    DeviceTable t(store, 8);
    Sighting s1 = mkS(1, -60, 0xEEEE); s1.flags |= SF_STATIC_RAND;
    Sighting s2 = mkS(2, -60, 0xEEEE); s2.flags |= SF_STATIC_RAND;
    t.ingest(0, s1, 1000); t.ingest(0, s2, 2000); t.ingest(0, s2, 8000);
    CHECK_EQ(t.maintain(8000), 0u);
  }
  {
    // Category upgrade moves the session count; cluster MACs are ignored; eviction keeps totals.
    DeviceTable t(store, 2);
    t.ingest(0, mkS(1, -60, 0, CAT_OTHER_BLE, 10), 1000);
    CHECK_EQ(t.total(CAT_OTHER_BLE), 1u);
    t.ingest(1, mkS(1, -55, 0, CAT_TRACKER, 80), 2000);
    CHECK_EQ(t.total(CAT_OTHER_BLE), 0u);
    CHECK_EQ(t.total(CAT_TRACKER), 1u);
    const uint8_t a1[6] = {0x5A, 0x3F, 0xC2, 0x9B, 0x14, 1};
    Device* d = t.find(a1, 2);
    CHECK(d->node_rssi[0] == -60 && d->node_rssi[1] == -55);

    const uint8_t node[6] = {0x24, 0x58, 0x7C, 0x10, 0x20, 0x30};
    t.addClusterMac(node);
    Sighting self = mkS(0, -30, 0);
    memcpy(self.addr, node, 6); self.addr[5] = 0x32;  // BT MAC = base + 2
    CHECK_EQ(t.ingest(0, self, 2500), DeviceTable::SELF);

    t.ingest(0, mkS(2, -60, 0), 3000);
    t.ingest(0, mkS(3, -60, 0), 4000);  // table full: evicts the least recently seen (addr 1)
    CHECK_EQ(t.used(), 2u);
    CHECK(t.find(a1, 2) == nullptr);
    CHECK_EQ(t.total(CAT_TRACKER), 1u);
    CHECK_EQ(t.totalAll(), 3u);
    CHECK_EQ(t.countNowAll(4000), 2u);
    CHECK_EQ(t.countNowAll(40000), 0u);
  }
}

static Hello mkHello(uint8_t last, uint8_t caps) {
  Hello h;
  const uint8_t mac[6] = {0x24, 0x58, 0x7C, 0x00, 0x00, last};
  memcpy(h.mac, mac, 6);
  h.caps = caps;
  h.hw = HW_C5_DEVKIT;
  return h;
}

static void testNodes() {
  puts("node registry + assignment");
  const uint8_t c5 = CAP_WIFI24 | CAP_WIFI5 | CAP_BLE;
  const uint8_t s3 = CAP_WIFI24 | CAP_BLE;  // e.g. Heltec S3: no 5 GHz
  {
    NodeRegistry r;
    bool fresh = false;
    const int a = r.add(mkHello(1, c5), 1000, &fresh);
    CHECK(a >= 0 && fresh);
    CHECK(r.add(mkHello(1, c5), 1100, &fresh) == a && !fresh);

    // One node, everything on: it does both, all channels.
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK_EQ(r.at(a).cfg.role, ROLE_BOTH);
    CHECK_EQ(r.at(a).cfg.channel_mask, kMaskAll);
    CHECK_EQ(r.at(a).cfg.ble_duty_pct, 35);
    const uint8_t v1 = r.cfgVersion();

    // Second C5 joins: one goes BLE-only, the other takes all Wi-Fi.
    const int b = r.add(mkHello(2, c5), 2000, &fresh);
    CHECK(fresh);
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK(r.cfgVersion() != v1);
    const NodeInfo& A = r.at(a);
    const NodeInfo& B = r.at(b);
    CHECK((A.cfg.role == ROLE_BLE) != (B.cfg.role == ROLE_BLE));
    const NodeInfo& W = A.cfg.role == ROLE_WIFI ? A : B;
    const NodeInfo& L = A.cfg.role == ROLE_BLE ? A : B;
    CHECK_EQ(W.cfg.channel_mask, kMaskAll);
    CHECK_EQ(L.cfg.channel_mask, 0u);
    CHECK_EQ(L.cfg.ble_duty_pct, 100);

    // Third node without 5 GHz: it becomes the BLE node; the two C5s split Wi-Fi.
    const int c = r.add(mkHello(3, s3), 3000);
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK_EQ(r.at(c).cfg.role, ROLE_BLE);
    CHECK(r.at(a).cfg.role == ROLE_WIFI && r.at(b).cfg.role == ROLE_WIFI);
    CHECK_EQ(r.at(a).cfg.channel_mask & r.at(b).cfg.channel_mask, 0u);
    CHECK_EQ(r.at(a).cfg.channel_mask | r.at(b).cfg.channel_mask, kMaskAll);

    // BLE-only categories: everyone scans BLE full time.
    r.assign(catBit(CAT_BODYCAM) | catBit(CAT_TRACKER), ASSIGN_SPLIT);
    for (int s : {a, b, c}) CHECK(r.at(s).cfg.role == ROLE_BLE && r.at(s).cfg.ble_duty_pct == 100);

    // Mirror mode: identical plans (minus 5 GHz on the S3) for direction finding.
    r.assign(kAllCategoriesMask, ASSIGN_MIRROR);
    CHECK(r.at(a).cfg.role == ROLE_BOTH && r.at(b).cfg.role == ROLE_BOTH);
    CHECK_EQ(r.at(a).cfg.channel_mask, kMaskAll);
    CHECK_EQ(r.at(c).cfg.channel_mask, kMask24);

    // Timeouts and loss tracking
    CHECK(r.touch(mkHello(2, c5).mac, 9000, -40) == b);
    CHECK(r.expire(10000, 5000));   // a and c silent since 3000
    CHECK_EQ(r.onlineCount(), 1);
    CHECK(r.touch(mkHello(1, c5).mac, 10000) == -1);  // offline nodes must HELLO again
    CHECK(!r.expire(10000, 5000));
    r.noteSeq(b, 10); r.noteSeq(b, 11); r.noteSeq(b, 15);
    CHECK_EQ(r.at(b).lost, 3u);
    r.noteSeq(b, 2);  // reboot: big jump, not counted
    CHECK_EQ(r.at(b).lost, 3u);
    r.noteSeq(b, 65535); r.noteSeq(b, 0);  // wrap
    CHECK_EQ(r.at(b).frames, 6u);
  }
  {
    NodeRegistry r;
    for (uint8_t i = 0; i < kMaxNodes; ++i) CHECK(r.add(mkHello(i, c5), 100) >= 0);
    CHECK_EQ(r.add(mkHello(99, c5), 100), -1);  // full, all online
    r.expire(10000, 1000);
    CHECK(r.add(mkHello(99, c5), 10000) >= 0);   // reuses an offline slot
  }
  {
    // Rob's kit: C5 node + classic ESP32 node (2.4 GHz Wi-Fi + Classic, no BLE), all on.
    // The ESP32 takes 2.4 GHz; the C5 keeps BLE and takes 5 GHz. Nobody idles.
    const uint8_t esp32 = CAP_WIFI24 | CAP_BT_CLASSIC;
    NodeRegistry r;
    const int c5n = r.add(mkHello(10, c5), 100);
    const int classic = r.add(mkHello(11, esp32), 100);
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK_EQ(r.at(classic).cfg.role, ROLE_WIFI);
    CHECK_EQ(r.at(classic).cfg.ble_duty_pct, 0);
    CHECK_EQ(r.at(classic).cfg.channel_mask, kMask24);
    CHECK_EQ(r.at(c5n).cfg.role, ROLE_BOTH);
    CHECK(r.at(c5n).cfg.ble_duty_pct > 0);
    CHECK_EQ(r.at(c5n).cfg.channel_mask, kMask5);

    // Second C5 joins: one C5 goes BLE-only, the other takes 5 GHz, the ESP32 2.4 GHz.
    const int c5b = r.add(mkHello(12, c5), 200);
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    const int bleOne = r.at(c5n).cfg.role == ROLE_BLE ? c5n : c5b;
    const int wifiOne = bleOne == c5n ? c5b : c5n;
    CHECK_EQ(r.at(bleOne).cfg.role, ROLE_BLE);
    CHECK_EQ(r.at(wifiOne).cfg.role, ROLE_WIFI);
    CHECK_EQ(r.at(wifiOne).cfg.channel_mask, kMask5);
    CHECK_EQ(r.at(classic).cfg.channel_mask, kMask24);

    // BLE-only categories: the ESP32 never gets a BLE job (it can't), the C5s do.
    r.assign(catBit(CAT_AUDIO), ASSIGN_SPLIT);
    CHECK_EQ(r.at(classic).cfg.ble_duty_pct, 0);
    CHECK_EQ(r.at(classic).cfg.channel_mask, 0u);
    CHECK(r.at(c5n).cfg.ble_duty_pct == 100 && r.at(c5b).cfg.ble_duty_pct == 100);

    // Mirror mode: the ESP32 does what it can (2.4 GHz), no BLE.
    r.assign(kAllCategoriesMask, ASSIGN_MIRROR);
    CHECK_EQ(r.at(classic).cfg.channel_mask, kMask24);
    CHECK_EQ(r.at(classic).cfg.ble_duty_pct, 0);
  }
  {
    // Jobs pinned on the master: a C5 forced to Wi-Fi only, another to Bluetooth only.
    NodeRegistry r;
    const int a = r.add(mkHello(20, c5), 100);
    const int b = r.add(mkHello(21, c5), 100);
    r.at(a).force_role = ROLE_WIFI;
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK(r.at(a).cfg.role == ROLE_WIFI && r.at(a).cfg.ble_duty_pct == 0);
    CHECK_EQ(r.at(a).cfg.channel_mask & kMask5, kMask5);   // the only 5 GHz-capable Wi-Fi node
    r.at(a).force_role = ROLE_AUTO;
    r.at(b).force_role = ROLE_BLE;
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK(r.at(b).cfg.role == ROLE_BLE && r.at(b).cfg.ble_duty_pct == 100 && r.at(b).cfg.channel_mask == 0);
    CHECK(r.at(a).cfg.channel_mask != 0);                  // the other one covers Wi-Fi
    r.forget(b);
    CHECK(!r.at(b).used);
    CHECK_EQ(r.onlineCount(), 1);
  }
  {
    // Which board hears a device best
    Device d;
    for (uint8_t i = 0; i < kMaxNodes; ++i) { d.node_rssi[i] = -127; d.node_ms[i] = 0; }
    int8_t r = 0;
    uint8_t c = 9;
    CHECK_EQ(bestNode(d, 100000, 60000, &r, &c), -1);
    d.node_rssi[1] = -70; d.node_ms[1] = 95000;
    d.node_rssi[4] = -55; d.node_ms[4] = 99000;
    d.node_rssi[2] = -40; d.node_ms[2] = 10000;   // strongest, but heard long ago
    CHECK(bestNode(d, 100000, 60000, &r, &c) == 4 && r == -55 && c == 2);
    CHECK(bestNode(d, 500000, 60000, &r, &c) == 4 && c == 0);   // nobody recently: the last one to hear it
  }
  {
    // EVERYTHING: a C5 across the house scans the whole plan on its own; the office
    // nodes (a C5, a classic ESP32, a C5) split the work as if it weren't there.
    const uint8_t esp32x = CAP_WIFI24 | CAP_BT_CLASSIC;
    NodeRegistry r;
    const int far = r.add(mkHello(30, c5), 100);
    const int o1 = r.add(mkHello(31, c5), 100);
    const int o2 = r.add(mkHello(32, esp32x), 100);
    const int o3 = r.add(mkHello(33, c5), 100);
    r.at(far).force_role = ROLE_ALL;
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    const ScanPlan plan = planForCategories(kAllCategoriesMask);
    CHECK(r.at(far).cfg.role == ROLE_BOTH && r.at(far).cfg.channel_mask == plan.channel_mask);
    CHECK_EQ(r.at(far).cfg.ble_duty_pct, plan.ble_duty_pct);
    // The office nodes cover every channel between them, without overlapping
    const uint64_t m1 = r.at(o1).cfg.channel_mask, m2 = r.at(o2).cfg.channel_mask, m3 = r.at(o3).cfg.channel_mask;
    CHECK_EQ(m1 | m2 | m3, plan.channel_mask);
    CHECK((m1 & m2) == 0 && (m1 & m3) == 0 && (m2 & m3) == 0);
    CHECK_EQ(m2 & kMask5, 0u);                                    // the classic one stays on 2.4 GHz
    CHECK(r.at(o1).cfg.ble_duty_pct || r.at(o3).cfg.ble_duty_pct); // and one of the office C5s does BLE
    CHECK(strcmp(roleName(ROLE_ALL), "all") == 0);
    // Alone with the classic one: it gets all of 2.4 GHz, the far node still everything
    r.at(o1).online = r.at(o3).online = false;
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK_EQ(r.at(o2).cfg.channel_mask, plan.channel_mask & kMask24);
    CHECK_EQ(r.at(far).cfg.channel_mask, plan.channel_mask);
    // Every node on EVERYTHING: each one scans it all
    r.at(o2).force_role = ROLE_ALL;
    r.assign(kAllCategoriesMask, ASSIGN_SPLIT);
    CHECK_EQ(r.at(o2).cfg.channel_mask, plan.channel_mask & kMask24);
    CHECK(r.at(o2).cfg.role == ROLE_WIFI && r.at(o2).cfg.ble_duty_pct == 0);
  }
  {
    // Master scanning locally: slot 0 is its own radio, never handed to a node.
    NodeRegistry r;
    r.reserve(0);
    for (uint8_t i = 0; i < kMaxNodes - 1; ++i) CHECK(r.add(mkHello(i, c5), 100) > 0);
    CHECK_EQ(r.add(mkHello(98, c5), 100), -1);   // 7 node slots left
    r.expire(10000, 1000);
    const int s = r.add(mkHello(99, c5), 10000);
    CHECK(s > 0);                                 // reuse skips the reserved slot too
    CHECK(!r.at(0).used);
    r.reserve(0, false);
    CHECK(!r.reserved(0));
  }
}


static Sighting spoof(uint8_t n, int8_t rssi, uint16_t sig, uint8_t cat) {
  Sighting s;
  const uint8_t addr[6] = {0x4D, 0x1B, 0x44, 0x73, 0x7F, n};
  memcpy(s.addr, addr, 6);
  s.kind = KIND_BLE_ADV; s.rssi_last = s.rssi_max = rssi; s.hits = 1; s.category = cat; s.conf = 60;
  s.sig_id = sig; s.flags = SF_RANDOM_ADDR; s.fp = 0x1000u + n;
  return s;
}

static Sighting ap(std::initializer_list<uint8_t> mac, int8_t rssi) {
  Sighting s;
  memcpy(s.addr, mac.begin(), 6);
  s.kind = KIND_WIFI_BEACON; s.rssi_last = s.rssi_max = rssi; s.hits = 1; s.category = CAT_OTHER_WIFI; s.conf = 5;
  s.sig_id = SIG_ACCESS_POINT; s.flags = SF_RANDOM_ADDR;
  return s;
}

static void testSpamAndAliases() {
  puts("ble spam + AP grouping");
  static Device store[96];
  const uint16_t sigs[] = {SIG_APPLE_AUDIO, SIG_FAST_PAIR, SIG_SAMSUNG_DEVICE, SIG_MICROSOFT, SIG_APPLE_DEVICE};
  const uint8_t cats[] = {CAT_AUDIO, CAT_AUDIO, CAT_PHONE, CAT_OTHER_BLE, CAT_PHONE};
  {
    // Flipper-style burst: 12 fake devices in ~3 s, all -44..-49 dBm, mixed types.
    DeviceTable t(store, 96);
    Device* real = nullptr;
    t.ingest(0, mkS(200, -70, 0xAAAA), 500, &real);  // a real phone across the room
    Event ev;
    for (uint8_t i = 0; i < 12; ++i)
      t.ingest(0, spoof(i, (int8_t)(-44 - (i % 6)), sigs[i % 5], cats[i % 5]), 1000 + i * 250);
    CHECK(t.pollSpam(ev));
    CHECK_EQ(ev.type, EV_BLE_SPAM);
    CHECK(ev.count >= 8);
    CHECK(!t.pollSpam(ev));               // reported once
    // Real batches arrive with slightly out-of-order times, and the band drifts:
    // a second burst while the first is still active must not raise a new alert.
    for (uint8_t i = 0; i < 12; ++i)
      t.ingest(0, spoof((uint8_t)(150 + i), (int8_t)(-60 - (i % 4)), sigs[i % 5], cats[i % 5]), 3700 - i * 10);
    CHECK(!t.pollSpam(ev));
    CHECK(t.spamActive(3600));
    CHECK(t.spamActive(4000));
    CHECK(t.spoofedTotal() >= 11u);
    CHECK_EQ(t.countNowAll(4000), 1u);   // only the real phone counts
    CHECK_EQ(t.total(CAT_AUDIO), 0u);
    CHECK_EQ(t.total(CAT_PHONE), 1u);
    // More fakes in the same band while active: absorbed silently.
    Device* d = nullptr;
    t.ingest(0, spoof(50, -46, SIG_APPLE_AUDIO, CAT_AUDIO), 5000, &d);
    CHECK(d->spoofed);
    CHECK(!t.pollSpam(ev));
    // A genuinely different device (far weaker) is not swept up.
    t.ingest(0, spoof(51, -80, SIG_APPLE_AUDIO, CAT_AUDIO), 5200, &d);
    CHECK(!d->spoofed);
    CHECK_EQ(t.total(CAT_AUDIO), 1u);
    // After the hold time a new burst is a new event.
    for (uint8_t i = 0; i < 10; ++i)
      t.ingest(0, spoof((uint8_t)(100 + i), -45, sigs[i % 5], cats[i % 5]), 40000 + i * 200);
    CHECK(t.pollSpam(ev));
  }
  {
    // Same burst, but sent with the *public* address type (what the Flipper does).
    DeviceTable t(store, 96);
    Event ev;
    for (uint8_t i = 0; i < 12; ++i) {
      Sighting s = spoof(i, (int8_t)(-50 - (i % 5)), sigs[i % 5], cats[i % 5]);
      s.flags = 0;
      t.ingest(0, s, 1000 + i * 250);
    }
    CHECK(t.pollSpam(ev));
    CHECK_EQ(t.countNowAll(4500), 0u);
  }
  {
    // One Flipper hopping addresses collapses into one device.
    DeviceTable t(store, 96);
    Device* f = nullptr;
    for (uint8_t i = 0; i < 6; ++i) {
      Sighting s = spoof((uint8_t)(200 + i), (int8_t)(-50 - i), SIG_FLIPPER, CAT_PENTEST);
      s.conf = 85;
      t.ingest(0, s, 1000 + i * 5000, &f);
    }
    CHECK_EQ(t.countNow(CAT_PENTEST, 26000), 1u);
    CHECK_EQ(t.total(CAT_PENTEST), 1u);
    CHECK_EQ(f->rotations, 5);
    // A second Flipper on the other side of the building stays separate.
    Sighting other = spoof(250, -88, SIG_FLIPPER, CAT_PENTEST);
    other.conf = 85;
    t.ingest(0, other, 27000);
    CHECK_EQ(t.countNow(CAT_PENTEST, 27000), 2u);
  }
  {
    // Replay of Rob's real Flipper capture (first ~4 s of BLE Spam, public address type).
    DeviceTable t(store, 96);
    Event ev;
    struct R { uint32_t ms; int8_t rssi; uint16_t sig; uint8_t cat; };
    const R cap[] = {
      {8000, -57, SIG_FAST_PAIR, CAT_AUDIO}, {8000, -45, SIG_FAST_PAIR, CAT_AUDIO}, {8000, -46, SIG_FAST_PAIR, CAT_AUDIO},
      {9000, -53, SIG_APPLE_AUDIO, CAT_AUDIO}, {9000, -50, SIG_FAST_PAIR, CAT_AUDIO}, {9000, -51, SIG_FAST_PAIR, CAT_AUDIO},
      {9000, -58, SIG_APPLE_DEVICE, CAT_PHONE}, {9000, -52, SIG_APPLE_DEVICE, CAT_PHONE}, {9000, -51, SIG_APPLE_DEVICE, CAT_PHONE},
      {10000, -53, SIG_APPLE_AUDIO, CAT_AUDIO}, {10000, -56, SIG_FAST_PAIR, CAT_AUDIO}, {10000, -71, SIG_FAST_PAIR, CAT_AUDIO},
      {10000, -67, SIG_FAST_PAIR, CAT_AUDIO}, {10000, -53, SIG_APPLE_DEVICE, CAT_PHONE}, {10000, -52, SIG_APPLE_DEVICE, CAT_PHONE},
      {10000, -58, SIG_APPLE_DEVICE, CAT_PHONE},
    };
    uint8_t n = 0;
    bool fired = false;
    uint32_t fired_at = 0;
    for (const R& r : cap) {
      Sighting s = spoof(n++, r.rssi, r.sig, r.cat);
      s.flags = 0;
      t.ingest(0, s, r.ms);
      if (!fired && t.pollSpam(ev)) { fired = true; fired_at = r.ms; }
    }
    CHECK(fired);
    CHECK(fired_at <= 10000);
    CHECK(t.countNowAll(10000) <= 5u);  // stragglers outside the band can remain
  }
  {
    // A crowd: lots of new Apple devices but spread over distance, no burst.
    DeviceTable t(store, 96);
    Event ev;
    for (uint8_t i = 0; i < 20; ++i)
      t.ingest(0, spoof(i, (int8_t)(-40 - i * 3), sigs[i % 5], cats[i % 5]), 1000 + i * 200);
    CHECK(!t.pollSpam(ev));
    CHECK_EQ(t.spoofedTotal(), 0u);
    CHECK_EQ(t.countNowAll(6000), 20u);
  }
  {
    // Same RSSI band but only one type and under the one-type threshold: not spam.
    DeviceTable t(store, 96);
    Event ev;
    for (uint8_t i = 0; i < 9; ++i) t.ingest(0, spoof(i, -45, SIG_APPLE_DEVICE, CAT_PHONE), 1000 + i * 300);
    CHECK(!t.pollSpam(ev));
  }
  {
    // Consumer categories off, pentest on: fakes arrive as spam probes. Hidden from
    // counts, still caught as a burst; a probe-only device appears once its category is on.
    DeviceTable t(store, 96);
    Event ev;
    for (uint8_t i = 0; i < 10; ++i) {
      Sighting s = spoof(i, -45, sigs[i % 5], cats[i % 5]);
      s.flags |= SF_SPAM_PROBE;
      t.ingest(0, s, 1000 + i * 200);
    }
    CHECK(t.pollSpam(ev));
    CHECK_EQ(t.countNowAll(4000), 0u);
    CHECK_EQ(t.totalAll(), 0u);
    Sighting quiet = spoof(77, -80, SIG_FAST_PAIR, CAT_AUDIO);
    quiet.flags |= SF_SPAM_PROBE;
    Device* d = nullptr;
    t.ingest(0, quiet, 5000, &d);
    CHECK(d->hidden && !d->spoofed);
    CHECK_EQ(t.countNowAll(5000), 0u);
    quiet.flags &= (uint8_t)~SF_SPAM_PROBE;  // audio switched on
    t.ingest(0, quiet, 6000, &d);
    CHECK(!d->hidden);
    CHECK_EQ(t.countNow(CAT_AUDIO, 6000), 1u);
    CHECK_EQ(t.total(CAT_AUDIO), 1u);
  }
  {
    // Mesh router: several BSSIDs collapse to one access point.
    DeviceTable t(store, 96);
    Device* p = nullptr;
    t.ingest(0, ap({0xB6, 0xA7, 0xB9, 0x25, 0xBD, 0x68}, -80), 1000, &p);
    t.ingest(0, ap({0xBA, 0xA7, 0xB9, 0x25, 0xBD, 0x69}, -81), 1100);
    t.ingest(0, ap({0xC2, 0xA7, 0xB9, 0x25, 0xBD, 0x69}, -83), 1200);
    t.ingest(0, ap({0x76, 0x62, 0x8B, 0x2E, 0xE7, 0xD5}, -52), 1300);  // a different router
    Device* a = nullptr;
    t.ingest(0, ap({0x6E, 0x62, 0x8B, 0x2E, 0xE7, 0xD4}, -52), 1400, &a);
    CHECK_EQ(p->aliases, 2);
    CHECK(a->alias_of != 0);
    CHECK_EQ(t.countNow(CAT_OTHER_WIFI, 1500), 2u);
    CHECK_EQ(t.total(CAT_OTHER_WIFI), 2u);
    CHECK_EQ(t.used(), 5u);
  }
}

// Prints reference frames as hex so tools/test_ssproto.py can check the Python
// decoder against the C++ encoder byte for byte.
static void printHex(const char* label, const uint8_t* p, size_t n) {
  printf("%s ", label);
  for (size_t i = 0; i < n; ++i) printf("%02x", p[i]);
  printf("\n");
}

static void emitGolden() {
  uint8_t pl[kMaxPayload], fr[kMaxFrame];
  Hello h;
  const uint8_t mac[6] = {0x24, 0x58, 0x7C, 0x11, 0x22, 0x33};
  memcpy(h.mac, mac, 6); h.fw_version = 0x0102; h.hw = HW_C5_DEVKIT; h.caps = CAP_WIFI24 | CAP_BLE; h.role_pref = ROLE_BLE;
  h.auth = 0x11223344;
  size_t n = encodeFrame(MSG_HELLO, 0x2233, 1, pl, encodeHello(h, pl, sizeof(pl)), fr, sizeof(fr));
  printHex("hello", fr, n);

  Config c;
  c.cfg_version = 4; c.role = ROLE_WIFI; c.category_mask = kSurveillanceMask; c.channel_mask = kMask24;
  c.dwell_ms = 120; c.report_ms = 1000; c.rssi_floor = -90; c.ble_duty_pct = 50;
  n = encodeFrame(MSG_CONFIG, 0x0001, 2, pl, encodeConfig(c, pl, sizeof(pl)), fr, sizeof(fr));
  printHex("config", fr, n);

  Sighting s[2];
  s[0] = mkS(7, -61, 0x3C7E0001, CAT_TRACKER, 80); s[0].sig_id = SIG_FINDMY_SEPARATED; s[0].company = 0x004C;
  s[0].hits = 12; s[0].channel = 37; s[0].age_ms = 250;
  s[1] = mkS(9, -88, 0, CAT_PHONE, 30); s[1].kind = KIND_WIFI_PROBE_REQ; s[1].company = 0xFFFF; s[1].channel = 149;
  n = encodeFrame(MSG_SIGHTINGS, 0x2233, 65535, pl, encodeSightings(s, 2, pl, sizeof(pl)), fr, sizeof(fr));
  printHex("sightings", fr, n);

  uint8_t cobs[cobsMaxEncoded(kMaxFrame)];
  const size_t e = cobsEncode(fr, n, cobs, sizeof(cobs));
  printHex("sightings_cobs", cobs, e);
}

static const uint8_t kMeshNodeInfo[] = {0xff, 0xff, 0xff, 0xff, 0xd4, 0xc3, 0xb2, 0xa1, 0x44, 0x33, 0x22, 0x11, 0x63, 0x08, 0x00, 0x12, 0xe6, 0x17, 0x40, 0xb7, 0xbe, 0x64, 0x5f, 0xc1, 0x50, 0x1a, 0xfe, 0x34, 0xa7, 0xd2, 0xbc, 0x16, 0xe7, 0x04, 0xa9, 0x2f, 0x07, 0x4a, 0x3f, 0xe9, 0x7f, 0x34, 0x71, 0x55, 0xea, 0x14, 0xc4, 0x17, 0xfd, 0x59, 0xbf, 0x7c, 0x03, 0x36, 0x13, 0x31};
static const uint8_t kMeshPos[] = {0xff, 0xff, 0xff, 0xff, 0xd4, 0xc3, 0xb2, 0xa1, 0x88, 0x77, 0x66, 0x55, 0x63, 0x08, 0x00, 0x12, 0xf9, 0x0a, 0x9c, 0x16, 0x2f, 0x83, 0xdd, 0xc2, 0xcb, 0xe6, 0x23, 0x1b, 0x25, 0xba, 0x69, 0x9f, 0x9f};

static void testSubGhz() {
  puts("sub-GHz watch");
  SubGhzWatch w;
  uint32_t t = 0;
  Event ev;
  for (int i = 0; i < 20; ++i, t += 600) w.onLook(0, -100, -97, t);   // quiet 315 MHz
  CHECK(w.floorDbm(0) <= -99 && w.floorDbm(0) >= -101);
  CHECK_EQ(w.bursts(0), 0u);
  w.onLook(0, -99, -62, t); t += 600;          // a fob press
  w.onLook(0, -100, -98, t); t += 600;
  w.onLook(0, -100, -60, t); t += 600;          // another
  CHECK_EQ(w.bursts(0), 2u);
  SubGhzBurst r[8];
  CHECK(w.recent(r, 8) == 2 && r[0].peak == -60 && r[1].peak == -62);
  CHECK(!w.poll(ev));
  // A jammer: the whole band loud for seconds
  for (int i = 0; i < 8; ++i, t += 600) w.onLook(0, -55, -52, t);
  CHECK(w.poll(ev));
  CHECK(ev.type == EV_RF_JAM && ev.channel == 0 && ev.count == 31500 && ev.dst[0] >= 40);
  CHECK(w.jammed(0, t));
  CHECK(w.floorDbm(0) < -90);                  // the jammer didn't become the floor
  for (int i = 0; i < 8; ++i, t += 600) w.onLook(0, -55, -52, t);
  CHECK(!w.poll(ev));                          // once a minute
  w.onLook(0, -100, -98, t);
  CHECK(!w.jammed(0, t));
  // Band numbers stay fixed (alerts, saved settings); the screen order is by frequency.
  CHECK(kSubGhzBandList[0].mhz == 315.00f && kSubGhzBandList[5].mhz == 314.35f);
  bool seen[kSubGhzBands] = {false};
  for (uint8_t i = 0; i < kSubGhzBands; ++i) {
    CHECK(kSubGhzBandOrder[i] < kSubGhzBands && !seen[kSubGhzBandOrder[i]]);
    seen[kSubGhzBandOrder[i]] = true;
    if (i) CHECK(kSubGhzBandList[kSubGhzBandOrder[i - 1]].mhz < kSubGhzBandList[kSubGhzBandOrder[i]].mhz);
  }
  // A Toyota fob at 314.3 MHz is inside the 314.35 look's window (650 kHz wide), not 315's.
  CHECK(314.30f > kSubGhzBandList[5].mhz - 0.325f && 314.30f < kSubGhzBandList[0].mhz - 0.325f);

  // Bursts in the device list: one row per band (Sub-GHz category).
  const Sighting sg = subGhzSighting(5, -71);
  CHECK(sg.kind == KIND_SUBGHZ && radioClass(sg.kind) == 5 && sg.category == CAT_SUBGHZ);
  CHECK(sg.channel == 5 && sg.rssi_max == -71 && sg.sig_id == SIG_SUBGHZ_BAND + 5);
  CHECK_EQ(subGhzKhz(sg.addr), 314350u);
  CHECK_EQ(subGhzKhz(subGhzSighting(3, -80).addr), 433920u);
  CHECK(subGhzKhz(subGhzSighting(0, -80).addr) != subGhzKhz(sg.addr));
  const uint8_t notOne[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  CHECK_EQ(subGhzKhz(notOne), 0u);
  CHECK(strcmp(sigName(SIG_SUBGHZ_BAND + 5), "Sub-GHz bursts: Toyota / Lexus fobs") == 0);
  CHECK(strcmp(categoryName(CAT_SUBGHZ), "subghz") == 0 && strcmp(categoryLabel(CAT_SUBGHZ), "Sub-GHz") == 0);
  CHECK(kAllCategoriesMask & catBit(CAT_SUBGHZ));
  CHECK_EQ(planForCategories(catBit(CAT_SUBGHZ)).ble_duty_pct, 0);   // needs neither BLE nor Wi-Fi time
  CHECK_EQ(planForCategories(catBit(CAT_SUBGHZ)).channel_mask, 0ull);
  {  // over the wire and into the master's table
    uint8_t buf[64];
    const size_t n = encodeSightings(&sg, 1, buf, sizeof(buf));
    Sighting back;
    size_t cnt = 0;
    CHECK(n > 0 && decodeSightings(buf, n, &back, 1, &cnt) && cnt == 1);
    CHECK(back.kind == KIND_SUBGHZ && back.category == CAT_SUBGHZ && memcmp(back.addr, sg.addr, 6) == 0);
    static Device store[4];
    DeviceTable t(store, 4);
    CHECK_EQ(t.ingest(3, sg, 1000), DeviceTable::NEW);
    t.ingest(3, subGhzSighting(5, -65), 2000);   // another burst on the same band: same row
    CHECK(t.ingest(3, subGhzSighting(0, -80), 3000) == DeviceTable::NEW);   // 315 MHz: its own row
    Device* d = t.find(sg.addr, 5);
    CHECK(d && d->hits == 2 && d->category == CAT_SUBGHZ && d->rssi_max == -65);
  }
}

static void testEvilTwin() {
  puts("evil twin / karma");
  FloodMonitor fm;
  auto resp = [](const char* ssid, uint8_t last) {
    Frame f(5, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, {0x02, 0xAA, 0xBB, 0xCC, 0xDD, last});
    f.ssid(ssid);
    WifiFrame w;
    parseWifiFrame(f.b.data(), f.b.size(), w);
    return w;
  };
  Event ev;
  // A normal router answering with its own name, over and over: nothing.
  for (int i = 0; i < 20; ++i) fm.onApFrame(resp("Neyhard-Home", 1), 6, 1000 + i * 100);
  CHECK(!fm.poll(ev, 4000));
  // A Pineapple answering whatever phones ask for, from one BSSID.
  fm.onApFrame(resp("Starbucks WiFi", 9), 6, 5000);
  fm.onApFrame(resp("Hilton Honors", 9), 6, 5200);
  CHECK(!fm.poll(ev, 5300));
  fm.onApFrame(resp("Neyhard-Home", 9), 6, 5400);
  CHECK(fm.poll(ev, 5500));
  CHECK(ev.type == EV_EVIL_TWIN && ev.count == 3 && ev.src[5] == 9 && ev.channel == 6);
  fm.onApFrame(resp("xfinitywifi", 9), 6, 6000);
  CHECK(!fm.poll(ev, 6100));   // once per 5 minutes per radio
}

static void testWifiSpam() {
  puts("wifi beacon spam");
  static Device store[256];
  auto beacon = [](uint8_t a0, uint8_t n, int8_t rssi) {
    Sighting x;
    x.addr[0] = a0; x.addr[1] = 0x10; x.addr[2] = (uint8_t)(n * 37); x.addr[3] = n; x.addr[4] = (uint8_t)(n * 11); x.addr[5] = (uint8_t)(n * 3);
    x.kind = KIND_WIFI_BEACON; x.channel = 6; x.rssi_last = x.rssi_max = rssi; x.hits = 1;
    x.category = CAT_OTHER_WIFI; x.sig_id = SIG_ACCESS_POINT; x.conf = 5;
    if (a0 & 0x02) x.flags |= SF_RANDOM_ADDR;
    return x;
  };
  {
    DeviceTable t(store, 256);
    // A neighbourhood: 20 real routers, fixed addresses, all sorts of levels, over 4 s
    for (uint8_t i = 0; i < 20; ++i) t.ingest(0, beacon(0x00, i, (int8_t)(-40 - i * 3)), 1000 + i * 200);
    Event ev;
    CHECK(!t.pollSpam(ev));
    CHECK_EQ(t.total(CAT_OTHER_WIFI), 20u);
  }
  {
    DeviceTable t(store, 256);
    // A Flipper / Marauder beacon flood: 30 random BSSIDs in 3 s at -45..-49 dBm
    for (uint8_t i = 0; i < 30; ++i) t.ingest(0, beacon(0x02 | (uint8_t)(i << 2), (uint8_t)(100 + i), (int8_t)(-45 - (i % 5))), 1000 + i * 100);
    Event ev;
    CHECK(t.pollSpam(ev));
    CHECK_EQ(ev.type, EV_WIFI_SPAM);
    CHECK(ev.count >= 10);
    CHECK(t.total(CAT_OTHER_WIFI) < 10u);   // the fakes don't count
    CHECK(t.wifiSpamActive(4000));
  }
}

static void testAesMesh() {
  puts("aes + meshtastic default channel");
  // FIPS-197 C.1 / C.3
  uint8_t key[32], pt[16], out[16];
  for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
  for (int i = 0; i < 16; ++i) pt[i] = (uint8_t)(i * 0x11);
  aesEncryptBlock(key, 16, pt, out);
  const uint8_t c128[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  CHECK(memcmp(out, c128, 16) == 0);
  aesEncryptBlock(key, 32, pt, out);
  const uint8_t c256[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
  CHECK(memcmp(out, c256, 16) == 0);
  // SP 800-38A F.5.1 CTR-AES128, first block
  const uint8_t k2[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  uint8_t iv[16];
  for (int i = 0; i < 16; ++i) iv[i] = (uint8_t)(0xf0 + i);
  const uint8_t p2[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
  const uint8_t c2[16] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce};
  aesCtr(k2, 16, iv, p2, out, 16);
  CHECK(memcmp(out, c2, 16) == 0);

  CHECK_EQ(meshChannelHash("LongFast"), 8);
  MeshHeader h;
  CHECK(parseMeshtastic(kMeshNodeInfo, sizeof(kMeshNodeInfo), h));
  uint8_t plain[256];
  const size_t n = sizeof(kMeshNodeInfo) - 16;
  CHECK(!meshDecryptDefault(h, "MediumFast", kMeshNodeInfo + 16, n, plain));   // other channel hash
  CHECK(meshDecryptDefault(h, "LongFast", kMeshNodeInfo + 16, n, plain));
  MeshInfo mi;
  CHECK(decodeMeshData(plain, n, mi));
  CHECK_EQ(mi.port, (uint32_t)MP_NODEINFO);
  CHECK(strcmp(mi.long_name, "Rob's Truck \xF0\x9F\x9A\x9A") == 0);
  CHECK(strcmp(mi.short_name, "RTK") == 0);
  CHECK_EQ(mi.hw_model, 43u);

  MeshHeader h2;
  CHECK(parseMeshtastic(kMeshPos, sizeof(kMeshPos), h2));
  const size_t n2 = sizeof(kMeshPos) - 16;
  CHECK(meshDecryptDefault(h2, "LongFast", kMeshPos + 16, n2, plain));
  MeshInfo mp;
  CHECK(decodeMeshData(plain, n2, mp));
  CHECK(mp.has_pos && mp.lat_e7 == 418002681 && mp.lon_e7 == -772516868 && mp.alt_m == 412);

  // Garbage never crashes or over-reads
  srand(7);
  for (int t = 0; t < 3000; ++t) {
    uint8_t junk[64];
    const size_t l = (size_t)(rand() % 65);
    for (size_t i = 0; i < l; ++i) junk[i] = (uint8_t)rand();
    MeshInfo j;
    decodeMeshData(junk, l, j);
  }
}

static void testLora() {
  puts("lora: meshtastic + lorawan");
  CHECK(kMeshLongFast.mhz > 906.87f && kMeshLongFast.mhz < 906.88f && kMeshLongFast.sf == 11 && kMeshLongFast.sync == 0x2B);
  CHECK(kMeshMediumFast.mhz > 913.12f && kMeshMediumFast.mhz < 913.13f && kMeshMediumFast.sf == 9);
  CHECK(loraWanUplinkMhz(0) > 903.89f && loraWanUplinkMhz(7) > 905.29f && loraWanUplinkMhz(7) < 905.31f);

  // Broadcast from !a1b2c3d4, packet id 0x01020304, hop_start 3, hop_limit 1 (2 hops), channel hash 0x08
  const uint8_t mesh[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xD4, 0xC3, 0xB2, 0xA1, 0x04, 0x03, 0x02, 0x01,
                          (uint8_t)((3 << 5) | 1), 0x08, 0x00, 0x5E, 0xAA, 0xBB};
  MeshHeader h;
  CHECK(parseMeshtastic(mesh, sizeof(mesh), h));
  CHECK_EQ(h.to, 0xFFFFFFFFu);
  CHECK_EQ(h.from, 0xA1B2C3D4u);
  CHECK_EQ(h.id, 0x01020304u);
  CHECK(h.hop_start == 3 && h.hop_limit == 1 && !h.via_mqtt && !h.want_ack);
  CHECK(h.channel_hash == 0x08 && h.relay_node == 0x5E);
  Sighting s = makeMeshSighting(h, -97);
  CHECK(s.kind == KIND_LORA_MESHTASTIC && s.category == CAT_LORA && s.sig_id == SIG_MESHTASTIC);
  CHECK(s.addr[0] == 0 && s.addr[1] == 0 && s.addr[2] == 0xA1 && s.addr[5] == 0xD4);
  CHECK_EQ(s.channel, 2);           // two hops away
  CHECK_EQ(radioClass(s.kind), 4);
  uint8_t direct[sizeof(mesh)];
  memcpy(direct, mesh, sizeof(mesh));
  direct[12] = (uint8_t)((3 << 5) | 3);  // limit == start: heard directly
  CHECK(parseMeshtastic(direct, sizeof(direct), h) && makeMeshSighting(h, -80).channel == 0);
  direct[12] = (uint8_t)((3 << 5) | 3 | 0x10);  // via MQTT
  CHECK(parseMeshtastic(direct, sizeof(direct), h) && makeMeshSighting(h, -80).channel == kLoraViaMqtt);
  CHECK(!parseMeshtastic(mesh, 15, h));                       // too short
  uint8_t bad[sizeof(mesh)];
  memcpy(bad, mesh, sizeof(mesh));
  bad[12] = (uint8_t)((1 << 5) | 5);                          // limit > start: not Meshtastic
  CHECK(!parseMeshtastic(bad, sizeof(bad), h));
  memset(bad + 4, 0, 4);
  CHECK(!parseMeshtastic(bad, sizeof(bad), h));               // sender 0

  // LoRaWAN unconfirmed uplink from DevAddr 0x260B1234, FCnt 7, FPort 1, 2 bytes, MIC
  const uint8_t up[] = {0x40, 0x34, 0x12, 0x0B, 0x26, 0x00, 0x07, 0x00, 0x01, 0xAB, 0xCD, 1, 2, 3, 4};
  LoraWanFrame f;
  CHECK(parseLoraWan(up, sizeof(up), f));
  CHECK(f.mtype == 2 && f.dev_addr == 0x260B1234u && f.fcnt == 7);
  s = makeLoraWanSighting(f, -110);
  CHECK(s.kind == KIND_LORAWAN && s.sig_id == SIG_LORAWAN_DEVICE && s.addr[2] == 0x26 && s.addr[5] == 0x34);
  // Join request: JoinEUI 70B3D57ED0000001, DevEUI 0004A30B001C0530 (little-endian on air)
  const uint8_t join[] = {0x00, 0x01, 0x00, 0x00, 0xD0, 0x7E, 0xD5, 0xB3, 0x70,
                          0x30, 0x05, 0x1C, 0x00, 0x0B, 0xA3, 0x04, 0x00, 0x11, 0x22, 1, 2, 3, 4};
  CHECK(parseLoraWan(join, sizeof(join), f));
  CHECK(f.mtype == 0 && f.dev_eui[0] == 0x00 && f.dev_eui[1] == 0x04 && f.dev_eui[7] == 0x30 && f.join_eui[0] == 0x70);
  s = makeLoraWanSighting(f, -100);
  CHECK(s.sig_id == SIG_LORAWAN_JOIN && s.addr[0] == 0xA3 && s.addr[5] == 0x30);
  CHECK(!parseLoraWan(join, 22, f));                           // wrong join length
  const uint8_t down[] = {0x60, 0x34, 0x12, 0x0B, 0x26, 0x00, 0x07, 0x00, 1, 2, 3, 4};
  CHECK(!parseLoraWan(down, sizeof(down), f));                 // downlink: not ours
  const uint8_t major1[] = {0x41, 0x34, 0x12, 0x0B, 0x26, 0x00, 0x07, 0x00, 1, 2, 3, 4};
  CHECK(!parseLoraWan(major1, sizeof(major1), f));             // unknown major version
  const uint8_t fopts[] = {0x40, 0x34, 0x12, 0x0B, 0x26, 0x0F, 0x07, 0x00, 1, 2, 3, 4};
  CHECK(!parseLoraWan(fopts, sizeof(fopts), f));               // FOpts longer than the frame

  // Meshtastic US slots (djb2 of the default channel name), as the Meshtastic docs list them
  CHECK_EQ(meshUsSlots(250.0f), 104);
  CHECK_EQ(meshUsSlots(125.0f), 208);
  CHECK_EQ(meshUsDefaultSlot("LongFast", 250.0f), 20);
  CHECK_EQ(meshUsDefaultSlot("MediumFast", 250.0f), 45);
  CHECK_EQ(meshUsDefaultSlot("ShortFast", 250.0f), 68);
  LoraPreset lp;
  CHECK(loraModePreset(LM_LONGFAST, 0, lp) && lp.mhz > 906.87f && lp.mhz < 906.88f && lp.sf == 11 && lp.sync == 0x2B);
  CHECK(loraModePreset(LM_MEDIUMFAST, 0, lp) && lp.mhz > 913.12f && lp.mhz < 913.13f && lp.sf == 9);
  CHECK(loraModePreset(LM_SHORTFAST, 0, lp) && lp.mhz > 918.87f && lp.mhz < 918.88f && lp.sf == 7);
  CHECK(loraModePreset(LM_LONGSLOW, 0, lp) && lp.bw_khz == 125.0f && lp.sf == 12 && lp.cr == 8);
  CHECK(loraModePreset(LM_LONGFAST, 1, lp) && lp.mhz > 902.12f && lp.mhz < 902.13f);    // slot 1 = 902.125
  CHECK(loraModePreset(LM_LONGFAST, 104, lp) && lp.mhz > 927.87f && lp.mhz < 927.88f);  // the last one
  CHECK(loraModePreset(LM_LONGFAST, 200, lp) && lp.mhz > 906.87f && lp.mhz < 906.88f);  // out of range: default
  CHECK(strcmp(lp.name, "LongFast") == 0);
  CHECK(loraModePreset(LM_MESHCORE, 5, lp) && lp.mhz > 910.52f && lp.mhz < 910.53f && lp.bw_khz == 62.5f && lp.sync == 0x12);
  CHECK(!loraModePreset(LM_AUTO, 0, lp) && !loraModePreset(LM_LORAWAN, 0, lp));
  uint8_t lm = 99;
  CHECK(parseLoraMode("MeshCore", lm) && lm == LM_MESHCORE);
  CHECK(parseLoraMode("longfast", lm) && lm == LM_LONGFAST);
  CHECK(parseLoraMode("auto", lm) && lm == LM_AUTO);
  CHECK(!parseLoraMode("longfas", lm) && !parseLoraMode("longfastx", lm) && !parseLoraMode("", lm));
  for (uint8_t m = 0; m < LM_COUNT; ++m) CHECK(parseLoraMode(loraModeName(m), lm) && lm == m);
  CHECK(loraModeIsMeshtastic(LM_SHORTTURBO) && !loraModeIsMeshtastic(LM_MESHCORE) && !loraModeIsMeshtastic(LM_AUTO));

  // MeshCore advert, flooded through 2 repeaters (1-byte hashes): a repeater named
  // "Hilltop RPT" at 40.123456,-75.654321
  uint8_t mc[200];
  size_t mn = 0;
  mc[mn++] = (uint8_t)((MCT_ADVERT << 2) | MCR_FLOOD);
  mc[mn++] = 0x02;            // path: 2 hops, 1-byte hashes
  mc[mn++] = 0xA1; mc[mn++] = 0xB2;
  const size_t adv = mn;
  for (int i = 0; i < 32; ++i) mc[mn++] = (uint8_t)(0x10 + i);  // public key
  mc[mn++] = 0x78; mc[mn++] = 0x56; mc[mn++] = 0x34; mc[mn++] = 0x12;  // timestamp
  for (int i = 0; i < 64; ++i) mc[mn++] = 0xEE;                  // signature
  mc[mn++] = 0x80 | 0x10 | MCN_REPEATER;
  const int32_t la6 = 40123456, lo6 = -75654321;
  for (int i = 0; i < 4; ++i) mc[mn++] = (uint8_t)((uint32_t)la6 >> (8 * i));
  for (int i = 0; i < 4; ++i) mc[mn++] = (uint8_t)((uint32_t)lo6 >> (8 * i));
  memcpy(mc + mn, "Hilltop RPT", 11); mn += 11;
  MeshCorePacket mp;
  CHECK(parseMeshCore(mc, mn, mp));
  CHECK(mp.type == MCT_ADVERT && mp.route == MCR_FLOOD && mp.hops == 2 && mp.hash_size == 1 && mp.payload_off == adv);
  MeshCoreAdvert ma;
  CHECK(parseMeshCoreAdvert(mc + mp.payload_off, mp.payload_len, ma));
  CHECK(ma.node_type == MCN_REPEATER && ma.has_pos && ma.lat_e6 == la6 && ma.lon_e6 == lo6);
  CHECK(strcmp(ma.name, "Hilltop RPT") == 0 && ma.timestamp == 0x12345678u && ma.pubkey[0] == 0x10);
  s = makeMeshCoreSighting(mp, ma, -101);
  CHECK(s.kind == KIND_LORA_MESHCORE && s.sig_id == SIG_MESHCORE_REPEATER && s.category == CAT_LORA);
  CHECK(s.addr[0] == 0x10 && s.addr[5] == 0x15 && s.channel == 2 && radioClass(s.kind) == 4);
  CHECK(strcmp(sigName(SIG_MESHCORE_REPEATER), "MeshCore repeater") == 0);
  // Transport-coded flood: 4 bytes of codes before the path length
  uint8_t tf[8] = {(uint8_t)((MCT_GRP_TXT << 2) | MCR_TRANSPORT_FLOOD), 1, 2, 3, 4, 0x00, 0x11, 0x22};
  CHECK(!parseMeshCore(tf, 8, mp));  // group message too short to be real
  uint8_t tf3[9] = {(uint8_t)((MCT_GRP_TXT << 2) | MCR_TRANSPORT_FLOOD), 1, 2, 3, 4, 0x00, 0x11, 0x22, 0x33};
  CHECK(parseMeshCore(tf3, 9, mp) && mp.payload_off == 6 && mp.payload_len == 3);
  // 2-byte path hashes: 0x41 = 1 hop of 2 bytes
  uint8_t h2[6] = {(uint8_t)((MCT_TXT_MSG << 2) | MCR_FLOOD), 0x41, 0xAA, 0xBB, 0x01, 0x02};
  CHECK(parseMeshCore(h2, 6, mp) && mp.hash_size == 2 && mp.hops == 1 && mp.payload_off == 4);
  // Not MeshCore: version bits set, reserved hash size, path past the end, unknown type
  uint8_t bad1[4] = {0x40 | 0x11, 0x00, 1, 2};
  CHECK(!parseMeshCore(bad1, 4, mp));
  uint8_t bad2[4] = {0x11, 0xC0, 1, 2};
  CHECK(!parseMeshCore(bad2, 4, mp));
  uint8_t bad3[4] = {0x11, 0x05, 1, 2};
  CHECK(!parseMeshCore(bad3, 4, mp));
  uint8_t bad4[4] = {(uint8_t)(0x0D << 2) | 1, 0x00, 1, 2};
  CHECK(!parseMeshCore(bad4, 4, mp));
  // Adverts: unknown node type, too short, control character in the name
  uint8_t ad[sizeof(mc)];
  memcpy(ad, mc + adv, mn - adv);
  const size_t an = mn - adv;
  ad[100] = 0x80 | 0x07;
  CHECK(!parseMeshCoreAdvert(ad, an, ma));
  ad[100] = 0x80 | MCN_CHAT;  // no position: the name starts right after the flags
  memcpy(ad + 101, "Rob T1000", 9);
  CHECK(parseMeshCoreAdvert(ad, 110, ma) && ma.node_type == MCN_CHAT && !ma.has_pos && strcmp(ma.name, "Rob T1000") == 0);
  CHECK(!parseMeshCoreAdvert(ad, 100, ma));
  ad[101] = 0x07;
  CHECK(!parseMeshCoreAdvert(ad, 110, ma));

  // CONFIG carries the LoRa choice; an older master's CONFIG (without it) means auto
  Config lc;
  lc.cfg_version = 3;
  lc.lora_mode = LM_MESHCORE;
  lc.lora_slot = 0;
  uint8_t lb[kMaxConfigSize];
  size_t ln = encodeConfig(lc, lb, sizeof(lb));
  Config ld;
  CHECK(decodeConfig(lb, ln, ld) && ld.lora_mode == LM_MESHCORE && ld.lora_slot == 0);
  lc.lora_mode = LM_LONGFAST;
  lc.lora_slot = 33;
  ln = encodeConfig(lc, lb, sizeof(lb));
  CHECK(decodeConfig(lb, ln, ld) && ld.lora_mode == LM_LONGFAST && ld.lora_slot == 33);
  CHECK(decodeConfig(lb, ln - 2, ld) && ld.lora_mode == LM_AUTO && ld.lora_slot == 0);
}

// A drive: every minute the access points around us are new ones; a tracker rides
// along, a passer-by is seen once, a parked car's device stays where it was.
static Sighting fsAp(uint32_t n) {
  Sighting s;
  // Distinct routers: differ in bytes 1-2 (AP grouping merges BSSIDs that share bytes 1-4)
  const uint8_t a[6] = {0x10, (uint8_t)(n >> 8), (uint8_t)n, 0x30, 0x40, 0x50};
  memcpy(s.addr, a, 6);
  s.kind = KIND_WIFI_BEACON; s.category = CAT_OTHER_WIFI; s.sig_id = SIG_ACCESS_POINT; s.conf = 5;
  s.rssi_last = s.rssi_max = -70; s.hits = 1; s.company = 0xFFFF;
  return s;
}
static Sighting fsBle(uint8_t n, uint8_t cat, uint16_t sig) {
  Sighting s;
  const uint8_t a[6] = {0xC4, 0x00, 0x00, 0x00, 0x00, n};  // random static: doesn't rotate
  memcpy(s.addr, a, 6);
  s.kind = KIND_BLE_ADV; s.category = cat; s.sig_id = sig; s.conf = 80;
  s.flags = SF_RANDOM_ADDR | SF_STATIC_RAND; s.rssi_last = s.rssi_max = -60; s.hits = 1; s.company = 0xFFFF;
  return s;
}

static void testDashViews() {
  puts("dashboard views (devices, detail, events, nodes)");
  static Device store[64];
  DeviceTable t(store, 64);
  const uint32_t now = 200000;
  // 15 devices: conf 10..80, some quiet; one in a switched-off category
  for (uint8_t i = 0; i < 14; ++i) {
    Sighting x = fsBle((uint8_t)(20 + i), i % 2 ? CAT_PHONE : CAT_TRACKER, i % 2 ? SIG_NONE : SIG_TILE);
    x.conf = (uint8_t)(10 + i * 5);
    x.rssi_last = x.rssi_max = (int8_t)(-50 - i);
    t.ingest(0, x, now - i * 1000);
  }
  Sighting off = fsBle(60, CAT_BODYCAM, SIG_AXON_NAME);
  t.ingest(0, off, now);
  const uint16_t cats = (uint16_t)(kAllCategoriesMask & ~catBit(CAT_BODYCAM));
  DashDevices dv;
  buildDeviceView(t, now, cats, 0, dv);
  CHECK_EQ(dv.total, 14);
  CHECK_EQ(dv.n, kDashRows);
  CHECK_EQ(dv.rows[0].conf, 75);                 // best first
  for (uint8_t i = 1; i < dv.n; ++i) CHECK(dv.rows[i - 1].conf >= dv.rows[i].conf);
  DashDevices p2;
  buildDeviceView(t, now, cats, 12, p2);         // last page: 2 rows
  CHECK_EQ(p2.n, 2);
  CHECK_EQ(p2.rows[1].conf, 10);
  buildDeviceView(t, now, cats, 40, p2);         // past the end
  CHECK_EQ(p2.n, 0);
  uint8_t buf[kMaxPayload];
  size_t len = encodeDashDevices(dv, buf, sizeof(buf));
  CHECK(len > 0 && len <= kMaxPayload);
  CHECK_EQ(dashViewOf(buf, len), DV_DEVICES);
  DashDevices back;
  CHECK(decodeDashDevices(buf, len, back));
  CHECK(back.total == 14 && back.n == kDashRows && back.rows[2].id == dv.rows[2].id && strcmp(back.rows[2].text, dv.rows[2].text) == 0);
  CHECK(!decodeDashDevices(buf, len - 1, back));
  DashDevices full;                              // worst case still fits
  full.n = kDashRows;
  for (auto& r : full.rows) memset(r.text, 'x', kDashText);
  CHECK(encodeDashDevices(full, buf, sizeof(buf)) > 0);

  // Detail
  DashDetail dd;
  buildDetail(t, dv.rows[0].id, now, dd);
  CHECK(dd.found && dd.conf == 75 && dd.category == CAT_PHONE);   // i = 13: odd = phone
  len = encodeDashDetail(dd, buf, sizeof(buf));
  DashDetail db;
  CHECK(len > 0 && decodeDashDetail(buf, len, db));
  CHECK(db.found && db.rssi == dd.rssi && memcmp(db.addr, dd.addr, 6) == 0 && strcmp(db.sig, dd.sig) == 0);
  buildDetail(t, 999999, now, dd);
  CHECK(!dd.found);
  len = encodeDashDetail(dd, buf, sizeof(buf));
  CHECK(decodeDashDetail(buf, len, db) && !db.found && db.id == 999999);
  DashDetail bigd; bigd.found = true; memset(bigd.sig, 's', kDashText); memset(bigd.name, 'n', kMaxName);
  CHECK(encodeDashDetail(bigd, buf, sizeof(buf)) > 0);

  // Events: history keeps everything, newest first, wraps at kHistory
  DashAlerts al;
  al.raise(2, CAT_TRACKER, "FOLLOWING: Tile", 1000);
  al.note(1, CAT_NONE, "Node ACDC stopped answering", 2000);
  al.raise(1, CAT_DRONE, "New: drone", 3000);    // notice: banner keeps the alert, history gets it
  DashAlert ba;
  al.fill(ba, 4000);
  CHECK(strcmp(ba.text, "FOLLOWING: Tile") == 0);
  DashEvents ev;
  al.page(0, 10000, ev);
  CHECK(ev.total == 3 && ev.n == 3);
  CHECK(strcmp(ev.ev[0].text, "New: drone") == 0 && ev.ev[0].age_s == 7);
  CHECK(strcmp(ev.ev[2].text, "FOLLOWING: Tile") == 0);
  for (int i = 0; i < 40; ++i) { char b[16]; snprintf(b, sizeof(b), "ev %d", i); al.note(0, 0, b, 20000 + i); }
  al.page(0, 30000, ev);
  CHECK(ev.total == DashAlerts::kHistory && strcmp(ev.ev[0].text, "ev 39") == 0);
  al.page(DashAlerts::kHistory - 2, 30000, ev);
  CHECK(ev.n == 2 && strcmp(ev.ev[1].text, "ev 10") == 0);
  DashEvents bige; bige.n = kDashEvents;
  for (auto& e : bige.ev) memset(e.text, 'e', kDashAlertText);
  len = encodeDashEvents(bige, buf, sizeof(buf));
  CHECK(len > 0 && len <= kMaxPayload);
  DashEvents eb;
  CHECK(decodeDashEvents(buf, len, eb) && eb.n == kDashEvents);

  // Nodes
  DashNodes nd; nd.total = 7; nd.n = kDashNodes;
  for (auto& n : nd.nodes) { memset(n.label, 'l', kDashLabel); n.up_s = 123456; n.link = -40; }
  len = encodeDashNodes(nd, buf, sizeof(buf));
  CHECK(len > 0 && len <= kMaxPayload);
  DashNodes nb;
  CHECK(decodeDashNodes(buf, len, nb) && nb.n == kDashNodes && nb.nodes[5].up_s == 123456 && nb.nodes[0].link == -40);
  CHECK(!decodeDashEvents(buf, len, eb));       // wrong view byte

  // View command round trip (and old 2-byte commands still decode)
  DashCommand vc; vc.cmd = DC_VIEW; vc.arg = DV_DETAIL; vc.offset = 12; vc.id = 0xABCDEF;
  DashCommand vb;
  CHECK(decodeDashCommand(buf, encodeDashCommand(vc, buf, sizeof(buf)), vb));
  CHECK(vb.cmd == DC_VIEW && vb.arg == DV_DETAIL && vb.offset == 12 && vb.id == 0xABCDEF);
  DashCommand tc{DC_TOGGLE_CAT, CAT_DRONE, 0, 0};
  CHECK_EQ(encodeDashCommand(tc, buf, sizeof(buf)), 2u);
}

static void unhex(const char* h, uint8_t* o, size_t n) { for (size_t i = 0; i < n; ++i) sscanf(h + 2 * i, "%2hhx", &o[i]); }

static void testCrypto() {
  puts("crypto: sha-256, hmac, x25519 (RFC vectors + python cryptography)");
  uint8_t o[32], want[32];
  sha256("abc", 3, o);
  unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
  CHECK(memcmp(o, want, 32) == 0);
  sha256("", 0, o);
  unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
  CHECK(memcmp(o, want, 32) == 0);
  {  // long input crossing block boundaries
    std::string m(1000, 'a');
    Sha256 h; h.begin();
    for (size_t i = 0; i < m.size(); i += 7) h.update(m.data() + i, std::min<size_t>(7, m.size() - i));
    uint8_t o2[32]; h.finish(o2);
    sha256(m.data(), m.size(), o);
    CHECK(memcmp(o, o2, 32) == 0);
    unhex("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", want, 32);
    CHECK(memcmp(o, want, 32) == 0);
  }
  hmacSha256((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, o);
  unhex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want, 32);
  CHECK(memcmp(o, want, 32) == 0);
  // RFC 7748 6.1
  uint8_t a[32], b[32], ap[32], bp[32], s1[32], s2[32];
  unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
  unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
  x25519Base(ap, a); x25519Base(bp, b);
  unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, 32);
  CHECK(memcmp(ap, want, 32) == 0);
  unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want, 32);
  CHECK(memcmp(bp, want, 32) == 0);
  x25519(s1, a, bp); x25519(s2, b, ap);
  unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want, 32);
  CHECK(memcmp(s1, want, 32) == 0 && memcmp(s2, want, 32) == 0);
  // Random pairs computed with Python's cryptography (OpenSSL)
  const char* v[3][4] = {
    {"11ed511c43912e6d5c4102733514c0e843350772c6a3649ac077483b2b276788", "cbefa6852f69ed5142c10bbeeadd8a365d53b8a764af896af2cc27ff480db185",
     "e0814eae7e85b6caf3c0c3915a8339b6a56897507c83d3746d82f501bb67c878", "d54d74320f51aa31c4497cfb2fa7a12f88ec842322cd0e19c31823a2ef93696c"},
    {"e1e8533459df87ba24a746ca40e1469cb2cdce9d04b9244f9746c9be1814df9f", "806d2f8d1dad78132bcfb8a3ed23d6689c91d099c3631102d25b9e030f002861",
     "74d92fbd883d7dc0d3f7f8e5b9ceb7c5de93b944f39a167751e59abf2fad0971", "7638275d8dbc1fdd84ccdf9728d8bc1dce15dbaa4d6d06b4f7fd810a86d69a28"},
    {"efab54ac82c2e88e95af74bd252c8746d8b163716e8e5ee6e4a56c46087ae944", "41aee35d8f66e77f8c9f3c0635a67ead4fc5badea71682819b523ab03e8635c1",
     "263e89b889e6d780daeb879037eb8173e143888d3347d80b26c1ea0b0c7e1732", "19cca5e88dc840762d5a491d1ab8387b4b4d44e9d957ef7ccb56e6ce700dd27b"},
  };
  for (auto& t : v) {
    unhex(t[0], a, 32); unhex(t[1], b, 32);
    x25519Base(bp, b); x25519Base(ap, a);
    x25519(s1, a, bp);
    unhex(t[2], want, 32);
    CHECK(memcmp(s1, want, 32) == 0);
    unhex(t[3], want, 32);
    CHECK(memcmp(ap, want, 32) == 0);
  }
  uint8_t x1[4] = {1, 2, 3, 4}, x2[4] = {1, 2, 3, 5};
  CHECK(equal(x1, x1, 4) && !equal(x1, x2, 4));
}

static void testPairing() {
  puts("pairing handshake (and a key-swapping attacker)");
  const uint8_t nodeMac[6] = {0x48, 0xCA, 0x43, 0x3B, 0xAC, 0xDC};
  uint8_t nPriv[32], mPriv[32], ePriv[32], nPub[32], mPub[32], ePub[32];
  for (int i = 0; i < 32; ++i) { nPriv[i] = (uint8_t)(i * 7 + 1); mPriv[i] = (uint8_t)(i * 13 + 5); ePriv[i] = (uint8_t)(i * 3 + 9); }
  x25519Base(nPub, nPriv); x25519Base(mPub, mPriv); x25519Base(ePub, ePriv);

  // 1. Request on the wire
  PairReq rq; memcpy(rq.mac, nodeMac, 6); rq.hw = HW_HELTEC_TRACKER; rq.caps = CAP_BLE | CAP_LORA_915; rq.fw = 1; memcpy(rq.pub, nPub, 32);
  uint8_t buf[kMaxPayload];
  PairReq rq2;
  CHECK(decodePairReq(buf, encodePairReq(rq, buf, sizeof(buf)), rq2));
  CHECK(memcmp(rq2.pub, nPub, 32) == 0 && rq2.hw == HW_HELTEC_TRACKER);
  // 2. Offer
  PairOffer of; memcpy(of.target, nodeMac, 6); memcpy(of.pub, mPub, 32); of.flags = PO_BLINK;
  PairOffer of2;
  CHECK(decodePairOffer(buf, encodePairOffer(of, buf, sizeof(buf)), of2));
  CHECK(of2.flags == PO_BLINK && memcmp(of2.pub, mPub, 32) == 0);
  // Both sides: same session key and code
  PairSecret sm, sn;
  pairDeriveFrom(mPriv, nPub, nPub, mPub, sm);
  pairDeriveFrom(nPriv, mPub, nPub, mPub, sn);
  CHECK(memcmp(sm.k, sn.k, 32) == 0 && sm.code == sn.code && sm.code < 1000000);
  char codeTxt[8];
  pairCodeText(sm.code, codeTxt);
  CHECK(strlen(codeTxt) == 7 && codeTxt[3] == ' ');
  pairCodeText(42, codeTxt);
  CHECK(strcmp(codeTxt, "000 042") == 0);
  // 3. Key, sealed for this node only
  char ckey[33];
  uint8_t rnd[16]; for (int i = 0; i < 16; ++i) rnd[i] = (uint8_t)(0xA0 + i);
  clusterKeyFromRandom(rnd, ckey);
  CHECK(strlen(ckey) == 32 && strncmp(ckey, "a0a1a2", 6) == 0);
  PairKey pk;
  CHECK(pairSeal(sm, nodeMac, ckey, pk));
  CHECK(memcmp(pk.sealed, ckey, 32) != 0);                       // not in the clear
  PairKey pk2;
  size_t len = encodePairKey(pk, buf, sizeof(buf));
  CHECK(len > 0 && decodePairKey(buf, len, pk2));
  char got[kPairKeyMax + 1];
  CHECK(pairOpen(sn, nodeMac, pk2, got));
  CHECK(strcmp(got, ckey) == 0);
  // Wrong node, tampering, wrong session key: refused
  const uint8_t otherMac[6] = {1, 2, 3, 4, 5, 6};
  CHECK(!pairOpen(sn, otherMac, pk2, got));
  PairKey bad = pk2; bad.sealed[3] ^= 1;
  CHECK(!pairOpen(sn, nodeMac, bad, got));
  bad = pk2; bad.tag[0] ^= 1;
  CHECK(!pairOpen(sn, nodeMac, bad, got));
  bad = pk2; bad.len = 31;
  CHECK(!pairOpen(sn, nodeMac, bad, got));
  PairSecret wrong = sn; wrong.k[0] ^= 1;
  CHECK(!pairOpen(wrong, nodeMac, pk2, got));
  // Typed keys still work; too long is refused
  CHECK(pairSeal(sm, nodeMac, "my cluster key", pk) && pairOpen(sn, nodeMac, pk, got) && strcmp(got, "my cluster key") == 0);
  CHECK(!pairSeal(sm, nodeMac, "0123456789012345678901234567890123", pk));
  CHECK(!pairSeal(sm, nodeMac, "", pk));

  // Attacker in the middle swaps both public keys for its own: each side ends up with
  // a key shared with the attacker, and the two codes on the screens don't match.
  PairSecret mSide, nSide;
  pairDeriveFrom(mPriv, ePub, ePub, mPub, mSide);   // master thinks ePub is the node
  pairDeriveFrom(nPriv, ePub, nPub, ePub, nSide);   // node thinks ePub is the master
  CHECK(mSide.code != nSide.code);
  CHECK(memcmp(mSide.k, nSide.k, 32) != 0);
  // and a key the master sealed for "the node" can't be opened by the real node
  CHECK(pairSeal(mSide, nodeMac, ckey, pk));
  CHECK(!pairOpen(nSide, nodeMac, pk, got));

  // Unpair command round trip
  NodeCommand uc; uc.cmd = NC_UNPAIR;
  NodeCommand ub;
  CHECK(decodeCommand(buf, encodeCommand(uc, buf, sizeof(buf)), ub) && ub.cmd == NC_UNPAIR);
}

static void testTouch() {
  puts("touch calibration");
  const TouchTargets tg{20, 300, 20, 220};
  // Panel A: raw X along screen X, raw Y along Y, both increasing
  const int16_t a[3][2] = {{400, 500}, {3600, 520}, {410, 3500}};
  TouchCal c;
  CHECK(touchCalibrate(a, c));
  CHECK_EQ(c.swap, 0);
  int16_t x, y;
  touchMap(c, tg, 2000, 2000, 320, 240, x, y);
  CHECK(x > 150 && x < 170 && y > 110 && y < 130);   // middle
  touchMap(c, tg, 400, 500, 320, 240, x, y);
  CHECK(x == 20 && y == 20);
  touchMap(c, tg, 4095, 4095, 320, 240, x, y);       // past the edge: clamped
  CHECK(x == 319 && y == 239);
  // Panel B (typical CYD): axes swapped and raw Y reversed
  const int16_t b[3][2] = {{3700, 3600}, {3690, 400}, {300, 3610}};
  CHECK(touchCalibrate(b, c));
  CHECK_EQ(c.swap, 1);
  touchMap(c, tg, 3690, 400, 320, 240, x, y);        // tapping the top-right target
  CHECK(x == 300 && y == 20);
  touchMap(c, tg, 300, 3610, 320, 240, x, y);        // bottom-left
  CHECK(x == 20 && y == 220);
  touchMap(c, tg, 300, 400, 320, 240, x, y);         // bottom-right corner: never tapped, still right
  CHECK(x == 300 && y == 220);
  // Three taps in the same spot: rejected
  const int16_t bad[3][2] = {{2000, 2000}, {2010, 2005}, {1990, 2003}};
  CHECK(!touchCalibrate(bad, c));
}

static void testDash() {
  puts("dashboard summary");
  static Device store[64];
  DeviceTable t(store, 64);
  uint32_t now = 100000;
  // A Find My tag away from its owner (alert), a weak tracker (notice), a strong Flock,
  // a phone (not surveillance), a body cam in a switched-off category, and 4 more trackers.
  Sighting tag = fsBle(1, CAT_TRACKER, SIG_FINDMY_SEPARATED); tag.rssi_last = tag.rssi_max = -80;
  Sighting weak = fsBle(2, CAT_TRACKER, SIG_DULT); weak.conf = 45; weak.rssi_last = weak.rssi_max = -40;
  Sighting flock = fsBle(3, CAT_FLOCK, SIG_FLOCK_BLE_NAME); flock.flags |= SF_STRONG_MATCH; flock.rssi_last = flock.rssi_max = -70;
  Sighting phone = fsBle(4, CAT_PHONE, SIG_NONE); phone.conf = 30;
  Sighting cam = fsBle(5, CAT_BODYCAM, SIG_AXON_NAME);
  t.ingest(0, tag, now); t.ingest(0, weak, now); t.ingest(0, flock, now); t.ingest(0, phone, now); t.ingest(0, cam, now);
  for (uint8_t i = 0; i < 4; ++i) {
    Sighting x = fsBle((uint8_t)(10 + i), CAT_TRACKER, SIG_TILE);
    x.rssi_last = x.rssi_max = (int8_t)(-90 + i);
    t.ingest(0, x, now - 20000);   // heard 20 s ago: quiet
  }
  Name nm; memcpy(nm.addr, flock.addr, 6); nm.kind = flock.kind; nm.len = 9; memcpy(nm.name, "Flock-7F6", 9);
  t.setName(nm);

  const uint16_t cats = (uint16_t)(kAllCategoriesMask & ~catBit(CAT_BODYCAM));
  Dash d;
  buildDash(t, now, cats, d);
  CHECK_EQ(d.now[CAT_TRACKER], 6);        // 20 s ago is still inside the 30 s "now" window
  CHECK(d.now[CAT_FLOCK] == 1 && d.now[CAT_PHONE] == 1);
  CHECK_EQ(d.n_threats, kDashThreats);
  CHECK(d.threats[0].category == CAT_FLOCK || d.threats[0].category == CAT_TRACKER);
  // Alerts first (tag, flock) by signal, then notices, then the quiet ones
  CHECK_EQ(d.threats[0].category, CAT_FLOCK);
  CHECK(strcmp(d.threats[0].text, "Flock-7F6") == 0);
  CHECK(d.threats[0].flags & DT_STRONG);
  CHECK_EQ(d.threats[1].rssi, -80);
  CHECK(strstr(d.threats[1].text, "away from owner") || strstr(d.threats[1].text, "Find My"));
  CHECK_EQ(d.threats[2].rssi, -40);
  CHECK(d.threats[3].flags & DT_QUIET);
  CHECK_EQ(d.threats[3].rssi, -87);      // strongest of the quiet Tiles
  for (uint8_t i = 0; i < d.n_threats; ++i) CHECK(d.threats[i].category != CAT_BODYCAM && d.threats[i].category != CAT_PHONE);

  // Following goes to the top
  t.forEachMutable([&](Device& x) { if (x.rssi_last == -90) x.follow_alerted = true; });
  buildDash(t, now, cats, d);
  CHECK(d.threats[0].flags & DT_FOLLOWING);
  CHECK_EQ(d.threats[0].rssi, -90);

  // Gone after a minute
  buildDash(t, now + 61000, cats, d);
  CHECK_EQ(d.n_threats, 0);

  // Round trip on the wire, and it fits one frame
  buildDash(t, now, cats, d);
  d.deploy = 1; d.nodes_online = 2; d.nodes_total = 3;
  DashAlerts al;
  al.raise(2, CAT_TRACKER, "FOLLOWING: Tile tracker", now);
  al.raise(1, CAT_TRACKER, "New: Tile tracker", now + 1000);      // notice doesn't replace the alert
  al.fill(d.alert, now + 5000);
  CHECK(d.alert.seq == 1 && d.alert.level == 2 && d.alert.age_s == 5);
  al.raise(1, CAT_DRONE, "New: drone Remote ID that is a long name", now + 70000);  // a minute later it does
  al.fill(d.alert, now + 70000);
  CHECK(d.alert.seq == 2 && d.alert.level == 1 && strlen(d.alert.text) == kDashAlertText);
  uint8_t buf[kMaxPayload];
  const size_t len = encodeDash(d, buf, sizeof(buf));
  CHECK(len > 0 && len <= kMaxPayload);
  Dash back;
  CHECK(decodeDash(buf, len, back));
  CHECK(back.deploy == 1 && back.nodes_online == 2 && back.cats == cats && back.n_threats == d.n_threats);
  CHECK(back.now[CAT_TRACKER] == d.now[CAT_TRACKER] && back.alert.seq == 2);
  CHECK(strcmp(back.threats[0].text, d.threats[0].text) == 0 && back.threats[0].flags == d.threats[0].flags);
  CHECK(!decodeDash(buf, len - 1, back));
  // Biggest possible dash still fits
  Dash big;
  big.n_threats = kDashThreats;
  for (auto& th : big.threats) memset(th.text, 'x', kDashText);
  memset(big.alert.text, 'y', kDashAlertText);
  CHECK(encodeDash(big, buf, sizeof(buf)) > 0);
  DashCommand dc{DC_TOGGLE_CAT, CAT_TRACKER, 0, 0}, dc2;
  CHECK(decodeDashCommand(buf, encodeDashCommand(dc, buf, sizeof(buf)), dc2));
  CHECK(dc2.cmd == DC_TOGGLE_CAT && dc2.arg == CAT_TRACKER);
}

static void testFollow() {
  puts("following / lingering");
  static Device store[256];
  {
    // Stationary at home: a weak Flock OUI match (your own Raspberry Pi) never lingers,
    // a real Flock SSID does.
    DeviceTable t(store, 256);
    FollowMonitor f;
    f.setDeployment(DEPLOY_STATIONARY);
    Sighting pi = fsBle(40, CAT_FLOCK, SIG_FLOCK_WIFI_OUI_GENERIC); pi.conf = 20;
    Sighting cam = fsBle(41, CAT_FLOCK, SIG_FLOCK_SSID); cam.conf = 95;
    uint32_t piId = 0, camId = 0;
    std::vector<uint32_t> alerts;
    for (uint32_t sec = 0; sec <= 30 * 60; sec += 5) {
      const uint32_t now = 1000 + sec * 1000;
      Device* d;
      t.ingest(0, pi, now, &d); piId = d->id;
      t.ingest(0, cam, now, &d); camId = d->id;
      f.update(t, now);
      FollowAlert a;
      while (f.poll(a)) alerts.push_back(a.device_id);
    }
    CHECK(std::count(alerts.begin(), alerts.end(), piId) == 0);
    CHECK(std::count(alerts.begin(), alerts.end(), camId) == 1);
  }
  {
    DeviceTable t(store, 256);
    FollowMonitor f;
    f.setDeployment(DEPLOY_MOBILE);
    std::vector<uint32_t> alerts;
    const Sighting tracker = fsBle(1, CAT_TRACKER, SIG_FINDMY_SEPARATED);
    const Sighting phone = fsBle(2, CAT_PHONE, SIG_APPLE_DEVICE);  // a passenger's phone
    const Sighting passer = fsBle(3, CAT_AUDIO, SIG_APPLE_AUDIO);  // seen in town once
    uint32_t trackerId = 0, phoneId = 0;
    for (uint32_t sec = 0; sec <= 25 * 60; ++sec) {
      const uint32_t now = 1000 + sec * 1000;
      const uint32_t minute = sec / 60;
      if (sec % 5 == 0) {
        for (uint32_t k = 0; k < 5; ++k) { Device* d; t.ingest(0, fsAp(minute * 100 + k), now, &d); }
        Device* d;
        t.ingest(0, tracker, now, &d); trackerId = d->id;
        t.ingest(0, phone, now, &d); phoneId = d->id;
        if (minute == 2) t.ingest(0, passer, now, &d);
      }
      f.update(t, now);
      FollowAlert a;
      while (f.poll(a)) {
        CHECK(!a.lingering);
        alerts.push_back(a.device_id);
        if (a.device_id == trackerId) CHECK(a.with_us_ms >= 10 * 60000u && a.places >= 3);
      }
    }
    CHECK(f.place() >= 10);  // scenery fully new each minute; snapshot overlap -> a new place every 1-2 min
    CHECK(std::count(alerts.begin(), alerts.end(), trackerId) == 1);  // tracker: once
    CHECK(std::count(alerts.begin(), alerts.end(), phoneId) == 1);    // any device, after 20 min
    CHECK_EQ(alerts.size(), 2u);                                   // not the passer-by
  }
  {
    // "Mobile" but parked: the same APs every minute, so no places and no alerts.
    DeviceTable t(store, 256);
    FollowMonitor f;
    f.setDeployment(DEPLOY_MOBILE);
    size_t n = 0;
    for (uint32_t sec = 0; sec <= 40 * 60; sec += 5) {
      const uint32_t now = 1000 + sec * 1000;
      Device* d;
      for (uint32_t k = 0; k < 6; ++k) t.ingest(0, fsAp(k), now, &d);
      t.ingest(0, fsBle(1, CAT_TRACKER, SIG_FINDMY_SEPARATED), now, &d);
      f.update(t, now);
      FollowAlert a;
      while (f.poll(a)) ++n;
    }
    CHECK_EQ(n, 0u);
    CHECK_EQ(f.place(), 1);
  }
  {
    // Countryside: too few APs to judge, nothing moves on its own. GPS can force it.
    DeviceTable t(store, 256);
    FollowMonitor f;
    f.setDeployment(DEPLOY_MOBILE);
    Device* d;
    t.ingest(0, fsAp(1), 1000, &d);
    f.update(t, 1000);
    f.update(t, 70000);
    CHECK_EQ(f.place(), 1);
    f.notePlaceChange();
    f.update(t, 71000);
    CHECK_EQ(f.place(), 2);
  }
  {
    // Stationary: a Flock camera hanging around 20+ minutes = one "lingering" note;
    // a phone doing the same is normal.
    DeviceTable t(store, 256);
    FollowMonitor f;
    f.setDeployment(DEPLOY_STATIONARY);
    Sighting cam = fsBle(9, CAT_FLOCK, SIG_FLOCK_BLE_NAME);
    std::vector<FollowAlert> got;
    for (uint32_t sec = 0; sec <= 30 * 60; sec += 10) {
      const uint32_t now = 1000 + sec * 1000;
      Device* d;
      t.ingest(0, cam, now, &d);
      t.ingest(0, fsBle(2, CAT_PHONE, SIG_APPLE_DEVICE), now, &d);
      f.update(t, now);
      FollowAlert a;
      while (f.poll(a)) got.push_back(a);
    }
    CHECK_EQ(got.size(), 1u);
    CHECK(got.size() == 1 && got[0].lingering && got[0].with_us_ms >= 20 * 60000u);
    CHECK_EQ(f.place(), 0);
  }
  CHECK(strcmp(deploymentName(DEPLOY_PORTABLE), "portable") == 0);
}

// NMEA sentence with its checksum filled in
static std::string nmea(const char* body) {
  uint8_t x = 0;
  for (const char* p = body; *p; ++p) x ^= (uint8_t)*p;
  char tail[8];
  snprintf(tail, sizeof(tail), "*%02X\r\n", x);
  return std::string("$") + body + tail;
}

static void feedAll(Nmea& g, const std::string& s, uint32_t now) {
  for (char c : s) g.feed(c, now);
}

static void testGps() {
  printf("gps nmea\n");
  Nmea g;
  CHECK_EQ(g.state(0), GPS_SILENT);
  // UC6580 at power-up: talking, no fix
  feedAll(g, nmea("GNGGA,,,,,,0,00,99.99,,,,,,"), 1000);
  feedAll(g, nmea("GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1"), 1000);
  CHECK_EQ(g.state(1000), GPS_SEARCHING);
  CHECK_EQ(g.sentences(), 2u);
  // Bad checksum is ignored
  feedAll(g, "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n", 1100);
  CHECK_EQ(g.badChecksums(), 1u);
  CHECK_EQ(g.state(1100), GPS_SEARCHING);
  // 3D fix
  feedAll(g, nmea("GNGGA,203015.000,4144.6582,N,07718.0412,W,1,09,1.1,412.3,M,-33.9,M,,"), 2000);
  feedAll(g, nmea("GNGSA,A,3,05,13,15,18,,,,,,,,,1.9,1.1,1.5,1"), 2000);
  feedAll(g, nmea("GNGSA,A,1,,,,,,,,,,,,,1.9,1.1,1.5,4"), 2000);   // BeiDou has nothing: still 3D
  CHECK_EQ(g.state(2000), GPS_FIX_3D);
  CHECK_EQ(g.sats(), 9);
  double lat = 0, lon = 0;
  CHECK(g.position(lat, lon));
  CHECK(lat > 41.744 && lat < 41.745 && lon < -77.300 && lon > -77.301);   // Wellsboro
  // 2D
  feedAll(g, nmea("GNGGA,203016.000,4144.6582,N,07718.0412,W,1,04,2.5,,M,,M,,"), 3000);
  feedAll(g, nmea("GNGSA,A,2,05,13,15,18,,,,,,,,,3.0,2.5,1.5,1"), 3000);
  CHECK_EQ(g.state(3000), GPS_FIX_2D);
  // Fix lost, then the receiver goes quiet
  feedAll(g, nmea("GNGGA,203020.000,,,,,0,00,99.99,,,,,,"), 4000);
  CHECK_EQ(g.state(4000), GPS_SEARCHING);
  CHECK_EQ(g.state(9500), GPS_SILENT);
  CHECK_EQ(std::string(gpsStateName(GPS_FIX_3D)), std::string("3D"));
  // Garbage and overlong lines don't break it
  feedAll(g, std::string(300, 'x') + "\n" + nmea("GPGGA,1,4144.0,N,07718.0,W,1,05,1.0,1,M,1,M,,"), 10000);
  CHECK_EQ(g.state(10000), GPS_FIX_3D);

  // Clock from RMC (it comes before the fix, with status V)
  Nmea c;
  uint32_t t = 0;
  CHECK(!c.utc(0, t));
  feedAll(c, nmea("GNRMC,150405.000,V,,,,,,,301026,,,N,V"), 5000);
  CHECK(c.utc(5000, t));
  CHECK_EQ(t, unixTime(2026, 10, 30, 15, 4, 5));
  CHECK(c.utc(17500, t));
  CHECK_EQ(t, unixTime(2026, 10, 30, 15, 4, 17));          // + 12.5 s
  char ts[20];
  formatUtc(unixTime(2026, 10, 30, 15, 4, 5), ts);
  CHECK(strcmp(ts, "2026-10-30 15:04:05") == 0);
  CHECK_EQ(unixTime(1970, 1, 1, 0, 0, 0), 0u);
  CHECK_EQ(unixTime(2000, 3, 1, 0, 0, 0), 951868800u);
  formatUtc(951868799u, ts);
  CHECK(strcmp(ts, "2000-02-29 23:59:59") == 0);
  {
    // Satellites (GSV), which ones are used (GSA), altitude (GGA), speed and course (RMC)
    Nmea g2;
    feedAll(g2, nmea("GPGSV,2,1,05,05,45,120,38,13,70,045,42,15,10,300,25,18,05,200,"), 1000);
    feedAll(g2, nmea("GPGSV,2,2,05,20,30,090,30"), 1000);
    feedAll(g2, nmea("BDGSV,1,1,02,05,60,180,35,08,20,270,"), 1000);
    feedAll(g2, nmea("GNGSA,A,3,05,13,20,,,,,,,,,,1.8,1.0,1.5,1"), 1000);
    feedAll(g2, nmea("GNGSA,A,3,05,,,,,,,,,,,,1.8,1.0,1.5,4"), 1000);
    feedAll(g2, nmea("GNGGA,203015.000,4144.6582,N,07718.0412,W,1,04,1.0,412.3,M,-33.9,M,,"), 1000);
    feedAll(g2, nmea("GNRMC,203015.000,A,4144.6582,N,07718.0412,W,30.5,271.4,301026,,,A,V"), 1000);
    GpsSat sv[16];
    const size_t ns = g2.satellites(sv, 16, 1500);
    CHECK_EQ(ns, 7u);
    int used = 0, bd = 0;
    for (size_t i = 0; i < ns; ++i) {
      if (sv[i].used) ++used;
      if (sv[i].sys == GNSS_BEIDOU) ++bd;
      if (sv[i].sys == GNSS_GPS && sv[i].prn == 13) CHECK(sv[i].elev == 70 && sv[i].az == 45 && sv[i].snr == 42 && sv[i].used);
      if (sv[i].sys == GNSS_GPS && sv[i].prn == 18) CHECK(sv[i].snr == 0 && !sv[i].used);   // in view, not tracked
      if (sv[i].sys == GNSS_BEIDOU && sv[i].prn == 8) CHECK(!sv[i].used);
    }
    CHECK_EQ(used, 4);   // GPS 5, 13, 20 + BeiDou 5
    CHECK_EQ(bd, 2);
    float alt = 0, kmh = 0, crs = 0;
    CHECK(g2.altitude(alt) && alt > 412 && alt < 413);
    CHECK(g2.motion(kmh, crs) && kmh > 56.4f && kmh < 56.6f && crs > 271 && crs < 272);
    CHECK_EQ(g2.satellites(sv, 16, 12000), 0u);   // 10 s later: gone
    GpsInfo gi;
    fillGpsInfo(g2, 1500, gi);
    CHECK(gi.state == GPS_FIX_3D && gi.has_pos && gi.has_alt && gi.alt_m == 412 && gi.has_motion && gi.kmh10 == 565);
    CHECK(gi.n_sat == 7 && gi.hdop10 == 10);
    uint8_t buf[256];
    const size_t len = encodeGpsInfo(gi, buf, sizeof(buf));
    CHECK(len > 0 && len <= kMaxPayload);
    GpsInfo back;
    CHECK(decodeGpsInfo(buf, len, back));
    CHECK(back.n_sat == 7 && back.lat_e7 == gi.lat_e7 && back.course10 == gi.course10 && back.utc == gi.utc);
    CHECK(back.sat[1].prn == gi.sat[1].prn && back.sat[1].used == gi.sat[1].used && back.sat[1].az2 == gi.sat[1].az2);
    CHECK(!decodeGpsInfo(buf, len - 1, back));
    GpsInfo full;
    full.n_sat = kGpsSats;
    CHECK(encodeGpsInfo(full, buf, sizeof(buf)) <= kMaxPayload);
  }
  Nmea c2;                                                 // no date yet (receiver's default 1980): ignored
  feedAll(c2, nmea("GNRMC,000012.000,V,,,,,,,060180,,,N,V"), 100);
  CHECK(!c2.utc(100, t));
}

static void testLog() {
  printf("detection log\n");
  LogRec r;
  r.seq = 7; r.utc = unixTime(2026, 10, 30, 15, 4, 5); r.up_s = 3600;
  r.has_pos = true; r.lat_e7 = 417443636; r.lon_e7 = -773006866;
  r.kind = LK_DEVICE; r.cat = CAT_FLOCK; r.sig = SIG_FLOCK_SSID; r.conf = 95; r.strong = true; r.rssi = -71;
  r.has_mac = true;
  const uint8_t mac[6] = {0x70, 0xC9, 0x4E, 0x7F, 0x68, 0xFF};
  memcpy(r.mac, mac, 6);
  strcpy(r.name, "Flock-7F68FF");
  uint8_t buf[80];
  const size_t n = encodeLogRec(r, buf, sizeof(buf));
  CHECK(n > 0 && n <= 64);
  LogRec b;
  CHECK(decodeLogRec(buf, n, b));
  CHECK(b.seq == 7 && b.utc == r.utc && b.has_pos && b.lat_e7 == r.lat_e7 && b.lon_e7 == r.lon_e7);
  CHECK(b.kind == LK_DEVICE && b.cat == CAT_FLOCK && b.sig == SIG_FLOCK_SSID && b.conf == 95 && b.strong);
  CHECK(b.rssi == -71 && b.has_mac && memcmp(b.mac, mac, 6) == 0 && strcmp(b.name, "Flock-7F68FF") == 0);
  for (size_t cut = 0; cut < n; ++cut) CHECK(!decodeLogRec(buf, cut, b));   // truncated

  char line[256];
  CHECK(formatLogCsv(r, line, sizeof(line)) > 0);
  CHECK_EQ(std::string(line), std::string("2026-10-30 15:04:05,3600,41.7443636,-77.3006866,device,flock,"
                                          "\"Flock camera SSID\",70:C9:4E:7F:68:FF,\"Flock-7F68FF\",-71,95,yes,new\n"));
  // No GPS, a name with a quote and a comma, a signature with a comma in it
  LogRec q;
  q.up_s = 12; q.kind = LK_DEVICE; q.cat = CAT_PHONE; q.sig = SIG_PROBE_RANDOM; q.conf = 50; q.a = 1;
  strcpy(q.name, "Bob's \"TV\", 2");
  CHECK(formatLogCsv(q, line, sizeof(line)) > 0);
  CHECK_EQ(std::string(line), std::string(",12,,,device,phone,\"Phone (Wi-Fi probe, randomized)\",,"
                                          "\"Bob's \"\"TV\"\", 2\",,50,no,upgraded\n"));
  // Events
  LogRec f;
  f.up_s = 99; f.kind = LK_DEAUTH; f.a = 6; f.b = 120; f.rssi = -40;
  formatLogCsv(f, line, sizeof(line));
  CHECK_EQ(std::string(line), std::string(",99,,,deauth_flood,,,,,-40,,,channel=6 frames=120\n"));
  LogRec fo;
  fo.up_s = 5; fo.kind = LK_FOLLOWING; fo.cat = CAT_TRACKER; fo.sig = SIG_FINDMY_SEPARATED; fo.conf = 80; fo.a = 3; fo.b = 25;
  formatLogCsv(fo, line, sizeof(line));
  CHECK(strstr(line, "following,tracker,") && strstr(line, "places=3 minutes=25"));
  LogRec tr;
  tr.up_s = 120; tr.kind = LK_TRACK; tr.has_pos = true; tr.lat_e7 = -5; tr.lon_e7 = 1800000000;
  formatLogCsv(tr, line, sizeof(line));
  CHECK_EQ(std::string(line), std::string(",120,-0.0000005,180.0000000,track,,,,,,,,\n"));
  // Every kind formats; header has as many columns as a line
  auto commas = [](const char* s) { int c = 0; bool inq = false; for (; *s; ++s) { if (*s == '"') inq = !inq; else if (*s == ',' && !inq) ++c; } return c; };
  for (uint8_t k = LK_START; k <= LK_TRACK; ++k) {
    LogRec x = r;
    x.kind = k;
    CHECK(formatLogCsv(x, line, sizeof(line)) > 0);
    CHECK_EQ(commas(line), commas(kLogCsvHeader));
  }
  CHECK_EQ(formatLogCsv(r, line, 20), 0u);  // doesn't fit: nothing
}

namespace {
struct FakeFiles : FileSource {
  bool in = true;
  std::vector<std::pair<std::string, std::string>> files;
  bool ready() override { return in; }
  void forEach(void (*fn)(const char*, uint32_t, void*), void* ctx) override {
    for (auto& f : files) fn(f.first.c_str(), (uint32_t)f.second.size(), ctx);
  }
  int32_t readAt(const char* name, uint32_t off, uint8_t* buf, size_t len, uint32_t* size) override {
    for (auto& f : files) {
      if (f.first != name) continue;
      *size = (uint32_t)f.second.size();
      if (off >= f.second.size()) return 0;
      const size_t n = std::min(len, f.second.size() - off);
      memcpy(buf, f.second.data() + off, n);
      return (int32_t)n;
    }
    return -1;
  }
};
std::vector<FileChunk> g_fileOut;
void fileSink(const uint8_t* p, size_t n, void*) {
  CHECK(n <= kMaxPayload);
  FileChunk c;
  CHECK(decodeFileChunk(p, n, c));
  g_fileOut.push_back(c);
}
}  // namespace

static void testFileServe() {
  printf("remote SD files\n");
  FileReq q;
  q.req = 9; q.op = FO_READ; q.off = 123456; q.count = 8;
  strcpy(q.name, "2026-10-30.csv");
  uint8_t buf[64];
  size_t n = encodeFileReq(q, buf, sizeof(buf));
  FileReq b;
  CHECK(n > 0 && decodeFileReq(buf, n, b));
  CHECK(b.req == 9 && b.op == FO_READ && b.off == 123456 && b.count == 8 && strcmp(b.name, q.name) == 0);
  CHECK(!decodeFileReq(buf, n - 1, b));
  CHECK(fileNameOk("2026-10-30.csv") && fileNameOk("undated.csv"));
  CHECK(!fileNameOk("") && !fileNameOk("../x") && !fileNameOk("a/b") && !fileNameOk("a..csv") && !fileNameOk("a\\b"));
  CHECK(!fileNameOk("0123456789012345678901234567890123.csv"));

  FakeFiles fs;
  std::string big;
  for (int i = 0; i < 2345; ++i) big += (char)('a' + i % 26);
  fs.files.push_back({"2026-10-30.csv", big});
  fs.files.push_back({"undated.csv", "x"});
  for (int i = 0; i < 20; ++i) { char nm[32]; snprintf(nm, sizeof(nm), "2026-09-%02d.csv", i + 1); fs.files.push_back({nm, "yy"}); }

  // List, a page at a time
  std::vector<std::pair<std::string, uint32_t>> seen;
  uint32_t total = 0;
  for (int guard = 0; guard < 10; ++guard) {
    g_fileOut.clear();
    FileReq l; l.req = 3; l.op = FO_LIST; l.off = (uint32_t)seen.size();
    CHECK_EQ(serveFileReq(l, fs, fileSink, nullptr), 1);
    const FileChunk& c = g_fileOut[0];
    CHECK(c.kind == FK_LIST && c.req == 3 && c.off == seen.size());
    total = c.total;
    for (size_t i = 0; i < c.len;) {
      const uint8_t l2 = c.data[i];
      std::string nm((const char*)c.data + i + 1, l2);
      const uint8_t* s = c.data + i + 1 + l2;
      seen.push_back({nm, ((uint32_t)s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]});
      i += 1 + l2 + 4;
    }
    if (seen.size() >= total) break;
  }
  CHECK_EQ(total, 22u);
  CHECK_EQ(seen.size(), 22u);
  CHECK(seen[0].first == "2026-10-30.csv" && seen[0].second == 2345 && seen[1].first == "undated.csv" && seen[21].second == 2);

  // Read the whole file with windows, then check it matches
  std::string got;
  for (int guard = 0; guard < 20 && got.size() < big.size(); ++guard) {
    g_fileOut.clear();
    FileReq r = q; r.off = (uint32_t)got.size();
    serveFileReq(r, fs, fileSink, nullptr);
    for (auto& c : g_fileOut) {
      CHECK(c.kind == FK_DATA && c.total == big.size());
      if (c.off == got.size()) got.append((const char*)c.data, c.len);
    }
  }
  CHECK(got == big);
  CHECK_EQ(g_fileOut.size(), 4u);   // last window: 1600..2345 = 4 chunks, the last one short

  g_fileOut.clear();
  FileReq e = q; e.off = 5000;    // past the end: one empty chunk
  CHECK_EQ(serveFileReq(e, fs, fileSink, nullptr), 1);
  CHECK(g_fileOut[0].kind == FK_DATA && g_fileOut[0].len == 0 && g_fileOut[0].total == 2345);
  g_fileOut.clear();
  strcpy(e.name, "nope.csv"); e.off = 0;
  serveFileReq(e, fs, fileSink, nullptr);
  CHECK(g_fileOut.size() == 1 && g_fileOut[0].kind == FK_ERROR && g_fileOut[0].off == FE_NOT_FOUND);
  g_fileOut.clear();
  strcpy(e.name, "../etc");
  serveFileReq(e, fs, fileSink, nullptr);
  CHECK(g_fileOut[0].kind == FK_ERROR && g_fileOut[0].off == FE_BAD_NAME);
  g_fileOut.clear();
  fs.in = false;
  serveFileReq(e, fs, fileSink, nullptr);
  CHECK(g_fileOut[0].kind == FK_ERROR && g_fileOut[0].off == FE_NO_CARD);
  FileChunk full; full.len = kFileChunk;
  uint8_t fb[kMaxPayload];
  CHECK(encodeFileChunk(full, fb, sizeof(fb)) > 0);
}

static void testLossWindow() {
  puts("recent link loss windows");
  LossWindow w;
  uint16_t pm = 0; uint32_t span = 0;
  uint32_t f = 1091, l = 633;            // a bad start (the CYD after a flash)...
  w.sample(0, f, l);
  CHECK(!w.over(30000, 600000, f, l, pm, span));   // under a minute: nothing yet
  for (uint32_t t = 1000; t <= 900000; t += 1000) { f += 3; w.sample(t, f, l); }   // ...then 15 clean minutes
  CHECK(w.over(900000, 600000, f, l, pm, span));
  CHECK_EQ(pm, 0);
  CHECK(span >= 600000 && span < 660000);
  CHECK(w.over(900000, 3600000, f, l, pm, span));   // hour: covers all 15 min, includes nothing lost after start
  CHECK_EQ(span, 900000u);
  CHECK_EQ(pm, 0);
  l += 30; f += 270;                     // a bad 30 s: 30 of 300 lost
  CHECK(w.over(930000, 600000, f, l, pm, span));
  CHECK(pm > 10 && pm < 20);             // ~30 / (1800 + 300)
  // Hour window after 2 h: only the last hour counts
  LossWindow h; uint32_t hf = 0, hl = 0;
  for (uint32_t t = 0; t <= 7200000; t += 1000) {
    hf += 3; if (t < 3000000) hl += 1;   // lossy for the first 50 min, clean after
    h.sample(t, hf, hl);
  }
  CHECK(h.over(7200000, 3600000, hf, hl, pm, span));
  CHECK_EQ(pm, 0);
  CHECK(span >= 3600000 && span < 3900000);
  CHECK(h.mn == LossWindow::kMinute && h.hn == LossWindow::kFive);
  // Counts going backwards (node forgotten, joined again) start the windows over
  h.sample(7201000, 5, 0);
  CHECK_EQ(h.mn, 1);
  CHECK(!h.over(7230000, 600000, 50, 0, pm, span));
}

static void testDeviceIndex() {
  puts("device table index under a flood");
  static Device st[256];
  DeviceTable t(st, 256);
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  auto brute = [&](const uint8_t a[6], uint8_t radio) -> Device* {
    for (auto& d : st) if (d.used && d.radio == radio && memcmp(d.addr, a, 6) == 0) return &d;
    return nullptr;
  };
  uint8_t keep[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  bool ok = true;
  for (uint32_t i = 0; i < 6000; ++i) {
    Sighting x;
    x.kind = (i & 1) ? KIND_WIFI_PROBE_REQ : KIND_BLE_ADV;
    x.category = CAT_OTHER_WIFI;
    x.rssi_max = x.rssi_last = -60;
    x.hits = 1;
    for (int k = 0; k < 6; ++k) x.addr[k] = (uint8_t)rnd();
    x.addr[0] = (uint8_t)((x.addr[0] & 0xFC) | 0x02);   // random (spam-like) address
    if (i % 50 == 0) memcpy(x.addr, keep, 6);            // one real device seen now and then
    t.ingest(0, x, 1000 + i, nullptr);
    if (i % 97 == 0) {   // every used entry must be found through the index, and only it
      for (auto& d : st) if (d.used && t.find(d.addr, d.radio) != &d) ok = false;
      uint8_t miss[6] = {0xFE, 0xED, 0, 0, 0, (uint8_t)i};
      if (t.find(miss, 1) != brute(miss, 1)) ok = false;
    }
  }
  CHECK(ok);
  CHECK(t.used() >= 256u - 256u / 64 && t.used() <= 256u);   // full (a batch of freed slots may wait on hand)
  {
    size_t real = 0;
    for (auto& d : st) if (d.used) ++real;
    CHECK_EQ(t.used(), real);   // the kept count matches the table
  }
  CHECK(t.find(keep, radioClass(KIND_WIFI_PROBE_REQ)) != nullptr || t.find(keep, radioClass(KIND_BLE_ADV)) != nullptr);
}

static void testDeviceViewOrder() {
  puts("dashboard device view: one-pass ranking matches a full sort");
  static Device st[512];
  DeviceTable t(st, 512);
  uint32_t seed = 777;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  const uint32_t now = 100000;
  for (int i = 0; i < 400; ++i) {
    Sighting x;
    x.kind = KIND_BLE_ADV;
    x.category = (i % 3) ? CAT_OTHER_BLE : CAT_PHONE;
    x.conf = (uint8_t)(rnd() % 4 * 25);
    x.rssi_max = x.rssi_last = -70;
    x.hits = 1;
    for (int k = 0; k < 6; ++k) x.addr[k] = (uint8_t)rnd();
    t.ingest(0, x, now - (rnd() % 50000), nullptr);
  }
  const uint16_t cats = 0xFFFF;
  // Brute force: every listed device, sorted the way the view promises
  std::vector<const Device*> all;
  t.forEach([&](const Device& d) { if (!d.hidden && !d.alias_of && !d.spoofed && (int32_t)(now - d.last_ms) <= 60000) all.push_back(&d); });
  auto sc = [&](const Device* d) { const uint32_t age = now - d->last_ms; return ((uint32_t)d->conf << 16) | (age > 0xFFFF ? 0 : 0xFFFF - age); };
  std::sort(all.begin(), all.end(), [&](const Device* a, const Device* b) { return sc(a) != sc(b) ? sc(a) > sc(b) : a->id < b->id; });
  bool ok = true;
  for (uint16_t off : {0, 6, 120, 246}) {
    DashDevices v;
    buildDeviceView(t, now, cats, off, v);
    if (v.total != all.size()) ok = false;
    for (uint8_t r = 0; r < v.n; ++r)
      if (off + r < all.size() && v.rows[r].id != all[off + r]->id) ok = false;
  }
  CHECK(ok);
  CHECK(all.size() > 100);
}

static void testLinkChannel() {
  puts("link channel survey");
  auto ap = [](uint8_t ch, int8_t rssi, uint8_t util = 255) { ChanHeard h; h.channel = ch; h.rssi = rssi; h.ap = true; h.util = util; return h; };
  // Overlap hurts more than sharing: a -50 router on 7 costs channel 6 more than one on 6
  CHECK(linkChannelCost(6, ap(7, -50)) > linkChannelCost(6, ap(6, -50, 25)));
  CHECK_EQ(linkChannelCost(1, ap(7, -50)), 0.0f);    // 6 apart: nothing
  CHECK_EQ(linkChannelCost(11, ap(6, -50)), 0.0f);   // 5 apart: nothing
  CHECK(linkChannelCost(6, ap(6, -50, 200)) > linkChannelCost(6, ap(6, -50, 10)));   // a busy router costs more
  // Rob's farm: EAPs on 1 and 11, a Verizon hotspot on 7 at -50, EAPs weakly on 6
  LinkSurvey s;
  s.add(ap(1, -60, 20)); s.add(ap(1, -65, 20)); s.add(ap(11, -61, 20)); s.add(ap(11, -63, 20)); s.add(ap(11, -60, 20));
  s.add(ap(7, -50)); s.add(ap(6, -85)); s.add(ap(6, -87));
  CHECK(s.best() != 6);
  CHECK_EQ(linkChannelPick(6, s, 0), s.best());
  CHECK_EQ(linkChannelPick(s.best(), s, 0), 0);          // already there: stay
  // Hysteresis: a slightly better channel isn't worth a move ...
  LinkSurvey t;
  t.add(ap(6, -60, 30)); t.add(ap(1, -62, 30));
  CHECK_EQ(linkChannelPick(6, t, 0), 0);
  // ... nor is anything when nothing has been heard yet
  CHECK_EQ(linkChannelPick(6, LinkSurvey(), 900), 0);
  // a lossy link makes the current channel count 1.5x
  LinkSurvey u;
  u.add(ap(7, -65)); u.add(ap(1, -60, 10));
  const uint8_t calm = linkChannelPick(6, u, 0), lossy = linkChannelPick(6, u, 60);
  CHECK(calm == 0 && lossy != 0 && lossy == u.best());
  CHECK_EQ(linkChannelPick(3, s, 0), kLinkChannelDefault);   // off the list: back to 6
  {  // LinkChooser: averaged, needs a streak, remembers measured loss
    auto surv = [&](float c1, float c6, float c11) { LinkSurvey x; x.cost[0] = c1; x.cost[1] = c6; x.cost[2] = c11; x.heard = 30; return x; };
    LinkChooser lc;
    for (int i = 0; i < 60; ++i) lc.survey(surv(100, 110, 40));   // an hour of 11 looking clearest
    CHECK_EQ(lc.decide(6, 20), 0);    // 1 of 3
    CHECK_EQ(lc.streak(), 1);
    CHECK_EQ(lc.decide(6, 20), 0);    // 2 of 3
    CHECK_EQ(lc.decide(6, 20), 11);   // 3 of 3: move
    // One noisy minute doesn't swing an hour's average
    LinkChooser q;
    for (int i = 0; i < 60; ++i) q.survey(surv(100, 60, 80));
    q.survey(surv(100, 60, 5));
    CHECK(q.avgCost()[2] > 70);
    for (int i = 0; i < 3; ++i) CHECK_EQ(q.decide(6, 10), 0);   // 6 is still the pick: stay
    // A streak broken by a "stay" starts over
    LinkChooser r;
    for (int i = 0; i < 60; ++i) r.survey(surv(100, 110, 40));
    CHECK_EQ(r.decide(6, 20), 0);
    for (int i = 0; i < 120; ++i) r.survey(surv(100, 50, 60));   // 6 fine again for two hours
    CHECK_EQ(r.decide(6, 20), 0);
    CHECK_EQ(r.streak(), 0);
    // Remembered loss: the link did worse on 11 (4%) than it does on 6 now (1%): don't go back
    LinkChooser m;
    for (int i = 0; i < 60; ++i) m.survey(surv(100, 110, 40));
    m.noteLoss(11, 40);
    CHECK_EQ(m.measuredLoss(11), 40);
    CHECK_EQ(m.measuredLoss(6), -1);
    CHECK_EQ(m.decide(6, 10, true), 0);
    CHECK_EQ(m.vetoed(), 11);
    CHECK_EQ(m.decide(6, 50, true), 11);   // ...unless 6 is doing worse still
    // SURVEY NOW skips the streak
    LinkChooser f;
    for (int i = 0; i < 60; ++i) f.survey(surv(100, 110, 40));
    CHECK_EQ(f.decide(6, 0, true), 11);
    CHECK_EQ(LinkChooser().decide(6, 0, true), 0);   // nothing surveyed yet
  }
  // Searching for a lost master: the last known channel first, then the others
  CHECK(linkSearchChannel(1, 0) == 1 && linkSearchChannel(1, 1) == 6 && linkSearchChannel(1, 2) == 11 && linkSearchChannel(1, 3) == 1);
  CHECK(linkSearchChannel(6, 0) == 6 && linkSearchChannel(6, 1) == 1 && linkSearchChannel(6, 2) == 11);
  CHECK_EQ(linkSearchChannel(9, 0), 6);
}

extern const uint32_t ssBuildStamp = 0;

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--golden") == 0) { emitGolden(); return 0; }
  testCrcAndFrames();
  testSightings();
  testCobs();
  testChannels();
  testBle();
  testWifi();
  testBatch();
  testDevices();
  testNodes();
  testSpamAndAliases();
  testLora();
  testAesMesh();
  testWifiSpam();
  testEvilTwin();
  testSubGhz();
  testGps();
  testFileServe();
  testLossWindow();
  testLinkChannel();
  testDeviceIndex();
  testDeviceViewOrder();
  testDash();
  testTouch();
  testDashViews();
  testCrypto();
  testPairing();
  testFollow();
  testLog();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
