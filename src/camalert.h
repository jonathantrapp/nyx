// camalert.h - ties GPS + the on-flash camera map (/cameras.bin) to the screen, LED and beeper.
// The decision logic is in camera_map.h (PC-tested); this file only does I/O.
#pragma once
#include <Arduino.h>
#include "camera_map.h"

struct CamAlertView {
  bool valid;          // a camera is currently ahead
  uint32_t atMs;       // last time it was ahead
  uint16_t distM;
  bool close;          // inside the second-warning distance
  bool facing;         // only meaningful when dirKnown
  bool dirKnown;       // lens direction mapped and you are moving; otherwise "direction unknown"
  uint8_t maker;       // CamMaker
  uint32_t idx;        // database index of that camera
  double lat, lon;     // where it is
};

// Nearest mapped camera in any direction, for the idle screen. Refreshed once a second while GPS is usable.
static const float CAM_NEAREST_MAX_M = 8047.0f;   // 5 miles
struct CamNearest {
  bool valid;          // a fix exists and the search ran (even if nothing was found)
  bool found;          // a camera within CAM_NEAREST_MAX_M
  float distM;
  float bearingDeg;    // absolute, clockwise from north
  uint8_t maker;       // CamMaker
};
const CamNearest& camAlertNearest();

void camAlertBegin();                   // after the filesystem is mounted: loads /cameras.bin into PSRAM
bool camAlertReload();                   // re-read after a new file was uploaded
void camAlertService();                  // call every loop(); does its work about once a second
const CamAlertView& camAlertView();
bool camAlertLoaded();
const CamDb& camAlertDb();               // the loaded map (empty if none)
uint32_t camAlertAnnouncedCount();       // camera warnings spoken since boot
const char* camMakerName(uint8_t maker);
void camAlertPrint(Print& out);
void camAlertNearby(Print& out);         // up to 5 nearest cameras to the current position
