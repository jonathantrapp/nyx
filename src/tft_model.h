// tft_model.h - everything a colour-screen frame needs, as plain data. tft_ui.cpp fills it from
// the live firmware state; the desktop preview fills it with sample values. The screens in
// tft_screens.cpp read nothing else, so what you see in the preview is what the unit draws.
#pragma once
#include <stdint.h>
#include "minimap.h"

// 320 x 240, landscape.
static const int TFT_W = 320;
static const int TFT_H = 240;

enum class TftScreen : uint8_t { BOOT, LISTENING, CAMERA, RF, FOLLOWER, MENU };

static const int TFT_MAP_CAMS = 24;
static const int TFT_FOLLOW_PLACES = 8;
static const int TFT_MENU_ROWS = 6;

struct TftModel {
  TftScreen screen = TftScreen::LISTENING;
  bool demo = false;                // sample data (the "tft demo" command, or the preview tool)
  uint32_t nowMs = 0;               // animation clock

  // ---- status bar (every screen) ----
  int16_t clockMinutes = -1;        // local time as minutes since midnight; -1 = unknown
  bool ble = true;
  bool wifi = true;
  uint8_t gps = 0;                  // 0 no module, 1 searching, 2 fix, 3 simulated
  uint8_t sats = 0;
  bool muted = false;
  bool carMode = false;

  // ---- listening ----
  uint32_t alertCount = 0;          // camera warnings + radio detections since boot
  int32_t lastAlertAgoS = -1;       // seconds since the last camera/radio alert was active; -1 = none since boot
  bool lastAlertCam = false;        // true: a mapped camera (lastAlertCode = CamMaker); false: radio (WL_CAT_*)
  uint8_t lastAlertCode = 0;
  struct PoliceEntry {
    uint8_t cat = 0, tier = 0;      // WL_CAT_*, WL_*
    bool ble = false, vendorOnly = false;
    int8_t rssi = -90;
    int8_t trend = 0;               // dB over the last ~4 s: + stronger (closer), - weaker
    uint32_t agoS = 0;              // since last heard
    char vendor[20] = "";
  };
  struct {                          // police/government gear heard in the last 5 minutes (radar-detector style)
    uint8_t count = 0;              // distinct devices
    uint8_t likely = 0;             // of those, likely or better on more than a maker match
    uint8_t n = 0;                  // entries in top[]
    PoliceEntry top[4];             // strongest first
  } police;
  struct {
    bool searched = false;          // GPS + camera map available and the search ran
    bool found = false;             // a mapped camera within 5 miles
    uint16_t distM = 0;
    float relDeg = 0;               // direction to it: relative to travel if headingKnown, else from north
    bool headingKnown = false;
    uint8_t maker = 0;              // CamMaker
  } nearest;
  uint32_t mappedCameras = 0;       // 0 = no camera map on the unit
  bool storageOk = true;            // LittleFS mounted
  bool storageWriteError = false;
  bool storageLow = false;
  float freeMb = 0;

  // ---- camera ----
  struct {
    uint8_t maker = 0;              // CamMaker
    uint16_t distM = 0;
    bool close = false;
    bool facing = true;             // only meaningful when dirKnown
    bool dirKnown = true;           // false: lens direction not mapped (or stopped), show "direction unknown"
    float headingDeg = 0;          // direction of travel; the map is drawn heading-up
    float speedMph = 0;
    float targetEast = 0, targetNorth = 0;   // the camera being warned about, metres from you
    float rangeM = 400;             // metres from you to the top of the map
    float gridE = 0, gridN = 0;     // your position modulo 1000 m east/north: lets the grid slide as you drive
    int camCount = 0;
    MapCam cams[TFT_MAP_CAMS];
    int trailCount = 0;             // newest first; metres east/north of you
    float trailEast[Trail::N], trailNorth[Trail::N];
  } cam;

  // ---- radio detection ----
  struct {
    uint8_t cat = 0;                // WL_CAT_*
    uint8_t tier = 0;               // WL_*
    bool ble = false;
    int8_t rssi = -90;
    char vendor[24] = "";
    uint16_t count = 0;             // frames heard from this device
    bool vendorOnly = false;        // matched on the maker's address block only (weaker evidence)
    int8_t trend = 0;               // dB over the last ~4 s: + stronger (closer)
    uint8_t others = 0;             // other police devices heard in the last 5 minutes
  } rf;

  // ---- follower (screen only: no detection logic feeds it yet) ----
  struct {
    uint8_t places = 0;
    uint16_t minutes = 0;
    bool ble = true;
    float east[TFT_FOLLOW_PLACES], north[TFT_FOLLOW_PLACES];   // oldest first
  } follow;

  // ---- settings menu (screen MENU) ----
  struct {
    int8_t sel = 0;
    bool editing = false;
    char label[TFT_MENU_ROWS][16] = {};
    char value[TFT_MENU_ROWS][14] = {};
  } menu;

  // ---- brief overlay on any screen: "VOLUME 60", "MUTED" ----
  struct {
    bool active = false;
    char text[16] = "";
    int8_t pct = -1;                  // 0..100 draws a bar; -1 = text only
  } toast;

  // ---- boot ----
  char bootNote[28] = "";
};
