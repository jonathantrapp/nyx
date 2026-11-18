// tft_ui.cpp - see tft_ui.h.
//
// Threading: the main loop() only decides WHAT to show and fills a TftModel (cheap). A separate
// low-priority task, pinned to core 0 where it only gets the time the radio stack leaves over,
// draws the frame 16 rows at a time and pushes it to the panel. On the bench the loop's own core
// spends most of its time in Wi-Fi/BLE interrupts (a plain 200k-iteration loop took 20 ms, about 5x
// slower than the clock suggests), so drawing in loop() would have slowed capture; a task cannot.
#include "tft_ui.h"
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include "camalert.h"
#include "controls.h"
#include "config.h"
#include "detector.h"
#include "gfx.h"
#include "gps.h"
#include "gps_policy.h"
#include "logger.h"
#include "loop_watchdog.h"
#include "pins.h"
#include "sniffer.h"
#include "tft_demo.h"
#include "tft_driver.h"
#include "tft_model.h"
#include "tft_screens.h"
#include "ui.h"
#include "watchlist.h"

static Preferences gPrefs;
static bool gPrefsReady = false;
static TftMode gMode = TftMode::AUTO;
static int16_t gUtcOffsetMin = 0;
static uint8_t gBrightness = 100;

static TftModel gModel;                  // what the loop last built; the loop owns it
static TftModel gShared;                 // handed to the render task under gMutex
static Trail gTrail;
static bool gActive = false;
static uint32_t gLastFrameMs = 0, gLastTrailMs = 0;
static TftScreen gDemoScreen = TftScreen::LISTENING;
static bool gDemoClose = false;
static uint8_t gDemoFacing = 0;   // TftDemoFacing
static uint32_t gDemoUntilMs = 0;
static float gLastHeading = 0;
static bool gHeadingKnown = false;   // a real course has been seen since boot (the idle arrow needs one)
static double gRefLat = 0;               // fixed latitude for the grid so it does not creep as you drive
static bool gHaveRef = false;

// ---- render task ----------------------------------------------------------------------------

static TaskHandle_t gTask = nullptr;
static SemaphoreHandle_t gMutex = nullptr;
static volatile bool gRequest = false;   // a new model is waiting in gShared
static volatile bool gBusy = false;      // the task is drawing a frame
static volatile bool gHold = false;      // the loop wants the strip buffer and the bus for itself
static volatile bool gStopTask = false;
static volatile bool gTaskExited = true;
static volatile uint32_t gFrameMs = 0, gFrameMaxMs = 0, gStripWorstUs = 0;

static void drawStrip(const TftModel& m, int strip, bool push,TftGeometryCache* cache=nullptr) {
  gfx::Canvas canvas;
  canvas.attachStrip(tftStripBuffer(), TFT_W, TFT_H, strip * TFT_STRIP_ROWS, TFT_STRIP_ROWS);
  const uint32_t t0 = micros();
  if(cache)tftRenderPrepared(canvas,m,*cache);else tftRender(canvas, m);
  const uint32_t us = micros() - t0;
  if (us > gStripWorstUs) gStripWorstUs = us;
  if (push) tftFrameRows(TFT_STRIP_ROWS);
}

static void drawFrame(const TftModel& m) {
  const uint32_t t0 = millis();
  tftFrameBegin();
  static TftGeometryCache cache; cache.reset();
  for (int s = 0; s < TFT_H / TFT_STRIP_ROWS; s++) drawStrip(m, s, true,&cache);
  tftFrameEnd();
  gFrameMs = millis() - t0;
  if (gFrameMs > gFrameMaxMs) gFrameMaxMs = gFrameMs;
}

static void renderTask(void*) {
  static TftModel local;                 // static: keeps the task stack small
  while (!gStopTask) {
    bool run = false;
    xSemaphoreTake(gMutex, portMAX_DELAY);
    if (gRequest && !gHold) { local = gShared; gRequest = false; gBusy = true; run = true; }
    xSemaphoreGive(gMutex);
    if (!run) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    drawFrame(local);
    gBusy = false;
  }
  gTaskExited = true;
  vTaskDelete(nullptr);
}

