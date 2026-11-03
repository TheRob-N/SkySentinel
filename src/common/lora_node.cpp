#ifdef SS_LORA
#include "lora_node.h"

#include <RadioLib.h>
#include <SPI.h>

#include "ss_lora.h"

// Heltec Wireless Tracker (V1.x) wiring, same as Meshtastic's variant for this board.
#ifndef SS_LORA_NSS
#define SS_LORA_NSS 8
#define SS_LORA_SCK 9
#define SS_LORA_MOSI 10
#define SS_LORA_MISO 11
#define SS_LORA_RST 12
#define SS_LORA_BUSY 13
#define SS_LORA_DIO1 14
#define SS_LORA_TCXO 1.8f
#define SS_VEXT_CTRL 3  // powers the GNSS and display; HIGH = on
#endif

namespace ss {
namespace lora {

static SX1262 radio = new Module(SS_LORA_NSS, SS_LORA_DIO1, SS_LORA_RST, SS_LORA_BUSY);
static Sink sink_ = nullptr;
static volatile uint16_t* cats_ = nullptr;
static bool ready_ = false;
static Stats stats_;
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;

static void count(uint32_t Stats::*field) {
  portENTER_CRITICAL(&statsMux);
  ++(stats_.*field);
  portEXIT_CRITICAL(&statsMux);
}

static bool apply(const LoraPreset& p) {
  return radio.setFrequency(p.mhz) == RADIOLIB_ERR_NONE && radio.setBandwidth(p.bw_khz) == RADIOLIB_ERR_NONE &&
         radio.setSpreadingFactor(p.sf) == RADIOLIB_ERR_NONE && radio.setCodingRate(p.cr) == RADIOLIB_ERR_NONE &&
         radio.setSyncWord(p.sync) == RADIOLIB_ERR_NONE && radio.setPreambleLength(p.preamble) == RADIOLIB_ERR_NONE;
}

// Reads a received packet and hands it on. Meshtastic first when listening there,
// LoRaWAN when sweeping; anything else is only counted.
static void handlePacket(bool meshMode) {
  uint8_t buf[256];
  const size_t len = radio.getPacketLength();
  const int state = radio.readData(buf, len > sizeof(buf) ? sizeof(buf) : len);
  if (state == RADIOLIB_ERR_CRC_MISMATCH) { count(&Stats::crc_errors); return; }
  if (state != RADIOLIB_ERR_NONE) return;
  count(&Stats::packets);
  const int8_t rssi = (int8_t)radio.getRSSI();
  if (meshMode) {
    MeshHeader h;
    if (parseMeshtastic(buf, len, h)) {
      count(&Stats::meshtastic);
      const Sighting s = makeMeshSighting(h, rssi);
      // Public default channel: read the node's name and the position it broadcasts.
      uint8_t plain[256];
      MeshInfo mi;
      const size_t n = len > 16 ? (len > sizeof(buf) ? sizeof(buf) : len) - 16 : 0;
      const bool readable = meshDecryptDefault(h, stats_.listening, buf + 16, n, plain) && decodeMeshData(plain, n, mi);
      if (readable) {
        count(&Stats::mesh_read);
        if (mi.port == MP_NODEINFO) {
          char name[kMaxName + 1];
          const int l = mi.short_name[0] ? snprintf(name, sizeof(name), "%s (%s)", mi.long_name, mi.short_name)
                                         : snprintf(name, sizeof(name), "%s", mi.long_name);
          sink_(s, name, (uint8_t)(l < (int)kMaxName ? l : (int)kMaxName), NAME_DEVICE);
          return;
        }
        if (mi.port == MP_TELEMETRY && mi.has_batt) {
          char b[8];
          const int l = snprintf(b, sizeof(b), "%u", mi.batt_pct);
          sink_(s, b, (uint8_t)l, NAME_BATT);
          return;
        }
        if (mi.port == MP_POSITION && mi.has_pos) {
          char pos[kMaxName + 1];
          const long la = labs((long)mi.lat_e7), lo = labs((long)mi.lon_e7);
          const int l = snprintf(pos, sizeof(pos), "%s%ld.%07ld,%s%ld.%07ld,%ld", mi.lat_e7 < 0 ? "-" : "", la / 10000000,
                                 la % 10000000, mi.lon_e7 < 0 ? "-" : "", lo / 10000000, lo % 10000000, (long)mi.alt_m);
          sink_(s, pos, (uint8_t)(l < (int)kMaxName ? l : (int)kMaxName), NAME_POS);
          return;
        }
      }
      // Anything else: what kind of packet it was, so the details can say why there's no
      // name yet ("telemetry on LongFast", "a private channel we can't read").
      char note[24];
      const int l = readable ? snprintf(note, sizeof(note), "%s:%lu", stats_.listening, (unsigned long)mi.port)
                             : snprintf(note, sizeof(note), "private:%02X", h.channel_hash);
      sink_(s, note, (uint8_t)l, NAME_MESH);
      return;
    }
  } else {
    LoraWanFrame f;
    if (parseLoraWan(buf, len, f)) {
      count(&Stats::lorawan);
      char name[33];
      int n;
      if (f.mtype == 0) {
        n = snprintf(name, sizeof(name), "DevEUI %02X%02X%02X%02X%02X%02X%02X%02X", f.dev_eui[0], f.dev_eui[1],
                     f.dev_eui[2], f.dev_eui[3], f.dev_eui[4], f.dev_eui[5], f.dev_eui[6], f.dev_eui[7]);
      } else {
        n = snprintf(name, sizeof(name), "DevAddr %08lX", (unsigned long)f.dev_addr);
      }
      sink_(makeLoraWanSighting(f, rssi), name, (uint8_t)n, NAME_DEVICE);
      return;
    }
  }
  count(&Stats::other);
}

// Listens on one Meshtastic preset for `ms`, taking packets as they land.
static void listenMesh(const LoraPreset& p, uint32_t ms) {
  if (!apply(p)) return;
  stats_.listening = p.name;
  radio.startReceive();
  const uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    if (digitalRead(SS_LORA_DIO1)) {
      handlePacket(true);
      radio.startReceive();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  radio.standby();
}

// LoRaWAN: 8 uplink channels x SF7-10. Channel-activity detection on each pair is a
// few ms; when something is there, receive it.
static void sweepLoraWan(uint32_t ms) {
  stats_.listening = "LoRaWAN";
  LoraPreset p = {"LoRaWAN", 0, 125.0f, 7, 5, 0x34, 8};
  const uint32_t until = millis() + ms;
  static uint8_t ch = 0, sf = 7;
  while ((int32_t)(millis() - until) < 0) {
    p.mhz = loraWanUplinkMhz(ch);
    p.sf = sf;
    if (apply(p) && radio.scanChannel() == RADIOLIB_LORA_DETECTED) {
      count(&Stats::cad_hits);
      radio.startReceive();
      const uint32_t wait = millis() + 600;  // longest SF10 uplink in this band ~ 400 ms
      while ((int32_t)(millis() - wait) < 0 && !digitalRead(SS_LORA_DIO1)) vTaskDelay(pdMS_TO_TICKS(2));
      if (digitalRead(SS_LORA_DIO1)) handlePacket(false);
      radio.standby();
    }
    if (++sf > 10) { sf = 7; ch = (uint8_t)((ch + 1) & 7); }
    vTaskDelay(1);
  }
}

static void loraTask(void*) {
  for (;;) {
    if (!cats_ || !(*cats_ & catBit(CAT_LORA))) {
      stats_.listening = "off (LoRa category disabled)";
      radio.standby();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    listenMesh(kMeshLongFast, 20000);
    listenMesh(kMeshMediumFast, 10000);
    sweepLoraWan(10000);
  }
}

bool begin(Sink sink, volatile uint16_t* category_mask) {
  sink_ = sink;
  cats_ = category_mask;
#ifdef SS_VEXT_CTRL
  pinMode(SS_VEXT_CTRL, OUTPUT);
  digitalWrite(SS_VEXT_CTRL, HIGH);
#endif
  pinMode(SS_LORA_DIO1, INPUT);
  SPI.begin(SS_LORA_SCK, SS_LORA_MISO, SS_LORA_MOSI, SS_LORA_NSS);
  const LoraPreset& p = kMeshLongFast;
  // Transmit power is irrelevant (we never transmit); TCXO on DIO3, RF switch on DIO2.
  if (radio.begin(p.mhz, p.bw_khz, p.sf, p.cr, p.sync, 2, p.preamble, SS_LORA_TCXO, false) != RADIOLIB_ERR_NONE)
    return false;
  radio.setDio2AsRfSwitch(true);
  radio.setRxBoostedGainMode(true);
  radio.setCRC(true);
  ready_ = true;
  xTaskCreate(loraTask, "lora", 6144, nullptr, 1, nullptr);
  return true;
}

bool ready() { return ready_; }

Stats stats() {
  portENTER_CRITICAL(&statsMux);
  Stats s = stats_;
  portEXIT_CRITICAL(&statsMux);
  return s;
}

}  // namespace lora
}  // namespace ss
#endif  // SS_LORA
