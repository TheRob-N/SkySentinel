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
static volatile uint8_t bandMask_ = (1u << kSubGhzBands) - 1;
static volatile bool jamAlerts_ = true;
static volatile uint8_t burstSamples_ = 6;
// Bursts waiting to go to the master (main loop: nextBurst). Small: bursts are rare and
// the loop drains it every pass; when full, the oldest goes.
static SubGhzBurst fifo_[16];
static uint8_t fifoHead_ = 0, fifoLen_ = 0;
static volatile uint8_t huntBand_ = 0xFF;
static volatile int8_t huntPeak_ = -127;
static volatile uint32_t huntMs_ = 0;
static int8_t histPeak_[kSubGhzBands][kHistSlots];
static uint8_t histBursts_[kSubGhzBands][kHistSlots];
static uint32_t histSlot_ = 0;   // the slot number (millis / kHistSlotMs) being filled
static bool histInit_ = false;
// Called under evMux: moves the history on to `slot`, clearing the slots it skips.
static void histAdvance(uint32_t slot) {
  if (!histInit_) {
    for (auto& b : histPeak_) for (auto& v : b) v = -127;
    memset(histBursts_, 0, sizeof(histBursts_));
    histInit_ = true;
    histSlot_ = slot;
    return;
  }
  uint32_t steps = slot - histSlot_;
  if (steps > kHistSlots) steps = kHistSlots;
  for (uint32_t k = 1; k <= steps; ++k) {
    const uint32_t i = (histSlot_ + k) % kHistSlots;
    for (uint8_t b = 0; b < kSubGhzBands; ++b) { histPeak_[b][i] = -127; histBursts_[b][i] = 0; }
  }
  histSlot_ = slot;
}

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
    const bool want = cats_ && (*cats_ & (catBit(CAT_PENTEST) | catBit(CAT_VEHICLE) | catBit(CAT_SUBGHZ)));
    if (!want) {
      if (running_) { tembed::SpiLock l; radio->standby(); }
      running_ = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    running_ = true;
    if (!bandMask_ && huntBand_ >= kSubGhzBands) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }   // every band off
    for (uint8_t b = 0; b < kSubGhzBands; ++b) {
      const uint8_t hunt = huntBand_;
      if (hunt < kSubGhzBands ? b != hunt : !(bandMask_ & (1u << b))) continue;   // HUNT: only that band
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
      // A burst needs burstSamples_ loud samples in the look (3 by default), not 1: a fob
      // press lasts 50 to 500 ms (many samples), while a lone 2.5 ms spike is the board's
      // own radio, the screen's SPI or USB. With 1 sample, a T-Embed on a desk "heard" a
      // burst on every band every few seconds (Nov 10). So the watch gets the Nth loudest.
      const uint8_t nth = burstSamples_ < 1 ? 1 : burstSamples_ > 40 ? 40 : burstSamples_;
      const uint32_t before = watch_.bursts(b);
      const uint32_t nowMs = millis();
      if (b == huntBand_) { huntPeak_ = peak; huntMs_ = nowMs ? nowMs : 1; }   // HUNT: the loudest sample, live
      watch_.onLook(b, s[20], s[40 - nth], nowMs);
      {
        portENTER_CRITICAL(&evMux);
        const uint32_t slot = nowMs / kHistSlotMs;
        if (!histInit_ || slot != histSlot_) histAdvance(slot);
        const uint32_t i = slot % kHistSlots;
        if (s[40 - nth] > histPeak_[b][i]) histPeak_[b][i] = s[40 - nth];
        if (watch_.bursts(b) != before && histBursts_[b][i] < 255) ++histBursts_[b][i];
        portEXIT_CRITICAL(&evMux);
      }
      if (watch_.bursts(b) != before) {
        SubGhzBurst r;
        if (watch_.recent(&r, 1) == 1) {
          portENTER_CRITICAL(&evMux);
          if (fifoLen_ == 16) { fifoHead_ = (uint8_t)((fifoHead_ + 1) % 16); --fifoLen_; }
          fifo_[(fifoHead_ + fifoLen_) % 16] = r;
          ++fifoLen_;
          portEXIT_CRITICAL(&evMux);
        }
      }
      Event ev;
      if (watch_.poll(ev) && jamAlerts_) {
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
void setHunt(uint8_t band) {
  huntBand_ = band < kSubGhzBands ? band : 0xFF;
  huntPeak_ = -127;
  huntMs_ = 0;
}
int8_t huntPeak(uint32_t* at_ms) {
  if (at_ms) *at_ms = huntMs_;
  return huntPeak_;
}
void history(uint8_t band, int8_t peak[kHistSlots], uint8_t bursts[kHistSlots]) {
  if (band >= kSubGhzBands) return;
  portENTER_CRITICAL(&evMux);
  const uint32_t now = millis() / kHistSlotMs;
  if (!histInit_ || now != histSlot_) histAdvance(now);   // slots nobody wrote since: empty
  for (uint8_t k = 0; k < kHistSlots; ++k) {               // oldest first
    const uint32_t i = (histSlot_ + 1 + k) % kHistSlots;
    peak[k] = histPeak_[band][i];
    bursts[k] = histBursts_[band][i];
  }
  portEXIT_CRITICAL(&evMux);
}

bool nextBurst(SubGhzBurst& out) {
  bool got = false;
  portENTER_CRITICAL(&evMux);
  if (fifoLen_) { out = fifo_[fifoHead_]; fifoHead_ = (uint8_t)((fifoHead_ + 1) % 16); --fifoLen_; got = true; }
  portEXIT_CRITICAL(&evMux);
  return got;
}

void setBandMask(uint8_t mask) { bandMask_ = (uint8_t)(mask & ((1u << kSubGhzBands) - 1)); }
uint8_t bandMask() { return bandMask_; }
void setBurstDb(int8_t db) { watch_.setBurstDb(db); }
void setJamAlerts(bool on) { jamAlerts_ = on; }
void setBurstSamples(uint8_t n) { burstSamples_ = n; }
void shutdown() {
  xSemaphoreTake(tembed::spi(), portMAX_DELAY);   // never given back: the watch stops here
  if (radio && ready_) radio->sleep();
}
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