static void startTask() {
  if (gTask) return;
  if (!gMutex) gMutex = xSemaphoreCreateMutex();
  gStopTask = false;
  gTaskExited = false;
  gRequest = false;
  gBusy = false;
  // Priority 1 on core 0: below every radio task, so it only uses idle time. Pinned because it uses floats.
  xTaskCreatePinnedToCore(renderTask, "tft", 6144, nullptr, 1, &gTask, 0);
}

static void stopTask() {
  if (!gTask) return;
  gStopTask = true;
  const uint32_t t0 = millis();
  while (!gTaskExited && millis() - t0 < 2000) delay(5);
  gTask = nullptr;
}

// Take the strip buffer and the bus away from the task for a moment (dump, bench, splash).
static void hold(bool on) {
  if (!gMutex) { gHold = on; return; }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  gHold = on;
  xSemaphoreGive(gMutex);
  if (on) { const uint32_t t0 = millis(); while (gBusy && millis() - t0 < 3000) delay(2); }
}

// ---- setup, settings ------------------------------------------------------------------------

static void loadPrefs() {
  gPrefsReady = gPrefs.begin("nyx_tft", false);
  if (!gPrefsReady) return;
  const uint8_t mode = gPrefs.getUChar("mode", 0);
  gMode = mode <= 2 ? static_cast<TftMode>(mode) : TftMode::AUTO;
  gUtcOffsetMin = static_cast<int16_t>(gPrefs.getShort("utc", 0));
  // Panel settings saved by "tft rot|bgr|inv|mhz"; missing keys keep the config.h defaults.
  TftPanelCfg c = tftDriverDefaultConfig();
  const uint8_t rot = gPrefs.getUChar("rot", c.rotation);
  c.rotation = (rot == 1 || rot == 3) ? rot : c.rotation;
  c.bgr = gPrefs.getBool("bgr", c.bgr);
  c.invert = gPrefs.getBool("inv", c.invert);
  const uint32_t hz = gPrefs.getULong("hz", c.hz);
  c.hz = (hz >= 1000000UL && hz <= 80000000UL) ? hz : c.hz;
  const uint8_t chip = gPrefs.getUChar("chip", c.chip);
  c.chip = chip <= TFT_CHIP_ST7789 ? chip : c.chip;
  tftDriverSetConfig(c);
}

static const char* modeName(TftMode m) { return m == TftMode::ON ? "on" : (m == TftMode::OFF ? "off" : "auto"); }

void tftUiBegin() {
  loadPrefs();
  if (gMode == TftMode::OFF) { Serial.println("[tft] off (saved setting)"); return; }
  gActive = tftDriverBegin(gMode == TftMode::ON);
  if (gActive) {
    tftSetBacklight(gBrightness);
    Serial.printf("[tft] %s %s (id 0x%06lX), %dx%d, drawn in strips by a background task\n",
                  tftDriverConfig().chip == TFT_CHIP_ST7789 ? "ST7789" : "ILI9341",
                  tftDriverProbe() == TftProbe::FOUND ? "found" : "assumed (forced on, ID not readable)",
                  static_cast<unsigned long>(tftDriverId()), TFT_W, TFT_H);
  } else {
    const char* why = tftDriverProbe() == TftProbe::ERROR ? "driver error (SPI bus or memory)" :
                      "no supported TFT answered (screen not wired, or SDO not connected: try 'tft on')";
    Serial.printf("[tft] not running: %s\n", why);
  }
}

bool tftUiActive() { return gActive; }

void tftUiSplash(const char* note) {
  if (!gActive) return;
  static TftModel splash;
  splash = TftModel();
  splash.screen = TftScreen::BOOT;
  strlcpy(splash.bootNote, note ? note : "", sizeof(splash.bootNote));
  hold(true);
  drawFrame(splash);                     // synchronous, at boot, before the task exists
  hold(false);
}

