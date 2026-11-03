#include "pairing.h"

#include <esp_random.h>

#include "core_common.h"
#include "ss_crypto.h"

namespace ss {
namespace pairing {

int bootPin() {
#if CONFIG_IDF_TARGET_ESP32C5
  return 28;
#else
  return 0;
#endif
}

void randomBytes(uint8_t* out, size_t n) { esp_fill_random(out, n); }

static void newKeypair(uint8_t priv[32], uint8_t pub[32]) {
  randomBytes(priv, 32);
  x25519Base(pub, priv);
}

// ---------------------------------------------------------------------------
// Node

void NodePair::begin(EspNowLink* link, const uint8_t my_mac[6], uint16_t my_id, uint8_t hw, uint8_t caps, uint16_t fw) {
  (void)my_id;
  link_ = link;
  memcpy(req_.mac, my_mac, 6);
  req_.hw = hw;
  req_.caps = caps;
  req_.fw = fw;
  newKeypair(priv_, req_.pub);
}

void NodePair::poll(uint32_t now) {
  if (done_ || !link_ || now - last_req_ < 1500) return;
  last_req_ = now;
  request();
}

void NodePair::request() {
  if (done_ || !link_) return;
  uint8_t buf[64];
  link_->send(nullptr, MSG_PAIR_REQ, nodeIdFromMac(req_.mac), buf, encodePairReq(req_, buf, sizeof(buf)));
}

bool NodePair::handle(const RxPacket& p, const Header& h, const uint8_t* pl, size_t n, uint32_t now) {
  if (h.type == MSG_PAIR_OFFER) {
    PairOffer o;
    if (!decodePairOffer(pl, n, o) || memcmp(o.target, req_.mac, 6) != 0) return true;
    // A new master, or a new key from the same one (it reopened pairing): recompute.
    if (memcmp(offer_mac_, p.src, 6) != 0 || memcmp(offer_pub_, o.pub, 32) != 0) {
      memcpy(offer_mac_, p.src, 6);
      memcpy(offer_pub_, o.pub, 32);
      pairDeriveFrom(priv_, o.pub, req_.pub, o.pub, secret_);
      char code[8];
      pairCodeText(secret_.code, code);
      Out.printf("[pair] master %04X offers to pair: code %s (check it matches the master's page)\n",
                 nodeIdFromMac(p.src), code);
    }
    offer_ms_ = now ? now : 1;
    if (o.flags & PO_BLINK) blink_until_ = now + 6000;
    return true;
  }
  if (h.type == MSG_PAIR_KEY) {
    PairKey k;
    if (!decodePairKey(pl, n, k)) return true;
    if (!offer_ms_ || memcmp(p.src, offer_mac_, 6) != 0) return true;  // only from the master we're talking to
    char key[kPairKeyMax + 1];
    if (!pairOpen(secret_, req_.mac, k, key)) {
      Out.println("[pair] key didn't check out, ignored");
      return true;
    }
    memcpy(key_, key, sizeof(key_));
    memcpy(master_, p.src, 6);
    done_ = true;
    Out.printf("[pair] paired with master %04X\n", nodeIdFromMac(p.src));
    return true;
  }
  return h.type == MSG_PAIR_REQ;  // another node asking: not for us
}

void NodePair::status(Print& out, uint32_t now) const {
  if (hasOffer(now)) {
    char code[8];
    pairCodeText(secret_.code, code);
    out.printf(",\"pair\":{\"state\":\"code\",\"code\":\"%s\",\"master\":\"%04X\"}", code, offerFrom());
  } else {
    out.print(",\"pair\":{\"state\":\"waiting\"}");
  }
}

// ---------------------------------------------------------------------------
// Master

void MasterPair::start(uint32_t now) {
  until_ = now + kWindowMs;
  if (!until_) until_ = 1;
  for (auto& c : c_) c.used = false;
  newKeypair(priv_, pub_);  // fresh for every window
  Out.println("[pair] pairing open for 2 minutes");
}

MasterPair::Cand* MasterPair::find(const uint8_t mac[6]) {
  for (auto& c : c_)
    if (c.used && memcmp(c.mac, mac, 6) == 0) return &c;
  return nullptr;
}

void MasterPair::sendOffer(Cand& c) {
  PairOffer o;
  memcpy(o.target, c.mac, 6);
  memcpy(o.pub, pub_, 32);
  o.flags = c.blink ? PO_BLINK : 0;
  uint8_t buf[64];
  link_->send(nullptr, MSG_PAIR_OFFER, my_id_, buf, encodePairOffer(o, buf, sizeof(buf)));
}

bool MasterPair::handle(const RxPacket& p, const Header& h, const uint8_t* pl, size_t n, uint32_t now) {
  if (h.type != MSG_PAIR_REQ) return h.type == MSG_PAIR_OFFER || h.type == MSG_PAIR_KEY;
  if (!active(now)) return true;
  PairReq r;
  if (!decodePairReq(pl, n, r) || memcmp(r.mac, p.src, 6) != 0) return true;
  Cand* c = find(r.mac);
  if (!c) {
    for (auto& x : c_) if (!x.used) { c = &x; break; }
    if (!c) return true;  // list full
    *c = Cand();
    c->used = true;
    memcpy(c->mac, r.mac, 6);
  }
  if (memcmp(c->req.pub, r.pub, 32) != 0) {  // the node restarted: new key, start over with it
    c->req = r;
    c->offered = false;
    c->accepted = false;
  }
  c->rssi = c->rssi == -127 ? p.rssi : (int8_t)((c->rssi * 3 + p.rssi) / 4);
  c->last_ms = now;
  // Close enough: exchange keys (the code appears on both sides)
  if (!c->offered && c->rssi >= kCloseRssi) {
    pairDeriveFrom(priv_, c->req.pub, c->req.pub, pub_, c->secret);
    c->offered = true;
  }
  if (c->offered && !c->accepted) sendOffer(*c);
  // Accepted but it's still asking: it hasn't got the key. It's listening right now
  // (it just sent this), so answer with the offer and the key straight away.
  if (c->accepted) {
    sendOffer(*c);
    sendKey(*c);
  }
  return true;
}

void MasterPair::sendKey(Cand& c) {
  PairKey k;
  if (!key_ || !pairSeal(c.secret, c.mac, key_, k)) return;
  uint8_t buf[80];
  link_->send(nullptr, MSG_PAIR_KEY, my_id_, buf, encodePairKey(k, buf, sizeof(buf)));
}

void MasterPair::poll(uint32_t now, const char* cluster_key) {
  key_ = cluster_key;
  for (auto& c : c_) {
    if (!c.used) continue;
    if (now - c.last_ms > 15000 && !c.accepted) { c.used = false; continue; }  // it went away
    // Also keep sending on our own for up to 30 s (it answers its next request anyway)
    if (c.accepted && now - c.last_send >= 500 && c.key_sends < 60) {
      sendKey(c);
      c.last_send = now;
      ++c.key_sends;
    }
    if (c.accepted && now - c.accepted_ms > 30000) c.used = false;  // never joined: let it ask again
  }
  if (until_ && !active(now)) {
    bool busy = false;
    for (auto& c : c_) busy |= c.used && c.accepted;
    if (!busy) { until_ = 0; for (auto& c : c_) c.used = false; Out.println("[pair] pairing closed"); }
  }
}

bool MasterPair::accept(const uint8_t mac[6], uint32_t now) {
  Cand* c = find(mac);
  if (!c || !c->offered) return false;
  c->accepted = true;
  c->accepted_ms = now;
  c->key_sends = 0;
  c->last_send = 0;
  Out.printf("[pair] accepted %04X\n", nodeIdFromMac(mac));
  return true;
}

bool MasterPair::ignore(const uint8_t mac[6]) {
  Cand* c = find(mac);
  if (!c) return false;
  c->used = false;
  return true;
}

bool MasterPair::blink(const uint8_t mac[6], uint32_t now) {
  Cand* c = find(mac);
  if (!c || !c->offered) return false;
  c->blink = true;
  sendOffer(*c);
  c->blink = false;  // one request = one blink; the next offers don't carry it
  (void)now;
  return true;
}

void MasterPair::joined(const uint8_t mac[6]) {
  Cand* c = find(mac);
  if (c && c->accepted) c->used = false;
}

void MasterPair::status(Print& out, uint32_t now) const {
  if (!active(now)) {
    bool any = false;
    for (auto& c : c_) any |= c.used;
    if (!any) { out.print(",\"pair\":{\"on\":false}"); return; }
  }
  out.printf(",\"pair\":{\"on\":true,\"left\":%lu,\"close\":%d,\"list\":[", active(now) ? (unsigned long)((until_ - now) / 1000) : 0UL,
             kCloseRssi);
  bool first = true;
  for (auto& c : c_) {
    if (!c.used) continue;
    char mac[18], code[8] = "";
    formatMac(c.mac, mac);
    if (c.offered) pairCodeText(c.secret.code, code);
    out.printf("%s{\"mac\":\"%s\",\"id\":\"%04X\",\"hw\":%u,\"caps\":%u,\"rssi\":%d,\"close\":%s,\"code\":\"%s\",\"state\":\"%s\"}",
               first ? "" : ",", mac, nodeIdFromMac(c.mac), c.req.hw, c.req.caps, c.rssi, c.offered ? "true" : "false", code,
               c.accepted ? "accepted" : c.offered ? "ready" : "far");
    first = false;
  }
  out.print("]}");
}

}  // namespace pairing
}  // namespace ss
