// camalert.cpp - see camalert.h.
#include "camalert.h"
#include <LittleFS.h>
#include "camera_map.h"
#include "config.h"
#include "detector.h"
#include "gps.h"
#include "logger.h"
#include "ui.h"
#include "watchlist.h"
#include "loop_watchdog.h"

static const char* kCamFile = "/cameras.bin";
static const uint32_t MAX_FILE_BYTES = 6UL * 1024UL * 1024UL;   // PSRAM is 8 MB; this is ~500k cameras

static uint8_t* gBuf = nullptr;
static size_t gLen = 0;
static CamDb gDb;
static CamAlerter gAlerter;
static CamAlertView gView = {};
static uint32_t gLastTickMs = 0, gAnnounced = 0;

const char* camMakerName(uint8_t m) {
  switch (m) {
    case CAM_MAKER_FLOCK:    return "FLOCK";
    case CAM_MAKER_MOTOROLA: return "MOTOROLA";
    case CAM_MAKER_AXON:     return "AXON";
    default:                 return "ALPR";
  }
}

bool camAlertReload() {
  gDb = CamDb();
  free(gBuf); gBuf = nullptr; gLen = 0;
  gView = {};
  File f = LittleFS.open(kCamFile, "r");
  if (!f) return false;
  const size_t size = f.size();
  if (size < 20 || size > MAX_FILE_BYTES) { f.close(); return false; }
  gBuf = static_cast<uint8_t*>(psramFound() ? ps_malloc(size) : malloc(size));
  if (!gBuf) { f.close(); return false; }
  size_t got = 0;
  while (got < size) {
    const int n = f.read(gBuf + got, min<size_t>(4096, size - got));
    if (n <= 0) break;
    got += n;
    loopWatchdogFeed();
  }
  f.close();
  if (got != size || !gDb.attach(gBuf, size)) { free(gBuf); gBuf = nullptr; gDb = CamDb(); return false; }
  gLen = size;
  return true;
}

void camAlertBegin() {
  if (camAlertReload()) {
    Serial.printf("[cam] %lu mapped cameras loaded (OSM snapshot %lu)\n", static_cast<unsigned long>(gDb.count()),
                  static_cast<unsigned long>(gDb.dataDate()));
    char note[40]; snprintf(note, sizeof(note), "cam_map=%lu date=%lu", static_cast<unsigned long>(gDb.count()),
                            static_cast<unsigned long>(gDb.dataDate()));
    loggerMetaDeferred(note);
  } else {
    Serial.println("[cam] no camera map on flash: camera alerts off. Upload one with tools/push_cameras.py");
    loggerMetaDeferred("cam_map=none");
  }
}

bool camAlertLoaded() { return gDb.count() > 0; }
const CamDb& camAlertDb() { return gDb; }
uint32_t camAlertAnnouncedCount() { return gAnnounced; }
const CamAlertView& camAlertView() { return gView; }

static CamNearest gNearest = {};
const CamNearest& camAlertNearest() { return gNearest; }

// Nearest camera within 5 miles. The file is sorted by latitude, so only a band ~16 km tall is read
// (a few hundred to a few thousand records), and anything outside the longitude window is skipped
// before any square root. Runs once a second.
static void updateNearest(const GpsFix& g) {
  const double latPerM = 1.0 / 111195.0;
  const double lonPerM = latPerM / max(0.01, cos(g.lat * M_PI / 180.0));
  const double R = CAM_NEAREST_MAX_M;
  const int32_t lo = static_cast<int32_t>((g.lat - R * latPerM) * 1e6), hi = static_cast<int32_t>((g.lat + R * latPerM) * 1e6);
  const int32_t lonLo = static_cast<int32_t>((g.lon - R * lonPerM) * 1e6), lonHi = static_cast<int32_t>((g.lon + R * lonPerM) * 1e6);
  CamNearest n = {};
  n.valid = true;
  n.distM = static_cast<float>(R);
  for (uint32_t i = gDb.lowerBoundLat(lo); i < gDb.count(); i++) {
    const CamRec r = gDb.rec(i);
    if (r.lat_e6 > hi) break;
    if (r.lon_e6 < lonLo || r.lon_e6 > lonHi) continue;
    const double north = (r.lat_e6 * 1e-6 - g.lat) / latPerM, east = (r.lon_e6 * 1e-6 - g.lon) / lonPerM;
    const float d = static_cast<float>(sqrt(north * north + east * east));
    if (d >= n.distM) continue;
    float brg = static_cast<float>(atan2(east, north) * 180.0 / M_PI);
    if (brg < 0) brg += 360.0f;
    n.found = true; n.distM = d; n.bearingDeg = brg; n.maker = r.maker;
  }
  gNearest = n;
}