void tftUiSetBrightness(uint8_t percent) {
  gBrightness = percent > 100 ? 100 : percent;
  if (gActive) tftSetBacklight(gBrightness);
}

bool tftUiSetMode(TftMode mode) {
  gMode = mode;
  bool saved = true;
  if (gPrefsReady) saved = gPrefs.putUChar("mode", static_cast<uint8_t>(mode)) == 1;
  if (mode == TftMode::OFF) {
    if (gActive) { stopTask(); tftDriverEnd(); gActive = false; }
  } else if (!gActive) {
    gActive = tftDriverBegin(mode == TftMode::ON);
    if (gActive) tftSetBacklight(gBrightness);
  }
  return saved;
}

bool tftUiSetPanel(const char* key, long value) {
  TftPanelCfg c = tftDriverConfig();
  const TftPanelCfg d = tftDriverDefaultConfig();
  const bool reset = strcmp(key, "reset") == 0;
  if (reset) c = d;
  else if (strcmp(key, "rot") == 0 && (value == 1 || value == 3)) c.rotation = static_cast<uint8_t>(value);
  else if (strcmp(key, "bgr") == 0 && (value == 0 || value == 1)) c.bgr = value == 1;
  else if (strcmp(key, "inv") == 0 && (value == 0 || value == 1)) c.invert = value == 1;
  else if (strcmp(key, "mhz") == 0 && value >= 1 && value <= 80) c.hz = static_cast<uint32_t>(value) * 1000000UL;
  else if (strcmp(key, "chip") == 0 && (value == TFT_CHIP_ILI9341 || value == TFT_CHIP_ST7789)) {
    // Each controller gets its usual colour order and inversion; bgr/inv can still be flipped after.
    c.chip = static_cast<uint8_t>(value);
    c.bgr = value == TFT_CHIP_ILI9341;
    c.invert = false;   // seen 2026-11-13: this board's ST7789 shows a negative with inversion on
  }
  else return false;
  tftDriverSetConfig(c);
  if (gPrefsReady) {
    if (reset) { gPrefs.remove("rot"); gPrefs.remove("bgr"); gPrefs.remove("inv"); gPrefs.remove("hz"); gPrefs.remove("chip"); }
    else {
      gPrefs.putUChar("rot", c.rotation); gPrefs.putBool("bgr", c.bgr); gPrefs.putBool("inv", c.invert);
      gPrefs.putULong("hz", c.hz); gPrefs.putUChar("chip", c.chip);
    }
  }
  // Re-initialise the panel so the change shows at once (the render task restarts on the next loop).
  if (gActive) {
    stopTask();
    tftDriverEnd();
    gActive = tftDriverBegin(gMode == TftMode::ON || tftDriverProbe() != TftProbe::FOUND);
    if (gActive) tftSetBacklight(gBrightness);
    gFrameMs=0;gFrameMaxMs=0;gStripWorstUs=0; // timing belongs to this panel/clock configuration
  }
  return true;
}

static bool gFrozen = false;
void tftUiFreeze(bool on) { gFrozen = on; }
bool tftUiFrozen() { return gFrozen; }

bool tftUiWriteCmd(uint8_t cmd, const uint8_t* data, size_t n) {
  if (!gActive) return false;
  hold(true);
  const bool ok = tftDriverWriteCmd(cmd, data, n);
  hold(false);
  return ok;
}

bool tftUiReadReg(uint8_t cmd, uint32_t& raw) {
  if (!gActive) return false;
  hold(true);
  raw = tftDriverReadReg(cmd);
  hold(false);
  return true;
}

void tftUiVerify(Print& out) {
  if (!gActive) { out.println("tft not running (try: tft on)"); return; }
  hold(true);
  if (gBusy) { hold(false); out.println("[verify] render task busy; test cancelled"); return; }
  LoopWatchdogPause pause;
  loggerMeta("tft_verify=start");
  const bool completed=tftDriverVerify(out,[]() { loggerService(); gpsService(); uiService(); delay(1); });
  gActive=tftDriverRunning();
  hold(false);
  gLastFrameMs=0;
  loggerMeta(completed?"tft_verify=completed":"tft_verify=inconclusive");
}

