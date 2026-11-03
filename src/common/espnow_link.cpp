#include "espnow_link.h"

#include <esp_wifi.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <string.h>

namespace ss {

const uint8_t EspNowLink::kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
EspNowLink* EspNowLink::self_ = nullptr;

static void deriveKey16(const char* key, const char* suffix, uint8_t out[16]) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)key, strlen(key));
  mbedtls_sha256_update(&ctx, (const unsigned char*)suffix, strlen(suffix));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  memcpy(out, hash, 16);
}

bool setRadioChannel(uint8_t channel) {
  return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

bool EspNowLink::begin(const char* cluster_key, uint8_t home_channel, uint8_t queue_depth) {
  self_ = this;
  home_channel_ = home_channel;
  queue_ = xQueueCreate(queue_depth, sizeof(RxPacket));
  if (!queue_) return false;

  esp_wifi_set_ps(WIFI_PS_NONE);
  setRadioChannel(home_channel_);
  if (esp_now_init() != ESP_OK) return false;

  has_key_ = cluster_key && cluster_key[0];
  if (has_key_) {
    key_len_ = strnlen(cluster_key, sizeof(key_));
    memcpy(key_, cluster_key, key_len_);
    deriveKey16(cluster_key, "_pmk", pmk_);
    deriveKey16(cluster_key, "_lmk", lmk_);
    esp_now_set_pmk(pmk_);
  }
  esp_now_register_recv_cb(&EspNowLink::onRecv);
  esp_now_register_send_cb(&EspNowLink::onSent);
  enableLrRx();
  return ensureBroadcastPeer();
}

// Listen for Long Range frames too (normal ones keep working).
void EspNowLink::enableLrRx() {
#if SOC_WIFI_SUPPORT_5G
  wifi_protocols_t pr = {};
  if (esp_wifi_get_protocols(WIFI_IF_STA, &pr) == ESP_OK) {
    pr.ghz_2g |= WIFI_PROTOCOL_LR;
    lr_rx_ = esp_wifi_set_protocols(WIFI_IF_STA, &pr) == ESP_OK;
  }
#else
  uint8_t bm = 0;
  if (esp_wifi_get_protocol(WIFI_IF_STA, &bm) == ESP_OK)
    lr_rx_ = esp_wifi_set_protocol(WIFI_IF_STA, bm | WIFI_PROTOCOL_LR) == ESP_OK;
#endif
}

bool EspNowLink::applyRate(const uint8_t mac[6], bool lr) {
  esp_now_rate_config_t c = {};
  if (lr) {
    c.phymode = WIFI_PHY_MODE_LR;
    c.rate = lr_fast_ ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_LORA_250K;
  } else {
    c.phymode = WIFI_PHY_MODE_11B;
    c.rate = WIFI_PHY_RATE_1M_L;   // ESP-NOW's usual rate: the most robust non-LR one
  }
  return esp_now_set_peer_rate_config(mac, &c) == ESP_OK;
}

bool EspNowLink::peerLongRange(const uint8_t mac[6]) const {
  for (uint8_t i = 0; i < n_lr_; ++i)
    if (memcmp(lr_peers_[i], mac, 6) == 0) return true;
  return false;
}

bool EspNowLink::setPeerLongRange(const uint8_t mac[6], bool on) {
  if (on && !lr_rx_) return false;
  for (uint8_t i = 0; i < n_lr_; ++i)
    if (memcmp(lr_peers_[i], mac, 6) == 0) {
      if (!on) { memmove(lr_peers_[i], lr_peers_[i + 1], (size_t)(n_lr_ - i - 1) * 6); --n_lr_; }
      return esp_now_is_peer_exist(mac) ? applyRate(mac, on) : true;
    }
  if (on) {
    if (n_lr_ >= 8) return false;
    memcpy(lr_peers_[n_lr_++], mac, 6);
  }
  return esp_now_is_peer_exist(mac) ? applyRate(mac, on) : true;
}

void EspNowLink::setLrFast(bool fast) {
  if (fast == lr_fast_) return;
  lr_fast_ = fast;
  for (uint8_t i = 0; i < n_lr_; ++i)
    if (esp_now_is_peer_exist(lr_peers_[i])) applyRate(lr_peers_[i], true);
}

void EspNowLink::setBroadcastLongRange(bool on) { applyRate(kBroadcast, on && lr_rx_); }

uint32_t EspNowLink::authTag(const uint8_t* data, size_t len) const {
  if (!has_key_) return 0;
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md_hmac(info, key_, key_len_, data, len, mac) != 0) return 0;
  return (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
}

bool EspNowLink::ensureBroadcastPeer() {
  if (esp_now_is_peer_exist(kBroadcast)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, kBroadcast, 6);
  p.channel = 0;  // current channel
  p.encrypt = false;
  return esp_now_add_peer(&p) == ESP_OK;
}

bool EspNowLink::addPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.encrypt = has_key_;
  if (has_key_) memcpy(p.lmk, lmk_, 16);
  if (esp_now_add_peer(&p) != ESP_OK) return false;
  if (peerLongRange(mac)) applyRate(mac, true);
  return true;
}

