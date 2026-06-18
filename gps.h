/*
 * gps.h — SkySentinel
 * Created by:
 * Rob Neyhard
 * Copyright (c) 2026
 * GPS stub — placeholder until ATGM336H module arrives
 * Wire: GPS TX → GPIO27 (P3 pin 3), VCC → P3 pin 4, GND → P3 pin 1
 * Library: TinyGPS++ (install via Library Manager)
 */
#pragma once

// GPS state (defined in SkySentinel.ino, updated here when GPS is active)
extern volatile float   gpsLat;
extern volatile float   gpsLon;
extern volatile float   gpsSpeedKnots;
extern volatile float   gpsCourse;
extern volatile float   gpsAltM;
extern volatile uint8_t gpsSats;
extern volatile bool    gpsTimeValid;
extern volatile uint8_t gpsHour;
extern volatile uint8_t gpsMinute;
extern volatile uint8_t gpsSecond;
extern volatile uint8_t gpsMonth;
extern volatile uint8_t gpsDay;

// Stub: does nothing until GPS is wired and library is enabled
void gpsInit()  { /* uncomment body when GPS arrives */ }
void gpsTick()  { /* uncomment body when GPS arrives */ }

/*
 * When GPS arrives, implement like this:
 *
 * #include <TinyGPS++.h>
 * TinyGPSPlus gps;
 * HardwareSerial gpsSerial(2);   // UART2
 *
 * void gpsInit() {
 *   gpsSerial.begin(9600, SERIAL_8N1, 27, -1);  // RX=27, no TX needed
 * }
 *
 * void gpsTick() {
 *   while (gpsSerial.available()) {
 *     gps.encode(gpsSerial.read());
 *   }
 *   if (gps.location.isValid()) {
 *     gpsLat = gps.location.lat();
 *     gpsLon = gps.location.lng();
 *     gpsSats = gps.satellites.value();
 *   }
 *   if (gps.speed.isValid())   gpsSpeedKnots = gps.speed.knots();
 *   if (gps.course.isValid())  gpsCourse     = gps.course.deg();
 *   if (gps.altitude.isValid()) gpsAltM      = gps.altitude.meters();
 *   if (gps.time.isValid()) {
 *     gpsTimeValid = true;
 *     gpsHour   = gps.time.hour();
 *     gpsMinute = gps.time.minute();
 *     gpsSecond = gps.time.second();
 *   }
 *   if (gps.date.isValid()) {
 *     gpsMonth = gps.date.month();
 *     gpsDay   = gps.date.day();
 *   }
 * }
 */