int tftUiUtcOffsetMinutes() { return gUtcOffsetMin; }

void tftUiSetUtcOffsetMinutes(int minutes) {
  gUtcOffsetMin = static_cast<int16_t>(minutes);
  if (gPrefsReady) gPrefs.putShort("utc", gUtcOffsetMin);
}

bool tftUiDemo(const char* screen, uint32_t seconds) {
  if (!strcmp(screen, "off")) { gDemoUntilMs = 0; return true; }
  gDemoClose = false;
  gDemoFacing = TFT_DEMO_TOWARD;
  if (!strcmp(screen, "listening")) gDemoScreen = TftScreen::LISTENING;
  else if (!strcmp(screen, "camera")) gDemoScreen = TftScreen::CAMERA;
  else if (!strcmp(screen, "close")) { gDemoScreen = TftScreen::CAMERA; gDemoClose = true; }
  else if (!strcmp(screen, "away")) { gDemoScreen = TftScreen::CAMERA; gDemoFacing = TFT_DEMO_AWAY; }
  else if (!strcmp(screen, "nodir")) { gDemoScreen = TftScreen::CAMERA; gDemoFacing = TFT_DEMO_NODIR; }
  else if (!strcmp(screen, "rf")) gDemoScreen = TftScreen::RF;
  else if (!strcmp(screen, "follower")) gDemoScreen = TftScreen::FOLLOWER;
  else if (!strcmp(screen, "boot")) gDemoScreen = TftScreen::BOOT;
  else return false;
  gDemoUntilMs = millis() + seconds * 1000UL;
  if (!gDemoUntilMs) gDemoUntilMs = 1;
  return true;
}

// ---- building the model from live state ---------------------------------------------------

static void fillStatus(TftModel& m, uint32_t now) {
  m.nowMs = now;
  const int64_t utc = gpsUtcNow();
  if (utc > 0) {
    const int64_t local = utc + static_cast<int64_t>(gUtcOffsetMin) * 60;
    const int64_t secOfDay = ((local % 86400) + 86400) % 86400;
    m.clockMinutes = static_cast<int16_t>(secOfDay / 60);
  } else {
    m.clockMinutes = -1;
  }
  m.ble = snifferBleEnabled();
  m.wifi = true;
  if (gpsSimActive()) m.gps = 3;
  else if (!gpsModuleTalking()) m.gps = 0;
  else if (!gpsUsable()) m.gps = 1;
  else { m.gps = 2; m.sats = gpsFix().sats; }
  m.muted = detectorMuted();
  m.carMode = loggerMode() == LogMode::CAR;
  m.alertCount = detectorAlertCount() + camAlertAnnouncedCount();
  {
    const CamAlertView& cv = camAlertView();
    const DetectView& dv = detectorView();
    uint32_t last = 0;
    if (cv.valid) { last = cv.atMs; m.lastAlertCam = true; m.lastAlertCode = cv.maker; }
    if (dv.valid && (!last || static_cast<int32_t>(dv.atMs - last) > 0)) {
      last = dv.atMs; m.lastAlertCam = false; m.lastAlertCode = dv.cat;
    }
    m.lastAlertAgoS = last ? static_cast<int32_t>((now - last) / 1000UL) : -1;
  }
  {
    const DetectSummary s = detectorSummary(5UL * 60UL * 1000UL);
    m.police.count = s.count; m.police.likely = s.likely; m.police.n = s.n;
    for (int k = 0; k < s.n; k++) {
      const DetectEntry& e = s.top[k];
      TftModel::PoliceEntry& p = m.police.top[k];
      p.cat = e.cat; p.tier = e.tier; p.ble = e.ble; p.vendorOnly = e.vendorOnly;
      p.rssi = e.rssi; p.trend = e.trend; p.agoS = e.agoS;
      strlcpy(p.vendor, e.vendor ? e.vendor : "", sizeof(p.vendor));
    }
  }
  {
    // Heading is tracked on every screen (not only the camera one) so the idle arrow can be heading-up.
    const GpsFix& g = gpsFix();
    if (gpsUsable() && g.courseValid && g.speedMps >= CAM_MIN_SPEED_MPS) { gLastHeading = g.courseDeg; gHeadingKnown = true; }
    const CamNearest& n = camAlertNearest();
    m.nearest.searched = n.valid;
    m.nearest.found = n.valid && n.found;
    m.nearest.distM = static_cast<uint16_t>(n.distM > 65000.0f ? 65000.0f : n.distM);
    m.nearest.headingKnown = gHeadingKnown;
    m.nearest.relDeg = gHeadingKnown ? n.bearingDeg - gLastHeading : n.bearingDeg;
    m.nearest.maker = n.maker;
  }
  m.mappedCameras = camAlertDb().count();
  const LoggerStatus& ls = loggerStatus();
  m.storageOk = ls.fsOk;
  m.storageWriteError = ls.writeError;
  m.storageLow = ls.full;
  m.freeMb = ls.freeBytes / 1048576.0f;
}

