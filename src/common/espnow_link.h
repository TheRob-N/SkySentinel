// ESP-NOW transport shared by nodes and the Core.
//
// The receive callback runs in the Wi-Fi driver task, so it only copies the
// frame into a FreeRTOS queue; everything else happens in loop().
//
// Security: with a cluster key set, peers are added with an ESP-NOW LMK
// (CCMP-encrypted unicast) and HELLO / WELCOME carry an HMAC tag so neither
// side pairs with a stranger. Key derivation matches the JCMK wardriver
// (SHA-256(key + "_pmk"/"_lmk"), first 16 bytes).
#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "ss_proto.h"

namespace ss {

struct RxPacket {
  uint8_t src[6];
  int8_t rssi;
  uint8_t len;
  uint8_t data[kMaxFrame];
};

class EspNowLink {
 public:
  // Call after WiFi.mode(WIFI_STA). Fixes the radio on `home_channel`.
  bool begin(const char* cluster_key, uint8_t home_channel, uint8_t queue_depth = 24);

  bool hasKey() const { return has_key_; }
  // HMAC-SHA256(key, data) truncated to 4 bytes (little-endian). 0 when no key.
  uint32_t authTag(const uint8_t* data, size_t len) const;

  bool addPeer(const uint8_t mac[6]);  // encrypted when a key is set
  bool removePeer(const uint8_t mac[6]);
  // Frames stuck in the radio (handed over, never reported sent or failed) fill ESP-NOW's
  // small queue and every send after that is refused. Restarting ESP-NOW (keeping the
  // peers, keys and Long Range settings) empties it in a few ms. Returns frames cleared.
  uint32_t resetTx();

  // Builds a frame (node_id, next seq) and sends it. dst = nullptr means broadcast.
  bool send(const uint8_t* dst, uint8_t type, uint16_t node_id, const uint8_t* payload, size_t len);

  bool poll(RxPacket& out) { return queue_ && xQueueReceive(queue_, &out, 0) == pdTRUE; }

  // Espressif Long Range (LR): 250 kbit/s frames that carry several times further.
  // Every board listens for LR (lrCapable() says whether its radio accepted that);
  // setPeerLongRange() picks what we *send* to one peer. Remembered, so it survives
  // the peer being removed and added again.
  bool lrCapable() const { return lr_rx_; }
  // rate: -1 = the link-wide rate (setLrFast), 0 = 250 kbit/s, 1 = 500 kbit/s for this peer.
  bool setPeerLongRange(const uint8_t mac[6], bool on, int8_t rate = -1);
  bool peerLongRange(const uint8_t mac[6]) const;
  // Rate for broadcasts (a node calling for its master alternates).
  void setBroadcastLongRange(bool on);
  // Long Range rate: 250 kbit/s (farthest) or 500 kbit/s (half the airtime per frame).
  // Re-applies to every Long Range peer. Receivers hear both.
  void setLrFast(bool fast);
  bool lrFast() const { return lr_fast_; }

  uint32_t sendFailures() const { return send_fail_; }
  // Frames handed to the radio and not yet sent (or given up on). A node that hops
  // channels waits for this to reach 0 first, so nothing goes out on the wrong channel.
  uint32_t inFlight() const { return sent_ - done_; }
  uint32_t notDelivered() const { return not_acked_; }
  uint32_t rxDropped() const { return rx_dropped_; }
  uint32_t rxCount() const { return rx_count_; }   // our frames received (any sender)
  int lastSendError() const { return last_err_; }   // esp_err_t of the last failed send (0 = none yet)
  uint32_t failStreak() const { return fail_streak_; }  // failed sends in a row (0 after any success)
  uint8_t homeChannel() const { return home_channel_; }
  uint32_t txResets() const { return resets_; }

  static const uint8_t kBroadcast[6];

 private:
  static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
  static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status);
  uint32_t sent_ = 0, fail_streak_ = 0;
  int last_err_ = 0;
  volatile uint32_t done_ = 0, not_acked_ = 0, rx_count_ = 0;
  bool ensureBroadcastPeer();
  void enableLrRx();
  bool applyRate(const uint8_t mac[6], bool lr);
  bool peerFast(const uint8_t mac[6]) const;
  bool lr_rx_ = false;
  bool lr_fast_ = false;
  uint8_t lr_peers_[8][6] = {};
  int8_t lr_rate_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};  // per peer: -1 link-wide, 0 250, 1 500
  uint8_t n_lr_ = 0;

  static EspNowLink* self_;
  QueueHandle_t queue_ = nullptr;
  uint8_t key_[32] = {0};
  size_t key_len_ = 0;
  bool has_key_ = false;
  uint8_t lmk_[16] = {0};
  uint8_t pmk_[16] = {0};
  uint16_t seq_ = 0;
  uint8_t home_channel_ = 6;
  volatile uint32_t rx_dropped_ = 0;
  uint32_t send_fail_ = 0;
  uint32_t resets_ = 0;
};

// Fixes the Wi-Fi radio on a channel (2.4 or 5 GHz).
bool setRadioChannel(uint8_t channel);

}  // namespace ss
