// ble_scan.h - passive BLE advertisement capture (NimBLE). Receive-only.
#pragma once
#include <Arduino.h>

void bleBegin();                    // init NimBLE as a passive observer
void bleScanSlice(uint32_t ms);     // listen for `ms`, called from the radio schedule