static void fillCamera(TftModel& m) {
  const CamAlertView& cv = camAlertView();
  const GpsFix& g = gpsFix();
  if (g.courseValid && g.speedMps >= CAM_MIN_SPEED_MPS) gLastHeading = g.courseDeg;
  m.cam.headingDeg = gLastHeading;
  m.cam.speedMph = gpsDisplayMph(g.speedMps);
  m.cam.maker = cv.maker;
  m.cam.distM = cv.distM;
  m.cam.close = cv.close;
  m.cam.facing = cv.facing;
  m.cam.dirKnown = cv.dirKnown;
  float rangeM = cv.distM * 1.25f + 60.0f;
  m.cam.rangeM = rangeM < 250.0f ? 250.0f : (rangeM > 1000.0f ? 1000.0f : rangeM);

  const double cosRef = cos(gRefLat * M_PI / 180.0);
  const double absE = g.lon * 111195.0 * cosRef, absN = g.lat * 111195.0;
  m.cam.gridE = static_cast<float>(fmod(fmod(absE, 1000.0) + 1000.0, 1000.0));
  m.cam.gridN = static_cast<float>(fmod(fmod(absN, 1000.0) + 1000.0, 1000.0));

  m.cam.camCount = mapCollectCameras(camAlertDb(), g.lat, g.lon, m.cam.rangeM * 1.4f, m.cam.cams, TFT_MAP_CAMS);
  // The camera being warned about must be on the map even if 24 closer ones crowd it out.
  mapEnu(g.lat, g.lon, cv.lat, cv.lon, m.cam.targetEast, m.cam.targetNorth);
  bool present = false;
  for (int i = 0; i < m.cam.camCount; i++) present |= m.cam.cams[i].idx == cv.idx;
  if (!present && camAlertDb().count() > cv.idx) {
    const CamRec r = camAlertDb().rec(cv.idx);
    const int slot = m.cam.camCount < TFT_MAP_CAMS ? m.cam.camCount++ : TFT_MAP_CAMS - 1;
    m.cam.cams[slot] = MapCam{m.cam.targetEast, m.cam.targetNorth, r.dir, r.maker, r.flags, cv.idx};
  }
  // Snap the target to its map entry so the screen finds it by position.
  for (int i = 0; i < m.cam.camCount; i++)
    if (m.cam.cams[i].idx == cv.idx) { m.cam.targetEast = m.cam.cams[i].east; m.cam.targetNorth = m.cam.cams[i].north; }

  const int n = gTrail.count() < Trail::N ? gTrail.count() : Trail::N;
  m.cam.trailCount = n;
  for (int i = 0; i < n; i++) {
    const Trail::Pt& p = gTrail.at(i);
    mapEnu(g.lat, g.lon, p.lat_e6 * 1e-6, p.lon_e6 * 1e-6, m.cam.trailEast[i], m.cam.trailNorth[i]);
  }
}

