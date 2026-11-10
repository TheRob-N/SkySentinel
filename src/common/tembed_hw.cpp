#ifdef SS_DISPLAY_TEMBED
#include "tembed_hw.h"

#include <SPI.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace ss {
namespace tembed {

static SemaphoreHandle_t spi_ = nullptr;
static bool begun_ = false;

void begin() {
  if (begun_) return;
  begun_ = true;
  gpio_hold_dis((gpio_num_t)PWR_EN);   // held low through deep sleep (deepSleep)
  gpio_hold_dis((gpio_num_t)TFT_BL);
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

static bool chargerRead(uint8_t reg, uint8_t& v) {
  Wire.beginTransmission(CHARGER_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)CHARGER_ADDR, 1) != 1) return false;
  v = (uint8_t)Wire.read();
  return true;
}
static bool chargerWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(CHARGER_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

bool usbPowered() {
  uint8_t v = 0;
  return chargerRead(0x11, v) && (v & 0x80);   // REG11 bit 7: VBUS_GD
}

bool batteryOff() {
  // REG09 bit 5 BATFET_DIS (bit 3 BATFET_DLY clear: right away). XPowersLib's shutdown().
  uint8_t v = 0;
  if (!chargerRead(0x09, v)) return false;
  v = (uint8_t)((v | 0x20) & ~0x08);
  return chargerWrite(0x09, v);
}

void deepSleep() {
  digitalWrite(TFT_BL, LOW);
  gpio_hold_en((gpio_num_t)TFT_BL);
  digitalWrite(PWR_EN, LOW);                 // CC1101, LEDs, peripherals off
  gpio_hold_en((gpio_num_t)PWR_EN);
  gpio_deep_sleep_hold_en();
  while (digitalRead(BACK_KEY) == LOW) delay(10);   // let go first, or it wakes at once
  delay(200);
  rtc_gpio_pullup_en((gpio_num_t)BACK_KEY);
  rtc_gpio_pulldown_dis((gpio_num_t)BACK_KEY);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BACK_KEY, 0);
  Serial.flush();
  esp_deep_sleep_start();
}

}  // namespace tembed
}  // namespace ss
#endif
