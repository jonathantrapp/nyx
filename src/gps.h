// gps.h - GNSS module on UART1. Finds the module's baud rate by itself, parses NMEA, logs the
// track, and sets the logger's clock from satellite time. Receive-only: nothing is sent to the module.
#pragma once
#include <Arduino.h>
#include "nmea.h"

void gpsBegin();
void gpsRaw(uint16_t lines);   // "gps raw N": echo the next N raw NMEA lines to the console
void gpsSendCas(const char* body);   // one CASIC config sentence to the module (checksum added), e.g. "PCAS10,3"
void gpsService();                  // call every loop()
const GpsFix& gpsFix();             // the module's fix, or the simulated one
bool gpsUsable();                   // position fix that is less than 3 s old (or simulated)
uint32_t gpsBaud();                 // 0 while still searching for the module's baud rate
bool gpsModuleTalking();            // valid NMEA seen in the last 5 s
int64_t gpsUtcNow();                // seconds since 1970 (UTC) from the module, 0 if it has no time yet
void gpsPrint(Print& out);

// Bench testing without satellites: pretend to drive from a point, moving along `courseDeg`.
void gpsSim(double lat, double lon, float speedMps, float courseDeg);
void gpsSimOff();
bool gpsSimActive();