static void fillRf(TftModel& m) {
  const DetectView& dv = detectorView();
  m.rf.cat = dv.cat;
  m.rf.tier = dv.tier;
  m.rf.ble = dv.ble;
  m.rf.rssi = dv.rssi;
  m.rf.count = dv.count;
  strlcpy(m.rf.vendor, dv.vendor ? dv.vendor : "", sizeof(m.rf.vendor));
  m.rf.vendorOnly = dv.vendorOnly;
  m.rf.trend = dv.trend;
  const uint8_t heard = m.police.count;   // filled by fillStatus, which runs first
  m.rf.others = heard > 0 ? heard - 1 : 0;
}

static void updateTrail(uint32_t now) {
  if (now - gLastTrailMs < 1000) return;
  gLastTrailMs = now;
  if (!gpsUsable()) return;
  const GpsFix& g = gpsFix();
  if (!gHaveRef || fabs(g.lat - gRefLat) > 0.5) { gRefLat = g.lat; gHaveRef = true; }
  gTrail.add(g.lat, g.lon);
}

void tftUiService() {
  if (!gActive) return;
  startTask();                           // no-op after the first call
  const uint32_t now = millis();
  updateTrail(now);
  if (gFrozen) return;                   // "tft freeze": diagnostics, no redraws (radio keeps running)
  if (gRequest || gBusy) return;         // the task has not finished the last frame: skip, never queue up

  const bool demo = gDemoUntilMs && static_cast<int32_t>(gDemoUntilMs - now) > 0;
  if (gDemoUntilMs && !demo) gDemoUntilMs = 0;

  const CamAlertView& cv = camAlertView();
  const DetectView& dv = detectorView();
  TftScreen screen = TftScreen::LISTENING;
  if (demo) screen = gDemoScreen;
  else if (cv.valid && now - cv.atMs < CAM_SHOW_MS) screen = TftScreen::CAMERA;
  else if (controlsMenuOpen()) screen = TftScreen::MENU;
  else if (dv.valid && now - dv.atMs < DETECT_SHOW_MS) screen = TftScreen::RF;

  const uint32_t interval = (screen == TftScreen::CAMERA || controlsBusy()) ? TFT_FRAME_MS_ALERT : TFT_FRAME_MS_IDLE;
  if (now - gLastFrameMs < interval) return;
  gLastFrameMs = now;

  gModel = TftModel();
  if (demo) {
    tftDemoModel(gModel, screen, gDemoClose, gDemoFacing);
    gModel.nowMs = now;
  } else {
    fillStatus(gModel, now);
    gModel.screen = screen;
    if (screen == TftScreen::CAMERA) fillCamera(gModel);
    else if (screen == TftScreen::RF) fillRf(gModel);
    controlsFillModel(gModel, now);
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  gShared = gModel;
  gRequest = true;
  xSemaphoreGive(gMutex);
}

// ---- diagnostics ----------------------------------------------------------------------------

void tftUiPrint(Print& out) {
  out.printf("tft: mode=%s running=%s", modeName(gMode), gActive ? "yes" : "no");
  if (gActive) {
    out.printf(" probe=%s id=0x%06lX", tftDriverProbe() == TftProbe::FOUND ? "found" : "forced",
               static_cast<unsigned long>(tftDriverId()));
  }
  out.println();
  const TftPanelCfg c = tftDriverConfig();
  out.printf("  panel: chip %s, rot %u, bgr %u, inv %u, spi %lu MHz (change: tft chip ili|st, rot 1|3, bgr 0|1, inv 0|1, mhz N, panel reset)\n",
             c.chip == TFT_CHIP_ST7789 ? "st7789" : "ili9341", c.rotation, c.bgr ? 1 : 0, c.invert ? 1 : 0,
             static_cast<unsigned long>(c.hz / 1000000UL));
  uint32_t d3 = 0, r04 = 0;
  tftDriverRawIds(d3, r04);
  out.printf("  id reads: D3=%08lX 04=%08lX (FFFFFFFF = nothing on SDO), spi errors %lu (last %d)\n",
             static_cast<unsigned long>(d3), static_cast<unsigned long>(r04),
             static_cast<unsigned long>(tftDriverSpiErrors()), tftDriverSpiLastError());
  if (gActive) {
    out.printf("  frames %lu, last frame %lu ms (max %lu), slowest strip %lu us, brightness %u%%, utc offset %d min\n",
               static_cast<unsigned long>(tftFramesPushed()), static_cast<unsigned long>(gFrameMs),
               static_cast<unsigned long>(gFrameMaxMs), static_cast<unsigned long>(gStripWorstUs), gBrightness, gUtcOffsetMin);
  } else {
    out.printf("  pins: SCK %d MOSI %d MISO %d CS %d DC %d RST %d BL %d (include/pins.h); utc offset %d min\n", PIN_TFT_SCK,
               PIN_TFT_MOSI, PIN_TFT_MISO, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BL, gUtcOffsetMin);
  }
}

static void b64(Print& out, const uint8_t* d, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char line[97];
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
    line[o++] = t[(v >> 18) & 63];
    line[o++] = t[(v >> 12) & 63];
    line[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
    line[o++] = i + 2 < n ? t[v & 63] : '=';
    if (o >= 96) { line[o] = 0; out.println(line); o = 0; loopWatchdogFeed(); }
  }
  if (o) { line[o] = 0; out.println(line); }
}

void tftUiDump(Print& out) {
  if (!gActive) { out.println("tft not running (try: tft on)"); return; }
  // Redraw what the loop last built into a temporary full-size buffer (PSRAM) and print that.
  uint16_t* full = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(TFT_W) * TFT_H * 2, MALLOC_CAP_SPIRAM));
  if (!full) { out.println("no memory for the dump"); return; }
  gfx::Canvas canvas(full, TFT_W, TFT_H);
  tftRender(canvas, gModel);
  out.printf("TFTDUMP BEGIN %d %d rgb565le\n", TFT_W, TFT_H);
  b64(out, reinterpret_cast<const uint8_t*>(full), static_cast<size_t>(TFT_W) * TFT_H * 2);
  out.println("TFTDUMP END");
  heap_caps_free(full);
}

