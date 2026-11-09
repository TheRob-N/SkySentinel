// LoRa (915 MHz) packet parsing for the sub-GHz node: Meshtastic, MeshCore and LoRaWAN.
// Headers, plus Meshtastic packets on the public default channel (LongFast /
// MediumFast with the published default key, which every Meshtastic radio ships with):
// node names and the positions they broadcast. Private channels and LoRaWAN data stay
// unreadable. Pure C++, host tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_classify.h"
#include "ss_proto.h"

namespace ss {

// What the radio listens to. US frequencies (Meshtastic default channel slots for the
// preset's default channel name; LoRaWAN US915 sub-band 2, used by TTN and most others).
struct LoraPreset {
  const char* name;
  float mhz;
  float bw_khz;
  uint8_t sf;
  uint8_t cr;        // 5 = 4/5
  uint8_t sync;      // 0x2B Meshtastic, 0x34 public LoRaWAN
  uint8_t preamble;
};
extern const LoraPreset kMeshLongFast;    // 906.875 MHz SF11 250 kHz (default)
extern const LoraPreset kMeshMediumFast;  // 913.125 MHz SF9 250 kHz (many meshes moved here)
// LoRaWAN uplink channel n (0-7): 903.9 + 0.2 n MHz, 125 kHz, SF7-10
float loraWanUplinkMhz(uint8_t n);
// MeshCore's US/Canada recommended setting: 910.525 MHz, 62.5 kHz, SF7, 4/5, private sync word.
extern const LoraPreset kMeshCoreUs;

// Meshtastic US frequency for a channel: slot = djb2(name) % slots (902-928 MHz cut into
// bw-wide slots), unless the owner picked a slot (1-based, as the Meshtastic app shows it).
uint16_t meshUsSlots(float bw_khz);
uint16_t meshUsDefaultSlot(const char* channel_name, float bw_khz);  // 1-based
float meshUsMhz(float bw_khz, uint16_t slot);                         // slot 1-based

// What the LoRa radio listens to, chosen per node on the master (or "lora ..." on the
// board's serial port). AUTO takes turns: LongFast, MediumFast, MeshCore, LoRaWAN.
enum LoraMode : uint8_t {
  LM_AUTO = 0,
  LM_LONGFAST, LM_MEDIUMFAST, LM_MEDIUMSLOW, LM_LONGSLOW, LM_LONGMOD,
  LM_SHORTFAST, LM_SHORTSLOW, LM_SHORTTURBO,   // Meshtastic modem presets
  LM_MESHCORE,
  LM_LORAWAN,
  LM_COUNT
};
const char* loraModeName(uint8_t mode);           // "auto", "longfast", ..., "meshcore", "lorawan"
bool parseLoraMode(const char* s, uint8_t& mode);  // case-insensitive name
bool loraModeIsMeshtastic(uint8_t mode);
// The radio settings for a single-preset mode (not AUTO / LoRaWAN). slot 0 = the preset's
// default channel slot; 1..N picks another (Meshtastic only; ignored for MeshCore).
// The preset's name is Meshtastic's default channel name ("LongFast"), used to read the
// public channel.
bool loraModePreset(uint8_t mode, uint16_t slot, LoraPreset& out);

// Meshtastic over-the-air header (16 bytes, little-endian), from the protocol docs.
struct MeshHeader {
  uint32_t to = 0, from = 0, id = 0;
  uint8_t hop_limit = 0, hop_start = 0;
  bool want_ack = false, via_mqtt = false;
  uint8_t channel_hash = 0, next_hop = 0, relay_node = 0;
};
bool parseMeshtastic(const uint8_t* p, size_t n, MeshHeader& h);

// Meshtastic default channel: AES-128-CTR with the published default key ("AQ==").
// channelHash(name) is the 1-byte hash a packet carries for its channel.
uint8_t meshChannelHash(const char* channel_name);
// Decrypts a packet's payload (after the 16-byte header) if it's on the default channel
// called channel_name. Returns false for any other channel.
bool meshDecryptDefault(const MeshHeader& h, const char* channel_name, const uint8_t* enc, size_t n, uint8_t* out);

// What a decrypted Meshtastic Data message says (protobuf, only the bits we show).
enum MeshPort : uint8_t { MP_TEXT = 1, MP_POSITION = 3, MP_NODEINFO = 4, MP_TELEMETRY = 67 };
struct MeshInfo {
  uint32_t port = 0;
  char long_name[kMaxName + 1] = {0};   // NodeInfo
  char short_name[8] = {0};
  uint32_t hw_model = 0;
  bool has_pos = false;                 // Position
  int32_t lat_e7 = 0, lon_e7 = 0, alt_m = 0;
  bool has_batt = false;                // Telemetry: device metrics
  uint8_t batt_pct = 0;
};
bool decodeMeshData(const uint8_t* p, size_t n, MeshInfo& out);

// LoRaWAN uplink: data frames carry the session DevAddr in the clear; join requests
// carry the device's permanent DevEUI (and JoinEUI) in the clear.
struct LoraWanFrame {
  uint8_t mtype = 0;            // 0 join request, 2 unconfirmed up, 4 confirmed up
  uint32_t dev_addr = 0;        // data frames
  uint16_t fcnt = 0;
  uint8_t dev_eui[8] = {0};     // join requests, MSB first as usually printed
  uint8_t join_eui[8] = {0};
};
bool parseLoraWan(const uint8_t* p, size_t n, LoraWanFrame& f);

// MeshCore (v1) packets. Only adverts say who sent them (the node's public key, its
// name, its kind and often its position, all in the clear); everything else carries
// 1-byte hashes, so it is counted but not turned into a device.
enum MeshCoreRoute : uint8_t { MCR_TRANSPORT_FLOOD = 0, MCR_FLOOD = 1, MCR_DIRECT = 2, MCR_TRANSPORT_DIRECT = 3 };
enum MeshCoreType : uint8_t {
  MCT_REQ = 0, MCT_RESPONSE, MCT_TXT_MSG, MCT_ACK, MCT_ADVERT, MCT_GRP_TXT, MCT_GRP_DATA,
  MCT_ANON_REQ, MCT_PATH, MCT_TRACE, MCT_MULTIPART, MCT_CONTROL, MCT_RAW_CUSTOM = 0x0F
};
struct MeshCorePacket {
  uint8_t route = 0, type = 0;
  uint8_t hash_size = 1, hops = 0;  // path: hops entries of hash_size bytes (repeaters it went through)
  size_t payload_off = 0, payload_len = 0;
};
bool parseMeshCore(const uint8_t* p, size_t n, MeshCorePacket& m);
const char* meshCoreTypeName(uint8_t type);  // "advert", "group text", ...

enum MeshCoreNode : uint8_t { MCN_CHAT = 1, MCN_REPEATER = 2, MCN_ROOM = 3, MCN_SENSOR = 4 };
struct MeshCoreAdvert {
  uint8_t pubkey[32] = {0};
  uint32_t timestamp = 0;
  uint8_t node_type = 0;
  bool has_pos = false;
  int32_t lat_e6 = 0, lon_e6 = 0;
  char name[kMaxName + 1] = {0};
};
// The advert payload (pubkey, time, signature, then flags/position/name). The signature
// isn't checked.
bool parseMeshCoreAdvert(const uint8_t* p, size_t n, MeshCoreAdvert& a);

// Sightings. addr = node id / DevAddr in the last 4 bytes (MSB first), or the last
// 6 bytes of the DevEUI for a join. channel = hops away for Meshtastic (0 = heard
// directly), 0xFF = arrived over MQTT (not actually nearby).
Sighting makeMeshSighting(const MeshHeader& h, int8_t rssi);
Sighting makeLoraWanSighting(const LoraWanFrame& f, int8_t rssi);
// MeshCore: addr = the first 6 bytes of its public key (MeshCore shows the first bytes
// as the node's id too); channel = hops (the flood path's length, 0 = heard directly).
Sighting makeMeshCoreSighting(const MeshCorePacket& m, const MeshCoreAdvert& a, int8_t rssi);
constexpr uint8_t kLoraViaMqtt = 0xFF;

}  // namespace ss
