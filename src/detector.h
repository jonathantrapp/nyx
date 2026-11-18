// detector.h - matches every captured row against the watchlist and raises alerts.
#pragma once
#include <Arduino.h>
#include "capture.h"

// What the OLED shows while a detection is fresh.
struct DetectView {
  bool valid;
  uint32_t atMs;        // last time this device was heard
  uint8_t mac[6];
  uint8_t tier, cat;
  bool ble;
  bool vendorOnly;      // OUI evidence, including via an AP's BSSID
  const char* vendor;
  char rule[40];
  int8_t rssi;
  uint16_t count;
  int8_t trend;         // dB change over the last ~4 s: + stronger (closer), - weaker
};

// Police/government gear heard recently, for the idle screen: devices the screen would show (not benign,
// not hidden categories) heard within the window. "strongest" = highest tier, then loudest.
struct DetectEntry {
  uint8_t cat, tier;
  bool ble, vendorOnly;
  const char* vendor;
  int8_t rssi;
  int8_t trend;         // dB over the last ~4 s
  uint32_t agoS;        // since last heard
};
struct DetectSummary {
  uint8_t count;        // distinct devices
  uint8_t likely;       // of those, likely or better on more than a maker match
  uint8_t n;            // entries filled in top[]
  DetectEntry top[4];   // strongest first: highest tier, then loudest
};
DetectSummary detectorSummary(uint32_t windowMs);

void detectorProcess(const CaptureRow& row);  // called for every row the logger drains
const DetectView& detectorView();
uint32_t detectorAlertCount();                // radio alerts raised since boot (benign matches excluded)
void detectorList(Print& out);                // "detections" serial command
void detectorSetMuted(bool muted);
bool detectorMuted();
void detectorSelfTest(Print& out); // synthetic matcher/policy tests; no RF or live-state changes
