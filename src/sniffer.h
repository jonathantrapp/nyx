// sniffer.h - passive Wi-Fi capture, time-shared with passive BLE scanning.
// Receive-only: nothing in here ever transmits.
#pragma once
#include <Arduino.h>
#include "capture.h"
#include "config.h"

struct SnifferStats {
  volatile uint32_t seen;      // probe requests + beacons heard (before beacon rate-limit)
  volatile uint32_t queued;    // rows handed to the logger (Wi-Fi and BLE)
  volatile uint32_t dropped;   // queue was full -> logger too slow; tune dwell/limits
  volatile uint32_t perChannel[15];  // seen-frame count per channel (index = channel)
  volatile uint32_t bleSeen;   // BLE advertisements heard (before the per-address limit)
  volatile uint32_t dataSeen;  // Wi-Fi data frames heard (before the per-sender limit)
};

void snifferBegin();                    // start promiscuous mode, BLE, channel hopping
bool snifferPop(CaptureRow& out);       // non-blocking; false when queue is empty
bool snifferPush(const CaptureRow& row);  // used by the BLE scanner
const SnifferStats& snifferStats();
void snifferCountBle();
uint8_t snifferChannel();               // 0 while the radio is in its BLE slice
uint16_t snifferDwellMs();
void snifferSetDwellMs(uint16_t ms);
uint32_t snifferBeaconMinMs();
void snifferSetBeaconMinMs(uint32_t ms);
uint32_t snifferDataMinMs();           // 0 = data frames not logged
void snifferSetDataMinMs(uint32_t ms);
bool snifferBleEnabled();
void snifferSetBleEnabled(bool on);
