#ifdef SS_SUBGHZ
#include "subghz_node.h"

#include <RadioLib.h>
#include <SPI.h>

#include "tembed_hw.h"

namespace ss {
namespace subghz {

static CC1101* radio = nullptr;
static volatile uint16_t* cats_ = nullptr;
static bool ready_ = false;
static volatile bool running_ = false;
static SubGhzWatch watch_;
static volatile uint8_t band_ = 0;
static volatile uint32_t sweeps_ = 0;
static portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;
static Event pending_{};
static bool hasPending_ = false;

static bool tune(float mhz) {
  tembed::SpiLock l;
  tembed::antennaFor(mhz);
  if (radio->setFrequency(mhz) != RADIOLIB_ERR_NONE) return false;
  return radio->receiveDirectAsync() == RADIOLIB_ERR_NONE;   // live signal level in RSSI register
}

static int8_t rssiNow() {
  tembed::SpiLock l;
  return (int8_t)radio->getRSSI();
}

static void task(void*) {
  int8_t s[40];
  for (;;) {
    const bool want = cats_ && (*cats_ & (catBit(CAT_PENTEST) | catBit(CAT_VEHICLE)));
    if (!want) {
      if (running_) { tembed::SpiLock l; radio->standby(); }
      running_ = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    running_ = true;
    for (uint8_t b = 0; b < kSubGhzBands; ++b) {
      band_ = b;
      if (!tune(kSubGhzBandList[b].mhz)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
      vTaskDelay(pdMS_TO_TICKS(3));   // let the RSSI settle on the new frequency
      int8_t peak = -127;
      for (uint8_t i = 0; i < 40; ++i) {
        s[i] = rssiNow();
        if (s[i] > peak) peak = s[i];
        delayMicroseconds(2500);
        if ((i & 7) == 7) vTaskDelay(1);
      }
      // median by partial sort
      for (uint8_t i = 1; i < 40; ++i)
        for (uint8_t j = i; j > 0 && s[j] < s[j - 1]; --j) { const int8_t t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
      watch_.onLook(b, s[20], peak, millis());
      Event ev;
      if (watch_.poll(ev)) {
        portENTER_CRITICAL(&evMux);
        pending_ = ev;
        hasPending_ = true;
        portEXIT_CRITICAL(&evMux);
      }
    }
    sweeps_ = sweeps_ + 1;
  }
}

bool begin(volatile uint16_t* category_mask) {
  cats_ = category_mask;
  tembed::begin();
  static Module mod(tembed::CC_CS, tembed::CC_GDO0, RADIOLIB_NC, tembed::CC_GDO2, SPI);
  static CC1101 r(&mod);
  radio = &r;
  {
    tembed::SpiLock l;
    tembed::antennaFor(433.92f);
    // 433.92 MHz, 4.8 kbps, 5 kHz deviation, 650 kHz receive bandwidth (fobs drift), lowest power
    // (unused: never transmits), 16-bit preamble.
    if (radio->begin(433.92f, 4.8f, 5.0f, 650.0f, -30, 16) != RADIOLIB_ERR_NONE) return false;
    radio->setOOK(true);
  }
  ready_ = true;
  xTaskCreate(task, "subghz", 4096, nullptr, 1, nullptr);
  return true;
}

bool ready() { return ready_; }
bool running() { return running_; }
uint8_t currentBand() { return band_; }
uint32_t sweeps() { return sweeps_; }
const SubGhzWatch& watch() { return watch_; }

bool pollEvent(Event& out) {
  bool got = false;
  portENTER_CRITICAL(&evMux);
  if (hasPending_) { out = pending_; hasPending_ = false; got = true; }
  portEXIT_CRITICAL(&evMux);
  return got;
}

}  // namespace subghz
}  // namespace ss
#endif
