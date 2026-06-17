/*
 * SkySentinel v0.2
 * Passive Flock Safety ALPR + FAA Remote ID drone detection
 * Hardware: ESP32-2432S028 (CYD)
 *
 * Board:      ESP32 Dev Module
 * Partition:  Huge APP (3MB No OTA / 1MB SPIFFS)
 * Library:    ggaljoen/TFT_eSPI fork (SoftSPI touch — no XPT2046 library needed)
 *
 * Pin summary:
 *   Display HSPI:  MOSI=13 MISO=12 SCLK=14 CS=15 DC=2  (via User_Setup.h)
 *   Touch SoftSPI: handled internally by ggaljoen TFT_eSPI fork
 *   SD VSPI:       MOSI=23 MISO=19 SCLK=18 CS=5
 *   Backlight:     GPIO21 (PWM)
 *   Boot button:   GPIO0 (active LOW)
 *   GPS (future):  RX2=GPIO27 (P3 pin 3)
 *   RGB LED:       R=4 G=16 B=17 (active LOW)
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <Preferences.h>
#include <math.h>

#include "flock.h"
#include "settings.h"
#include "ble_scan.h"
#include "wifi_scan.h"
#include "display.h"
#include "sd_log.h"
#include "drone_id.h"
#include "gps.h"

// ── Pins ─────────────────────────────────────────────────────────────────────
#define PIN_BL   21
#define SD_CS     5
#define BTN_PIN   0    // Boot button, active LOW
#define LED_R     4
#define LED_G    16
#define LED_B    17
// Speaker — not yet wired. Set to the actual GPIO once a speaker is added;
// playAlert() below no-ops safely if this stays -1.
#define PIN_SPEAKER -1

// ── Touch calibration (from CYD_Cal_ggaljoen, rotation 1) ────────────────────
// Touch calibration now lives in display.h (TOUCH_CAL_ROT1/ROT3 + getTouchCalData())

// ── Brightness PWM levels (0-255) ─────────────────────────────────────────────
#define BRIGHT_PWM_LOW   35
#define BRIGHT_PWM_MED   120
#define BRIGHT_PWM_HIGH  210

// ── Settings (defined here, extern'd in settings.h) ───────────────────────────
bool           settingSDLogging   = true;
BrightnessLevel settingBrightness = BRIGHT_HIGH;
VolumeLevel     settingVolume     = VOL_MED;
bool           settingFlipped     = false;
bool           settingBLEEnabled  = true;
bool           settingWiFiEnabled = true;

// ── GPS state (defined here, extern'd everywhere) ─────────────────────────────
volatile float   gpsLat        = 0.0f;
volatile float   gpsLon        = 0.0f;
volatile float   gpsSpeedKnots = 0.0f;
volatile float   gpsCourse     = 0.0f;
volatile float   gpsAltM       = 0.0f;
volatile uint8_t gpsSats       = 0;
volatile bool    gpsTimeValid  = false;
volatile uint8_t gpsHour       = 0;
volatile uint8_t gpsMinute     = 0;
volatile uint8_t gpsSecond     = 0;
volatile uint8_t gpsMonth      = 6;   // default to non-DST-boundary month
volatile uint8_t gpsDay        = 1;

// ── Objects ───────────────────────────────────────────────────────────────────
TFT_eSPI tft;

// ── Queues ────────────────────────────────────────────────────────────────────
QueueHandle_t detectionQueue;
QueueHandle_t droneQueue;

// ── State ─────────────────────────────────────────────────────────────────────
uint16_t flockCount = 0;
uint16_t droneCount = 0;
bool     sdReady    = false;
uint8_t  brightness = BRIGHT_PWM_HIGH;  // current actual PWM value applied

// Applies the current settingBrightness to the backlight. For AUTO, uses
// GPS local time (reusing the same NA timezone logic as the header clock)
// to pick day vs night brightness; falls back to HIGH if GPS has no fix yet.
void applyBrightness() {
  uint8_t pwm;
  switch (settingBrightness) {
    case BRIGHT_LOW:  pwm = BRIGHT_PWM_LOW;  break;
    case BRIGHT_MED:  pwm = BRIGHT_PWM_MED;  break;
    case BRIGHT_AUTO: {
      if (gpsTimeValid) {
        int8_t  tzOffset  = getNATimezoneOffset(gpsLon, gpsMonth, gpsDay);
        int16_t localHour = ((int16_t)gpsHour + tzOffset + 24) % 24;
        bool isNight = (localHour < 7) || (localHour >= 20);
        pwm = isNight ? BRIGHT_PWM_LOW : BRIGHT_PWM_HIGH;
      } else {
        pwm = BRIGHT_PWM_HIGH;  // no GPS fix yet — default bright
      }
      break;
    }
    case BRIGHT_HIGH:
    default:          pwm = BRIGHT_PWM_HIGH; break;
  }
  brightness = pwm;
  analogWrite(PIN_BL, pwm);
}

// One-shot audio alert — distinct short pattern per detection type.
// Safe no-op until PIN_SPEAKER is set to a real GPIO (speaker not yet wired).
void playAlert(bool isFlock) {
  if (settingVolume == VOL_OFF) return;
  if (PIN_SPEAKER < 0) return;  // no speaker hardware yet

  uint8_t vol;
  switch (settingVolume) {
    case VOL_LOW:  vol = 60;  break;
    case VOL_MED:  vol = 150; break;
    case VOL_HIGH: vol = 255; break;
    default: return;
  }

  if (isFlock) {
    // Two short descending tones for Flock Cam
    tone(PIN_SPEAKER, 1800, 90); delay(110);
    tone(PIN_SPEAKER, 1200, 90); delay(110);
    noTone(PIN_SPEAKER);
  } else {
    // Two short ascending tones for Drone
    tone(PIN_SPEAKER, 1000, 90); delay(110);
    tone(PIN_SPEAKER, 1600, 90); delay(110);
    noTone(PIN_SPEAKER);
  }
}

// ── Button ────────────────────────────────────────────────────────────────────
static bool     btnLast      = HIGH;
static uint32_t btnDownMs    = 0;
static bool     btnLongFired = false;

void bleTask(void*);
void handleButton();

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("[SkySentinel] v0.2 boot");

  // LEDs off (active LOW)
  pinMode(LED_R, OUTPUT); digitalWrite(LED_R, HIGH);
  pinMode(LED_G, OUTPUT); digitalWrite(LED_G, HIGH);
  pinMode(LED_B, OUTPUT); digitalWrite(LED_B, HIGH);

  // Boot button
  pinMode(BTN_PIN, INPUT_PULLUP);

  // Backlight
  pinMode(PIN_BL, OUTPUT);
  analogWrite(PIN_BL, BRIGHT_PWM_HIGH);  // safe default until settings load

  // Load NVS settings
  loadSettings();
  applyBrightness();  // now apply the real saved brightness setting

  // Display init
  tft.begin();
  tft.setRotation(settingFlipped ? 3 : 1);
  tft.fillScreen(TFT_BLACK);

  // Touch calibration — ggaljoen SoftSPI handles everything internally.
  // Real measured values per rotation (see display.h TOUCH_CAL_ROT1/ROT3).
  uint16_t cal[5];
  getTouchCalData(settingFlipped, cal);
  tft.setTouch(cal);

  // Splash
  displaySplash(tft);
  delay(1200);

  // SD card — init before queues/BLE to avoid heap fragmentation
  if (SD.begin(SD_CS)) {
    sdReady = true;
    if (settingSDLogging) sdLogInit();
    Serial.println("[SD] OK");
  } else {
    Serial.println("[SD] No card");
  }

  // FreeRTOS queues
  detectionQueue = xQueueCreate(32, sizeof(FlockDetection));
  droneQueue     = xQueueCreate(16, sizeof(DroneDetection));

  // BLE task on Core 0
  xTaskCreatePinnedToCore(bleTask, "BLE", 8192, NULL, 1, NULL, 0);

  // WiFi
  wifiScanInit();

  // GPS stub
  gpsInit();

  // Display init — creates sprite, draws initial UI
  displayInit(tft, settingFlipped);

  Serial.println("[SkySentinel] Running");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  FlockDetection fd;
  while (xQueueReceive(detectionQueue, &fd, 0) == pdTRUE) {
    flockCount++;
    displayAddFlock(fd);
    if (sdReady && settingSDLogging) sdLogFlock(fd);
    ledPulse(true);
    playAlert(true);
    Serial.printf("[FLOCK] %s  match=%02X  rssi=%d\n",
                  fd.mac, fd.matchReason, fd.rssi);
  }

  DroneDetection dd;
  while (xQueueReceive(droneQueue, &dd, 0) == pdTRUE) {
    droneCount++;
    displayAddDrone(dd);
    if (sdReady && settingSDLogging) sdLogDrone(dd);
    ledPulse(false);
    playAlert(false);
    Serial.printf("[DRONE] %s  UAS:%s  rssi=%d\n",
                  dd.mac, dd.uasID, dd.rssi);
  }

  if (settingWiFiEnabled) wifiScanTick(detectionQueue, droneQueue);

  displayTick(flockCount, droneCount);

  if (sdReady && settingSDLogging) sdLogTick();

  gpsTick();

  handleButton();
}

// ── Button: short press = cycle brightness, long press = cycle volume ────────
// Gives physical-button access to both new settings without touching the
// screen — useful brightness control while driving in bright/dark conditions.
void handleButton() {
  bool state = digitalRead(BTN_PIN);
  uint32_t now = millis();
  if (state == LOW && btnLast == HIGH) { btnDownMs = now; btnLongFired = false; }
  if (state == LOW && !btnLongFired && (now - btnDownMs) >= 2000) {
    btnLongFired = true;
    settingVolume = (VolumeLevel)((settingVolume + 1) % 4);
    saveSettings();
  }
  if (state == HIGH && btnLast == LOW && !btnLongFired) {
    settingBrightness = (BrightnessLevel)((settingBrightness + 1) % 4);
    applyBrightness();
    saveSettings();
  }
  btnLast = state;
}

// ── BLE Task: Core 0, passive scan ────────────────────────────────────────────
void bleTask(void*) {
  BLEDevice::init("");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(
    new FlockBLECallback(detectionQueue, droneQueue));
  pScan->setActiveScan(false);
  pScan->setInterval(100);
  pScan->setWindow(99);
  while (true) {
    if (settingBLEEnabled) { pScan->start(3, false); pScan->clearResults(); }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