// Draws each demo screen a few times in the strip buffer without sending anything to the panel.
// This runs in the loop task (slow core), so the numbers are a worst case for the render task.
void tftUiBench(Print& out) {
  if (!gActive) { out.println("tft not running (try: tft on)"); return; }
  hold(true);
  static TftModel m;
  const struct { const char* name; TftScreen s; bool close; } cases[] = {
      {"boot", TftScreen::BOOT, false}, {"listening", TftScreen::LISTENING, false}, {"camera", TftScreen::CAMERA, false},
      {"camera close", TftScreen::CAMERA, true}, {"rf", TftScreen::RF, false}, {"follower", TftScreen::FOLLOWER, false}};
  for (const auto& c : cases) {
    m = TftModel();
    tftDemoModel(m, c.s, c.close);
    gStripWorstUs = 0;
    tftProfileBegin(micros);
    const uint32_t t0 = micros();
    static TftGeometryCache cache;
    for (int i = 0; i < 3; i++) {
      cache.reset();
      for (int st = 0; st < TFT_H / TFT_STRIP_ROWS; st++) drawStrip(m, st, false,&cache);
    }
    out.printf("  %-13s %6lu us per frame, slowest strip %lu us\n", c.name, static_cast<unsigned long>((micros() - t0) / 3),
               static_cast<unsigned long>(gStripWorstUs));
    if (c.s==TftScreen::CAMERA) {
      static const char* names[]={"grid","trail","markers","target","chevron","card/text/bar","  status","  rounded card","  text"};
      for(unsigned p=0;p<PROF_COUNT;p++) out.printf("    %-14s %lu us\n",names[p],static_cast<unsigned long>(tftProfileRead(p)/3));
    }
    tftProfileBegin(nullptr);
    loopWatchdogFeed();
  }
  gStripWorstUs = 0;
  hold(false);
}
