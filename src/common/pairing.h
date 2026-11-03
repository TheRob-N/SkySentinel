// Pairing over ESP-NOW: the radio side of ss_pair.h, shared by every board's firmware.
//
// Node: with no cluster key it's unpaired and does nothing but ask to join
// (MSG_PAIR_REQ every 1.5 s). A master's offer gives it a 6-digit code to show; the
// sealed key that follows ACCEPT is saved with the master's address, then it restarts
// into the cluster. Pairing again (BOOT held 5 s, "pair", or FORGET on the master)
// drops the key and starts over.
//
// Master: makes its own random cluster key the first time. ADD NODE opens a 2-minute
// window: nodes asking to join are listed, offered a key exchange once they're close
// (strong signal), and get the cluster key only when you press ACCEPT.
#pragma once

#include <Arduino.h>

#include "espnow_link.h"
#include "ss_pair.h"
#include "ss_proto.h"

namespace ss {
namespace pairing {

// BOOT button: GPIO 28 on the ESP32-C5, 0 on the S3 and the original ESP32.
int bootPin();
// Fill with random bytes from the hardware RNG.
void randomBytes(uint8_t* out, size_t n);

class NodePair {
 public:
  void begin(EspNowLink* link, const uint8_t my_mac[6], uint16_t my_id, uint8_t hw, uint8_t caps, uint16_t fw);
  void poll(uint32_t now);  // while unpaired: ask to join, on the home channel (every 1.5 s)
  void request();           // ask right now
  // A pairing frame (offer, key) for us? Handles it and returns true.
  bool handle(const RxPacket& p, const Header& h, const uint8_t* pl, size_t n, uint32_t now);
  // The key arrived: save it (and the master) and restart.
  bool done() const { return done_; }
  const char* key() const { return key_; }
  const uint8_t* master() const { return master_; }

  bool hasOffer(uint32_t now) const { return offer_ms_ && now - offer_ms_ < 10000; }
  uint32_t code() const { return secret_.code; }
  uint16_t offerFrom() const { return nodeIdFromMac(offer_mac_); }
  bool blinking(uint32_t now) const { return (int32_t)(blink_until_ - now) > 0; }
  void status(Print& out, uint32_t now) const;  // ,"pair":{...}

 private:
  EspNowLink* link_ = nullptr;
  PairReq req_;
  uint8_t priv_[32] = {0};
  uint8_t offer_mac_[6] = {0};
  uint8_t offer_pub_[32] = {0};
  PairSecret secret_;
  uint32_t offer_ms_ = 0, last_req_ = 0, blink_until_ = 0;
  bool done_ = false;
  char key_[kPairKeyMax + 1] = {0};
  uint8_t master_[6] = {0};
};

class MasterPair {
 public:
  static constexpr int8_t kCloseRssi = -55;      // "arm's reach"
  static constexpr uint32_t kWindowMs = 120000;  // 2 minutes
  static constexpr uint8_t kMax = 6;

  void begin(EspNowLink* link, uint16_t my_id) { link_ = link; my_id_ = my_id; }
  void start(uint32_t now);
  void stop() { until_ = 0; for (auto& c : c_) c.used = false; }
  bool active(uint32_t now) const { return until_ && (int32_t)(until_ - now) > 0; }
  // A pairing request? Handles it and returns true.
  bool handle(const RxPacket& p, const Header& h, const uint8_t* pl, size_t n, uint32_t now);
  void poll(uint32_t now, const char* cluster_key);
  bool accept(const uint8_t mac[6], uint32_t now);
  bool ignore(const uint8_t mac[6]);
  bool blink(const uint8_t mac[6], uint32_t now);
  void joined(const uint8_t mac[6]);  // its HELLO checked out: done with it
  void status(Print& out, uint32_t now) const;  // ,"pair":{...}

 private:
  struct Cand {
    bool used = false;
    uint8_t mac[6] = {0};
    PairReq req;
    int8_t rssi = -127;
    PairSecret secret;
    bool offered = false;     // key exchange done (it was close enough)
    bool accepted = false;
    bool blink = false;
    uint32_t last_ms = 0, accepted_ms = 0, last_send = 0;
    uint8_t key_sends = 0;
  };
  Cand* find(const uint8_t mac[6]);
  void sendOffer(Cand& c);
  void sendKey(Cand& c);
  const char* key_ = nullptr;  // the cluster key (set by poll)

  EspNowLink* link_ = nullptr;
  uint16_t my_id_ = 0;
  uint32_t until_ = 0;
  uint8_t priv_[32] = {0}, pub_[32] = {0};
  Cand c_[kMax];
};

}  // namespace pairing
}  // namespace ss
