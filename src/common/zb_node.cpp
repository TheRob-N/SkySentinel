#include "zb_node.h"

#include <Arduino.h>
#include <string.h>

#include "ss_154.h"

#if defined(CONFIG_IDF_TARGET_ESP32C5) && defined(CONFIG_IEEE802154_ENABLED)
#define SS_HAVE_154 1
#include "esp_ieee802154.h"
#else
#define SS_HAVE_154 0
#endif

namespace zb {

using namespace ss;

#if SS_HAVE_154

namespace {

// Frames are copied out in the receive interrupt and handled in the main loop.
struct Rx {
  uint8_t len;          // MAC frame length without the FCS
  uint8_t ch;
  int8_t rssi;
  uint8_t data[127];
};
constexpr uint8_t kSlots = 8;
Rx q_[kSlots];
volatile uint8_t head_ = 0, tail_ = 0;
portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t frames_ = 0, dropped_ = 0;
uint32_t reported_ = 0;
bool ready_ = false, listening_ = false;
volatile uint8_t ch_ = 0;
uint32_t perCh_[16] = {0};

void IRAM_ATTR onRx(uint8_t* frame, esp_ieee802154_frame_info_t* info) {
  // frame[0] = PHY length (MAC frame + 2-byte FCS); the FCS bytes now hold RSSI / LQI.
  const uint8_t phr = frame[0] & 0x7F;
  frames_ = frames_ + 1;
  if (phr >= 5) {
    portENTER_CRITICAL_ISR(&mux_);
    const uint8_t next = (uint8_t)((head_ + 1) % kSlots);
    if (next == tail_) {
      dropped_ = dropped_ + 1;
    } else {
      Rx& r = q_[head_];
      r.len = (uint8_t)(phr - 2);
      r.ch = info ? info->channel : ch_;
      r.rssi = info ? info->rssi : -127;
      memcpy(r.data, frame + 1, r.len);
      head_ = next;
    }
    portEXIT_CRITICAL_ISR(&mux_);
  }
  esp_ieee802154_receive_handle_done(frame);
}

}  // namespace

bool supported() { return true; }

bool begin() {
  if (ready_) return true;
  if (esp_ieee802154_enable() != ESP_OK) return false;
  esp_ieee802154_event_cb_list_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.rx_done_cb = onRx;
  if (esp_ieee802154_event_callback_list_register(cbs) != ESP_OK) {
    esp_ieee802154_disable();
    return false;
  }
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(false);
  esp_ieee802154_sleep();
  ready_ = true;
  return true;
}

bool ready() { return ready_; }

void listen(uint8_t channel) {
  if (!ready_ || channel < k154First || channel > k154Last) return;
  ch_ = channel;
  esp_ieee802154_set_channel(channel);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_receive();
  listening_ = true;
}

void stop() {
  if (!ready_ || !listening_) return;
  esp_ieee802154_set_rx_when_idle(false);
  esp_ieee802154_sleep();
  listening_ = false;
}

bool listening() { return listening_; }

void poll(Sink sink, int8_t floor) {
  for (int guard = 0; guard < kSlots; ++guard) {
    Rx r;
    portENTER_CRITICAL(&mux_);
    const bool any = tail_ != head_;
    if (any) { r = q_[tail_]; tail_ = (uint8_t)((tail_ + 1) % kSlots); }
    portEXIT_CRITICAL(&mux_);
    if (!any) break;
    if (r.ch >= k154First && r.ch <= k154Last) ++perCh_[r.ch - k154First];
    if (r.rssi < floor) continue;
    Frame154 f;
    if (!parse154(r.data, r.len, f)) continue;
    const Match m = classify154(f);
    const Sighting s = make154Sighting(f, m, r.ch, r.rssi);
    char name[kMaxName + 1];
    const uint8_t nl = name154(f, m, name, sizeof(name));
    ++reported_;
    if (sink) sink(s, nl ? name : nullptr, nl);
  }
}

uint32_t frames() { return frames_; }
uint32_t reported() { return reported_; }
uint32_t dropped() { return dropped_; }
uint8_t channel() { return ch_; }
uint32_t heardOn(uint8_t c) { return c >= k154First && c <= k154Last ? perCh_[c - k154First] : 0; }

#else   // no 802.15.4 on this chip

bool supported() { return false; }
bool begin() { return false; }
bool ready() { return false; }
void listen(uint8_t) {}
void stop() {}
bool listening() { return false; }
void poll(Sink, int8_t) {}
uint32_t frames() { return 0; }
uint32_t reported() { return 0; }
uint32_t dropped() { return 0; }
uint8_t channel() { return 0; }
uint32_t heardOn(uint8_t) { return 0; }

#endif

}  // namespace zb
