// tft_demo.h - sample data for every screen. Used by the "tft demo" serial command (to look at a
// screen on the real display with no GPS or radio hit) and by the desktop preview, so both show
// exactly the same thing. Everything here is invented; nothing is read from the device.
#pragma once
#include <math.h>
#include <string.h>
#include "menu_logic.h"
#include "tft_model.h"
#include "watchlist.h"

// Metres in the heading-up frame (right, ahead) -> east/north.
inline void tftDemoEnu(float headingDeg, float right, float ahead, float& east, float& north) {
  const float h = headingDeg * static_cast<float>(M_PI / 180.0);
  east = right * cosf(h) + ahead * sinf(h);
  north = ahead * cosf(h) - right * sinf(h);
}

// Camera demo variants: which way the warned-about camera's lens points.
enum TftDemoFacing : uint8_t { TFT_DEMO_TOWARD = 0, TFT_DEMO_AWAY = 1, TFT_DEMO_NODIR = 2 };

inline void tftDemoModel(TftModel& m, TftScreen screen, bool close = false, uint8_t facing = TFT_DEMO_TOWARD) {
  m.screen = screen;
  m.demo = true;
  m.clockMinutes = 21 * 60 + 47;
  m.ble = true; m.wifi = true; m.gps = 2; m.sats = 9; m.muted = false;
  m.alertCount = 3; m.lastAlertAgoS = 12 * 60; m.mappedCameras = 145851;
  m.lastAlertCam = false; m.lastAlertCode = WL_CAT_BODYCAM;
  m.nearest.searched = true; m.nearest.found = true; m.nearest.distM = 1930;
  m.nearest.relDeg = 38.0f; m.nearest.headingKnown = true; m.nearest.maker = CAM_MAKER_FLOCK;
  m.police.count = 4; m.police.likely = 2; m.police.n = 4;
  {
    struct { uint8_t cat, tier; bool ble, vo; int8_t rssi, trend; uint32_t ago; const char* v; } d[4] = {
        {WL_CAT_BODYCAM, WL_LIKELY, true, false, -58, 6, 2, "Axon"},
        {WL_CAT_FLEET_MODEM, WL_LIKELY, false, false, -63, 0, 4, "Sierra Wireless"},
        {WL_CAT_GEAR, WL_POSSIBLE, false, true, -81, -5, 40, "Motorola"},
        {WL_CAT_TACTICAL, WL_POSSIBLE, true, false, -86, -4, 3, "Unknown"}};
    for (int k = 0; k < 4; k++) {
      TftModel::PoliceEntry& p = m.police.top[k];
      p.cat = d[k].cat; p.tier = d[k].tier; p.ble = d[k].ble; p.vendorOnly = d[k].vo;
      p.rssi = d[k].rssi; p.trend = d[k].trend; p.agoS = d[k].ago;
      strncpy(p.vendor, d[k].v, sizeof(p.vendor) - 1);
    }
  }
  m.storageOk = true; m.storageWriteError = false; m.storageLow = false; m.freeMb = 2.4f;

  if (screen == TftScreen::CAMERA) {
    const float heading = 35.0f;
    m.cam.headingDeg = heading;
    m.cam.speedMph = 38.0f;
    m.cam.maker = CAM_MAKER_FLOCK;
    m.cam.close = close;
    m.cam.facing = facing != TFT_DEMO_AWAY;
    m.cam.dirKnown = facing != TFT_DEMO_NODIR;
    m.cam.gridE = 337.0f; m.cam.gridN = 612.0f;

    const float ahead = close ? 120.0f : 330.0f, right = close ? 6.0f : 14.0f;
    float e, n;
    tftDemoEnu(heading, right, ahead, e, n);
    m.cam.targetEast = e; m.cam.targetNorth = n;
    m.cam.distM = static_cast<uint16_t>(sqrtf(right * right + ahead * ahead));
    m.cam.rangeM = m.cam.distM * 1.25f + 60.0f;
    if (m.cam.rangeM < 250.0f) m.cam.rangeM = 250.0f;

    int k = 0;
    auto add = [&](float r, float a, uint16_t dir, uint8_t flags) {
      float ce, cn;
      tftDemoEnu(heading, r, a, ce, cn);
      m.cam.cams[k++] = MapCam{ce, cn, dir, CAM_MAKER_FLOCK, flags, static_cast<uint32_t>(k)};
    };
    if (facing == TFT_DEMO_NODIR) add(right, ahead, CAM_DIR_UNKNOWN, 0);   // the one being warned about
    else add(right, ahead, facing == TFT_DEMO_AWAY ? 35 : 215, CAM_FLAG_DIR);
    add(-70.0f, ahead + 90.0f, 40, CAM_FLAG_DIR);        // another further up on the left
    add(150.0f, ahead * 0.55f, CAM_DIR_UNKNOWN, 0);      // side street, direction not mapped
    add(60.0f, -40.0f, 290, CAM_FLAG_DIR);               // behind you
    m.cam.camCount = k;

    m.cam.trailCount = 30;
    for (int i = 0; i < m.cam.trailCount; i++) {
      const float d = 12.0f * (i + 1);
      tftDemoEnu(heading, -0.0007f * d * d, -d, m.cam.trailEast[i], m.cam.trailNorth[i]);
    }
  } else if (screen == TftScreen::RF) {
    m.rf.cat = WL_CAT_BODYCAM;
    m.rf.tier = WL_LIKELY;
    m.rf.ble = true;
    m.rf.rssi = -58;
    strncpy(m.rf.vendor, "Axon", sizeof(m.rf.vendor) - 1);
    m.rf.count = 4;
    m.rf.others = 2;
    m.rf.trend = 6;
  } else if (screen == TftScreen::FOLLOWER) {
    m.follow.places = 4;
    m.follow.minutes = 38;
    m.follow.ble = true;
    const float e[4] = {0, 2400, 5200, 8100}, n[4] = {0, 1100, -300, 1800};
    for (int i = 0; i < 4; i++) { m.follow.east[i] = e[i]; m.follow.north[i] = n[i]; }
  } else if (screen == TftScreen::MENU) {
    MenuLogic menu;
    menu.open(0);
    menu.rotate(1, 0);                     // selection on "Brightness"
    menu.values.volume = 60; menu.values.brightness = 80; menu.values.mode = 1; menu.values.utcHours = -5;
    m.menu.sel = static_cast<int8_t>(menu.selected());
    for (int i = 0; i < TFT_MENU_ROWS; i++) {
      strncpy(m.menu.label[i], MenuLogic::label(i), sizeof(m.menu.label[i]) - 1);
      menu.valueText(i, m.menu.value[i], sizeof(m.menu.value[i]));
    }
  } else if (screen == TftScreen::BOOT) {
    strncpy(m.bootNote, "BOARD OK", sizeof(m.bootNote) - 1);
  }
}
