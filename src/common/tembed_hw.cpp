#ifdef SS_DISPLAY_TEMBED
#include "tembed_hw.h"

#include <SPI.h>
#include <Wire.h>

namespace ss {
namespace tembed {

static SemaphoreHandle_t spi_ = nullptr;
static bool begun_ = false;

void begin() {
  if (begun_) return;
  begun_ = true;
  pinMode(PWR_EN, OUTPUT);
  digitalWrite(PWR_EN, HIGH);
  for (int cs : {TFT_CS, SD_CS, NRF_CS, CC_CS}) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
  pinMode(CC_SW0, OUTPUT);
  pinMode(CC_SW1, OUTPUT);
  spi_ = xSemaphoreCreateMutex();
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(20);
}

SemaphoreHandle_t spi() {
  if (!spi_) spi_ = xSemaphoreCreateMutex();
  return spi_;
}

void antennaFor(float mhz) {
  // SW1 SW0: 1 0 = 315 MHz path, 1 1 = 433 MHz path, 0 1 = 868 / 915 MHz path
  if (mhz <= 350.0f) { digitalWrite(CC_SW1, HIGH); digitalWrite(CC_SW0, LOW); }
  else if (mhz < 468.0f) { digitalWrite(CC_SW1, HIGH); digitalWrite(CC_SW0, HIGH); }
  else { digitalWrite(CC_SW1, LOW); digitalWrite(CC_SW0, HIGH); }
}

uint8_t batteryPercent() {
  // BQ27220: StateOfCharge (0x2C, %), Voltage (0x08, mV)
  auto rd = [](uint8_t reg, uint16_t& v) {
    Wire.beginTransmission(GAUGE_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)GAUGE_ADDR, 2) != 2) return false;
    v = (uint16_t)(Wire.read() | (Wire.read() << 8));
    return true;
  };
  uint16_t soc = 0, mv = 0;
  if (!rd(0x2C, soc) || !rd(0x08, mv)) return 255;
  if (mv < 2500) return 255;   // no cell
  return (uint8_t)(soc > 100 ? 100 : soc);
}

}  // namespace tembed
}  // namespace ss
#endif