void camAlertService() {
  const uint32_t now = millis();
  if (now - gLastTickMs < 1000) return;
  gLastTickMs = now;
  if (!camAlertLoaded() || !gpsUsable()) { gNearest.valid = false; return; }

  const GpsFix& g = gpsFix();
  updateNearest(g);
  CamFix fix{g.lat, g.lon, g.speedMps, g.courseDeg, g.courseValid};
  CamHit hit; CamStage stage; bool announce;
  if (!gAlerter.update(gDb, fix, now, hit, stage, announce)) return;

  gView.valid = true; gView.atMs = now; gView.maker = hit.maker; gView.facing = hit.facing; gView.dirKnown = hit.dirKnown;
  gView.idx = hit.idx; gView.lat = hit.lat; gView.lon = hit.lon;
  gView.distM = hit.distM > 65000.0f ? 65000 : static_cast<uint16_t>(hit.distM);
  gView.close = stage == CAM_STAGE_CLOSE;

  if (!announce) return;
  gAnnounced++;
  char text[136];   // CaptureRow::extra
  snprintf(text, sizeof(text), "event=camera %s maker=%s dist_m=%u facing=%d dir_known=%d cam_lat=%.6f cam_lon=%.6f",
           stage == CAM_STAGE_CLOSE ? "close" : "ahead", camMakerName(hit.maker), gView.distM, hit.facing ? 1 : 0,
           hit.dirKnown ? 1 : 0, hit.lat, hit.lon);
  loggerGps(text);
  Serial.printf("[cam] %s %s camera %u m %s\n", stage == CAM_STAGE_CLOSE ? "CLOSE" : "ahead", camMakerName(hit.maker),
                gView.distM, !hit.dirKnown ? "(direction unknown)" : hit.facing ? "(lens toward you)" : "(lens away)");
  if (!detectorMuted()) uiBeepCategory(WL_CAT_CAMERA);
}

void camAlertPrint(Print& out) {
  if (!camAlertLoaded()) { out.println("camera map: none on flash (/cameras.bin)"); return; }
  out.printf("camera map: %lu cameras, OSM snapshot %lu, %lu KB in RAM, %lu announced this boot\n",
             static_cast<unsigned long>(gDb.count()), static_cast<unsigned long>(gDb.dataDate()),
             static_cast<unsigned long>(gLen / 1024), static_cast<unsigned long>(gAnnounced));
  if (gView.valid) out.printf("ahead now: %s camera %u m%s\n", camMakerName(gView.maker), gView.distM, gView.close ? " (close)" : "");
}

void camAlertNearby(Print& out) {
  if (!camAlertLoaded()) { out.println("no camera map"); return; }
  if (!gpsUsable()) { out.println("no position (no GPS fix, and no 'gps sim')"); return; }
  const GpsFix& g = gpsFix();
  const double latPerM = 1.0 / 111195.0;
  const double lonPerM = latPerM / max(0.01, cos(g.lat * M_PI / 180.0));
  struct Near { float d; float brg; CamRec r; } best[5]; int n = 0;
  const int32_t lo = static_cast<int32_t>((g.lat - 5000 * latPerM) * 1e6), hi = static_cast<int32_t>((g.lat + 5000 * latPerM) * 1e6);
  for (uint32_t i = gDb.lowerBoundLat(lo); i < gDb.count(); i++) {
    const CamRec r = gDb.rec(i);
    if (r.lat_e6 > hi) break;
    const double north = (r.lat_e6 * 1e-6 - g.lat) / latPerM, east = (r.lon_e6 * 1e-6 - g.lon) / lonPerM;
    const float d = static_cast<float>(sqrt(north * north + east * east));
    if (d > 5000.0f) continue;
    float brg = static_cast<float>(atan2(east, north) * 180.0 / M_PI); if (brg < 0) brg += 360.0f;
    int pos;
    if (n < 5) pos = n++;
    else if (d < best[4].d) pos = 4;
    else continue;
    best[pos] = {d, brg, r};
    for (int k = pos; k > 0 && best[k].d < best[k - 1].d; k--) { Near t = best[k]; best[k] = best[k - 1]; best[k - 1] = t; }
  }
  if (!n) { out.println("no mapped cameras within 5 km"); return; }
  for (int i = 0; i < n; i++)
    out.printf("  %5.0f m  bearing %3.0f  %s  facing %s\n", best[i].d, best[i].brg, camMakerName(best[i].r.maker),
               best[i].r.dir == CAM_DIR_UNKNOWN ? "?" : String(best[i].r.dir).c_str());
}