uint32_t EspNowLink::resetTx() {
  const uint32_t stuck = sent_ - done_;
  uint8_t peers[12][6];
  int n = 0;
  esp_now_peer_info_t p = {};
  bool head = true;
  while (n < 12 && esp_now_fetch_peer(head, &p) == ESP_OK) {
    head = false;
    if (memcmp(p.peer_addr, kBroadcast, 6) != 0) memcpy(peers[n++], p.peer_addr, 6);
  }
  esp_now_deinit();
  if (esp_now_init() != ESP_OK) return 0;
  if (has_key_) esp_now_set_pmk(pmk_);
  esp_now_register_recv_cb(&EspNowLink::onRecv);
  esp_now_register_send_cb(&EspNowLink::onSent);
  ensureBroadcastPeer();
  for (int i = 0; i < n; ++i) addPeer(peers[i]);   // re-applies each peer's Long Range rate
  done_ = sent_;
  fail_streak_ = 0;
  ++resets_;
  return stuck;
}

bool EspNowLink::removePeer(const uint8_t mac[6]) {
  return esp_now_is_peer_exist(mac) ? esp_now_del_peer(mac) == ESP_OK : true;
}

bool EspNowLink::send(const uint8_t* dst, uint8_t type, uint16_t node_id, const uint8_t* payload, size_t len) {
  uint8_t frame[kMaxFrame];
  const size_t n = encodeFrame(type, node_id, seq_++, payload, len, frame, sizeof(frame));
  if (!n) return false;
  const uint8_t* to = dst ? dst : kBroadcast;
  // ESP-NOW holds only a few frames in flight: when they're all taken, give the radio
  // a moment instead of dropping (a lost heartbeat can cost a node its master).
  esp_err_t r = esp_now_send(to, frame, n);
  for (int i = 0; i < 5 && r == ESP_ERR_ESPNOW_NO_MEM; ++i) {
    delay(2);
    r = esp_now_send(to, frame, n);
  }
  const bool ok = r == ESP_OK;
  if (ok) { ++sent_; fail_streak_ = 0; }
  else { ++send_fail_; ++fail_streak_; last_err_ = r; }
  return ok;
}

void EspNowLink::onSent(const esp_now_send_info_t*, esp_now_send_status_t status) {
  if (!self_) return;
  self_->done_ = self_->done_ + 1;
  if (status != ESP_NOW_SEND_SUCCESS) self_->not_acked_ = self_->not_acked_ + 1;
}

void EspNowLink::onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!self_ || !info || !data || len <= 0 || len > (int)kMaxFrame) return;
  if (len < 2 || data[0] != (kMagic & 0xFF) || data[1] != (kMagic >> 8)) return;  // not ours
  self_->rx_count_ = self_->rx_count_ + 1;
  RxPacket p;
  memcpy(p.src, info->src_addr, 6);
  p.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : -127;
  p.len = (uint8_t)len;
  memcpy(p.data, data, len);
  if (xQueueSend(self_->queue_, &p, 0) != pdTRUE) self_->rx_dropped_ = self_->rx_dropped_ + 1;
}

}  // namespace ss
