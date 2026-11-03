// LilyGO T-Embed CC1101 (ESP32-S3): pins and the shared SPI bus. From LilyGO's factory
// test (utilities.h) and Bruce's board file. The screen, the CC1101 and the SD slot share
// one SPI bus, so every use of it takes spiLock().
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ss {
namespace tembed {

constexpr int PWR_EN = 15;        // powers the CC1101, LEDs and peripherals, HIGH = on
constexpr int SPI_SCK = 11, SPI_MOSI = 9, SPI_MISO = 10;
constexpr int TFT_CS = 41, TFT_DC = 16, TFT_RST = 40, TFT_BL = 21;
constexpr int SD_CS = 13, NRF_CS = 44;
constexpr int CC_CS = 12, CC_GDO0 = 3, CC_GDO2 = 38, CC_SW1 = 47, CC_SW0 = 48;
constexpr int ENC_A = 4, ENC_B = 5, ENC_KEY = 0, BACK_KEY = 6;
constexpr int I2C_SDA = 8, I2C_SCL = 18;
constexpr uint8_t GAUGE_ADDR = 0x55;   // BQ27220 fuel gauge
constexpr int LED_PIN = 14;            // 8 x WS2812 around the jog wheel

// Once, before anything touches the bus: power on, all chip selects high, SPI up.
void begin();
SemaphoreHandle_t spi();
struct SpiLock {
  SpiLock() { xSemaphoreTake(spi(), portMAX_DELAY); }
  ~SpiLock() { xSemaphoreGive(spi()); }
};
// The antenna switch in front of the CC1101 has a path per band (from Bruce / LilyGO).
void antennaFor(float mhz);
// State of charge from the fuel gauge, 255 = no gauge / no battery.
uint8_t batteryPercent();

}  // namespace tembed
}  // namespace ss
