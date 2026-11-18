// tft_screens.cpp - see tft_screens.h. Look: near-black, amber accents, red for mapped cameras.
// Same palette as the Nyx video so the product matches what was shown.
#include "tft_screens.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gfx_fonts.h"
#include "watchlist.h"

using namespace gfx;
using namespace gfx::fonts;

namespace {

constexpr Color UIBG  = rgb(9, 11, 15);
constexpr Color AMBER = rgb(232, 168, 72);
constexpr Color WARM  = rgb(238, 232, 222);
constexpr Color MUTED = rgb(122, 124, 132);
constexpr Color LIGHT = rgb(198, 198, 206);   // secondary text that must still read in daylight
constexpr Color DIM   = rgb(84, 86, 96);
constexpr Color HAIR  = rgb(52, 54, 62);
constexpr Color RED   = rgb(236, 84, 64);
constexpr Color GRID  = rgb(22, 25, 31);

constexpr int BAR_H = 29;   // status bar height; content starts below it
constexpr int CARD_Y = 33, CARD_H = 100;  // alert screens: dark card under the status bar (as in the Nyx video)
constexpr int MAP_TOP = CARD_Y + CARD_H; // camera screen: the small map fills from here to CAM_STRIP_Y
constexpr int CAM_STRIP_Y = 214;   // camera screen: bottom strip for speed / MUTED / pop-ups; the map stays above it



void statusBar(Canvas& c, const TftModel& m, const char* label) {
  c.fillRect(0, 0, TFT_W, BAR_H, UIBG);
  const int nyxEnd = c.text(kBodyBold, 10, 20, "NYX", WARM, 3);
  // Heartbeat: blinks once a second, so "running" and "frozen" never look the same.
  c.discAA(static_cast<float>(nyxEnd + 8), 14.0f, 4.0f, AMBER, (m.nowMs / 1000) % 2 == 0 ? 255 : 70);
  if (label && label[0]) c.text(kSmall, 54, 19, label, MUTED, 2);

  // Right side, right to left. Bold and light: this bar is on most screens, read at arm's length.
  int x = TFT_W - 8;
  auto put = [&](const char* s, Color col) {
    const int w = textWidth(kBodyBold, s, 1);
    x -= w;
    c.text(kBodyBold, x, 20, s, col, 1);
    x -= 12;
  };
  char gps[12];
  Color gpsColor = RED;
  switch (m.gps) {
    case 2: snprintf(gps, sizeof(gps), "GPS %u", m.sats); gpsColor = AMBER; break;
    case 3: snprintf(gps, sizeof(gps), "GPS SIM"); gpsColor = AMBER; break;
    case 1: snprintf(gps, sizeof(gps), "GPS --"); gpsColor = LIGHT; break;
    default: snprintf(gps, sizeof(gps), "NO GPS"); break;
  }
  put(gps, gpsColor);
  if (m.ble) put("BLE", LIGHT);
  if (m.muted) put("MUTE", AMBER);
  if (m.clockMinutes >= 0) {
    char t[8];
    snprintf(t, sizeof(t), "%02d:%02d", (m.clockMinutes / 60) % 24, m.clockMinutes % 60);
    put(t, WARM);
  }
  c.hline(0, BAR_H - 1, TFT_W, HAIR);
}

void demoChip(Canvas& c) {   // in the empty middle of the status bar
  c.roundRectAA(84, 6, 42, 17, 4, false, 0, 0, 1.0f, AMBER, 150);
  c.textAligned(kTiny, Align::CENTER, 105, 18, "DEMO", AMBER, 2, 200);
}

// ---- boot ----

void drawBoot(Canvas& c, const TftModel& m) {
  c.fill(UIBG);
  c.textAligned(kHugeBold, Align::CENTER, 160, 118, "NYX", WARM, 14);
  c.fillRect(130, 134, 60, 2, AMBER);
  c.textAligned(kSmall, Align::CENTER, 160, 162, m.bootNote, MUTED, 2);
}

// ---- listening ----

// Flat panel with small anti-aliased corners: looks rounded, costs a fraction of a full AA shape
// (the idle screen is up most of the time).
// Outline-only panel on the black background. A filled dark-grey box shimmered visibly on unit A's
// panel (supply ripple from the radio on the 3V3 rail shows most on flat dark greys, 2026-11-14); black
// and a thin line do not. Square corners, flat lines: cheap to draw.
void panel(Canvas& c, int x, int y, int w, int h, Color line) {
  c.frameRect(x, y, w, h, line);
}

const char* categoryTitle(uint8_t cat);   // radio section, below

const char* makerTitle(uint8_t maker) {
  switch (maker) {
    case CAM_MAKER_FLOCK: return "Flock";
    case CAM_MAKER_MOTOROLA: return "Motorola";
    case CAM_MAKER_AXON: return "Axon";
    default: return "Plate reader";
  }
}

// "ahead right" etc. for a direction relative to travel (degrees, any range).
const char* sideWords(float rel) {
  while (rel > 180.0f) rel -= 360.0f;
  while (rel < -180.0f) rel += 360.0f;
  const float a = fabsf(rel);
  const bool right = rel > 0;
  if (a <= 22.5f) return "ahead";
  if (a <= 67.5f) return right ? "ahead right" : "ahead left";
  if (a <= 112.5f) return right ? "to the right" : "to the left";
  if (a <= 157.5f) return right ? "behind right" : "behind left";
  return "behind";
}

// Big signal bars (fixed RSSI steps, a hint of closeness, not a distance). x,y = bottom-left.
void signalBars(Canvas& c, int x, int y, int bars, Color on, Color off, int unit) {
  for (int i = 0; i < 5; i++) {
    const int h = unit * (2 + i * 2);
    c.fillRect(x + i * (unit * 2 + 2), y - h, unit * 2, h, i < bars ? on : off);
  }
}

// Radar-detector style trend: is the signal getting stronger (closer) or weaker over the last ~4 s.
// Shape carries it (filled up-triangle / down-triangle / bar), the word says it too.
constexpr int TREND_DB = 3;                   // smaller changes are noise
int trendDir(int8_t trend, uint32_t agoS) {
  if (agoS > 15) return -2;                   // not heard for a while: fading out
  return trend >= TREND_DB ? 1 : (trend <= -TREND_DB ? -1 : 0);
}
const char* trendWord(int dir) {
  return dir == 1 ? "closer" : (dir == -1 ? "farther" : (dir == -2 ? "fading" : "steady"));
}
void trendGlyph(Canvas& c, float x, float y, int dir, Color up, Color other, float size) {
  if (dir == 1) {
    const float t[6] = {x, y - size, x + size, y + size * 0.7f, x - size, y + size * 0.7f};
    c.polyAA(t, 3, up);
  } else if (dir == -1 || dir == -2) {
    const float t[6] = {x - size, y - size * 0.7f, x + size, y - size * 0.7f, x, y + size};
    c.polyAA(t, 3, other);
  } else {
    c.fillRect(static_cast<int>(x - size), static_cast<int>(y - 2), static_cast<int>(2 * size), 4, other);
  }
}

// One line of the "everything nearby" list: kind, signal bars, trend arrow + word.
void policeRow(Canvas& c, int x, int y, const TftModel::PoliceEntry& e, bool withVendor) {
  const bool likely = e.tier >= WL_LIKELY && !e.vendorOnly;
  const Color hot = likely ? RED : AMBER;
  char what[40];
  if (withVendor && e.vendor[0]) {
    char v[20];
    snprintf(v, sizeof(v), "%s", e.vendor);
    char* sp = strchr(v, ' ');
    if (sp) *sp = 0;                          // first word of the maker is enough here
    snprintf(what, sizeof(what), "%s (%s)", categoryTitle(e.cat), v);
  } else {
    snprintf(what, sizeof(what), "%s", categoryTitle(e.cat));
  }
  c.text(kLabelBold, x, y, what, likely ? WARM : LIGHT, 0);
  const int dir = trendDir(e.trend, e.agoS);
  signalBars(c, 206, y + 1, dir == -2 ? 0 : tftRssiBars(e.rssi), hot, rgb(48, 50, 58), 2);
  trendGlyph(c, 250.0f, y - 6.0f, dir, hot, LIGHT, 6.0f);
  c.text(kBody, 262, y, trendWord(dir), dir == 1 ? hot : LIGHT, 0);
}

// Idle (most of the time). Police gear first (the point of the device), the nearest mapped camera second,
// then the last alert. Status shows up only when something is wrong; the status bar has the rest.
void drawListening(Canvas& c, const TftModel& m) {
  c.fill(UIBG);
  statusBar(c, m, "");

  // Police gear heard in the last 5 minutes, radar-detector style: a big count, then the list.
  constexpr int PY = 36, PH = 136;
  panel(c, 10, PY, 300, PH, rgb(70, 74, 86));
  c.text(kBodyBold, 22, PY + 22, "POLICE GEAR", AMBER, 2);
  c.textAligned(kBody, Align::RIGHT, 298, PY + 22, "last 5 min", LIGHT, 0);
  if (m.police.count == 0) {
    c.text(kHugeBold, 22, PY + 78, "NONE NEARBY", WARM, 1);
    c.text(kLabel, 22, PY + 108, "nothing heard", LIGHT, 0);
  } else {
    char n[8];
    snprintf(n, sizeof(n), "%u", m.police.count > 99 ? 99 : m.police.count);
    c.textAligned(kDigitsBig, Align::CENTER, 46, PY + 88, n, m.police.likely ? RED : AMBER, 0);
    c.textAligned(kBodyBold, Align::CENTER, 46, PY + 110, "NEARBY", LIGHT, 1);
    const int rows = m.police.n < 4 ? m.police.n : 4;
    for (int k = 0; k < rows; k++) policeRow(c, 98, PY + 48 + k * 24, m.police.top[k], false);
  }

  // Nearest mapped camera: one line with a small arrow. Problems that switch camera alerts off win.
  constexpr int CY = 200;
  const bool gpsOk = m.gps == 2 || m.gps == 3;
  char cam[56];
  Color camColor = WARM;
  bool arrow = false;
  if (!m.mappedCameras) { snprintf(cam, sizeof(cam), "No camera map: camera alerts off"); camColor = AMBER; }
  else if (m.gps == 0) { snprintf(cam, sizeof(cam), "No GPS module: camera alerts off"); camColor = RED; }
  else if (!gpsOk) { snprintf(cam, sizeof(cam), "No GPS fix: camera alerts off"); camColor = AMBER; }
  else if (!m.nearest.searched) { snprintf(cam, sizeof(cam), "Camera: looking..."); camColor = LIGHT; }
  else if (!m.nearest.found) { snprintf(cam, sizeof(cam), "No cameras within 5 mi"); camColor = LIGHT; }
  else {
    char num[12], unit[6];
    tftFormatDistance(m.nearest.distM, num, sizeof(num), unit, sizeof(unit));
    if (m.nearest.headingKnown) snprintf(cam, sizeof(cam), "Camera %s %s, %s", num, unit, sideWords(m.nearest.relDeg));
    else snprintf(cam, sizeof(cam), "Camera %s %s away", num, unit);
    arrow = true;
  }
  int camX = 12;
  if (arrow) {
    const float ax = 22.0f, ay = CY - 7.0f;
    const float rr = m.nearest.relDeg * static_cast<float>(M_PI / 180.0), cs = cosf(rr), sn = sinf(rr);
    const float shape[8] = {0, -10, 6, 6, 0, 3, -6, 6};
    float pts[8];
    for (int k = 0; k < 4; k++) {
      pts[2 * k] = ax + shape[2 * k] * cs - shape[2 * k + 1] * sn;
      pts[2 * k + 1] = ay + shape[2 * k] * sn + shape[2 * k + 1] * cs;
    }
    c.polyAA(pts, 4, RED);
    camX = 38;
  }
  c.text(kLabel, camX, CY, cam, camColor, 0);

  // Bottom line: storage trouble if any, otherwise the last alert by name.
  char last[56];
  Color lastColor = LIGHT;
  if (!m.storageOk) { snprintf(last, sizeof(last), "NO STORAGE: not logging"); lastColor = RED; }
  else if (m.storageWriteError) { snprintf(last, sizeof(last), "LOG WRITE ERROR"); lastColor = RED; }
  else if (m.storageLow) { snprintf(last, sizeof(last), "Log space low"); lastColor = AMBER; }
  else if (m.lastAlertAgoS < 0) snprintf(last, sizeof(last), "No alerts yet");
  else {
    const char* what = m.lastAlertCam ? makerTitle(m.lastAlertCode) : categoryTitle(m.lastAlertCode);
    char ago[24];
    if (m.lastAlertAgoS < 60) snprintf(ago, sizeof(ago), "just now");
    else if (m.lastAlertAgoS < 3600) snprintf(ago, sizeof(ago), "%ld min ago", static_cast<long>(m.lastAlertAgoS / 60));
    else snprintf(ago, sizeof(ago), "%ld hr ago", static_cast<long>(m.lastAlertAgoS / 3600));
    snprintf(last, sizeof(last), "Last alert: %s%s, %s", what, m.lastAlertCam ? " camera" : "", ago);
  }
  c.text(kLabel, 12, 228, last, lastColor, 0);
  if (m.demo) demoChip(c);
}

// ---- camera ----

static uint32_t (*gProfileClock)()=nullptr;
static uint32_t gProfile[PROF_COUNT]={};
static uint32_t profileNow() { return gProfileClock?gProfileClock():0; }
static void profileMark(unsigned part,uint32_t& start) { if(gProfileClock){const uint32_t now=gProfileClock();gProfile[part]+=now-start;start=now;} }

void drawCameraMap(Canvas& c, const TftModel& m, float ox, float oy, float s,TftGeometryCache* cache) {
  uint32_t stamp=profileNow();
  unsigned xyIndex=0,triIndex=0;
  const float heading = m.cam.headingDeg;
  auto toScreen = [&](float east, float north, float& sx, float& sy) {
    if(cache && xyIndex+2<=cache->xyCount) {sx=cache->xy[xyIndex++];sy=cache->xy[xyIndex++];return;}
    float right, ahead;
    mapHeadingUp(east, north, heading, right, ahead);
    sx = ox + right * s;
    sy = oy - ahead * s;
    if(cache && xyIndex+2<=512) {cache->xy[xyIndex++]=sx;cache->xy[xyIndex++]=sy;cache->xyCount=xyIndex;}
  };

  // Grid, anchored to the ground so it slides past as you drive.
  static const float steps[] = {25, 50, 100, 200, 500, 1000};
  float g = 1000;
  for (float st : steps) if (st * s >= 34.0f) { g = st; break; }
  const float reach = 280.0f / s;                      // metres from you to the farthest screen corner
  for (int axis = 0; axis < 2; axis++) {
    const float own = axis == 0 ? m.cam.gridE : m.cam.gridN;
    const int k0 = static_cast<int>(floorf((own - reach) / g)), k1 = static_cast<int>(ceilf((own + reach) / g));
    for (int k = k0; k <= k1; k++) {
      const float rel = k * g - own;
      float ax, ay, bx, by;
      if (axis == 0) { toScreen(rel, -reach, ax, ay); toScreen(rel, reach, bx, by); }
      else { toScreen(-reach, rel, ax, ay); toScreen(reach, rel, bx, by); }
      c.line(static_cast<int>(ax), static_cast<int>(ay), static_cast<int>(bx), static_cast<int>(by), GRID);
    }
  }

  profileMark(PROF_GRID,stamp);
  // Trail: bright and thick near you, fading with distance travelled (as in the Nyx video).
  if (m.cam.trailCount > 0) {
    float px = ox, py = oy;
    for (int i = 0; i < m.cam.trailCount; i++) {
      float sx, sy;
      toScreen(m.cam.trailEast[i], m.cam.trailNorth[i], sx, sy);
      const float f = static_cast<float>(i) / static_cast<float>(m.cam.trailCount);
      c.lineAA(px, py, sx, sy, 4.4f - 2.6f * f, blend(AMBER, rgb(56, 42, 24), static_cast<uint8_t>(f * 230.0f)));
      px = sx; py = sy;
    }
  }
  profileMark(PROF_TRAIL,stamp);
  // Mapped cameras. Lens direction is drawn as a faint wedge when OSM recorded one.
  auto wedge = [&](float sx, float sy, const MapCam& cam, uint8_t alpha) {
    if (cam.dir == CAM_DIR_UNKNOWN || !(cam.flags & CAM_FLAG_DIR) || (cam.flags & CAM_FLAG_MULTI)) return;
    if(cache && triIndex+6<=cache->triangleCount) { c.polyAA(cache->triangles+triIndex,3,RED,alpha);triIndex+=6;return; }
    const float rel = (static_cast<float>(cam.dir) - heading) * static_cast<float>(M_PI / 180.0);
    const float spread = 0.5f, len = 24.0f;
    const float tri[6] = {sx, sy,
                          sx + sinf(rel - spread) * len, sy - cosf(rel - spread) * len,
                          sx + sinf(rel + spread) * len, sy - cosf(rel + spread) * len};
    c.polyAA(tri, 3, RED, alpha);
    if(cache && triIndex+6<=150){memcpy(cache->triangles+triIndex,tri,sizeof(tri));triIndex+=6;cache->triangleCount=triIndex;}
  };
  const MapCam* target = nullptr;
  for (int i = 0; i < m.cam.camCount; i++) {
    const MapCam& cam = m.cam.cams[i];
    if (fabsf(cam.east - m.cam.targetEast) < 1.0f && fabsf(cam.north - m.cam.targetNorth) < 1.0f) { target = &cam; continue; }
    float sx, sy;
    toScreen(cam.east, cam.north, sx, sy);
    if (sx < -30 || sx > TFT_W + 30 || sy < MAP_TOP - 30 || sy > TFT_H + 30) continue;
    c.discAA(sx, sy, 4.2f, scale(RED, 150));
    c.ringAA(sx, sy, 7.0f, 1.2f, RED, 70);
  }
  profileMark(PROF_MARKERS,stamp);
  if (target) {
    float sx, sy;
    toScreen(target->east, target->north, sx, sy);
    wedge(sx, sy, *target, 70);
    c.ringAA(sx, sy, 17.0f, 1.6f, RED, 150);
    c.ringAA(sx, sy, 25.0f, 1.2f, RED, 60);
    c.discAA(sx, sy, 10.5f, RED);
    c.fillRect(static_cast<int>(sx) - 6, static_cast<int>(sy) - 4, 8, 8, UIBG);          // camera body
    const float lens[8] = {sx + 2.0f, sy - 2.0f, sx + 7.0f, sy - 5.0f, sx + 7.0f, sy + 5.0f, sx + 2.0f, sy + 2.0f};
    c.polyAA(lens, 4, UIBG);
  }

  profileMark(PROF_TARGET,stamp);
  // You: a glow and a chevron pointing up (the map is heading-up).
  c.discAA(ox, oy, 19.0f, AMBER, 34);
  const float chevron[8] = {ox, oy - 14.0f, ox + 10.5f, oy + 11.0f, ox, oy + 5.0f, ox - 10.5f, oy + 11.0f};
  c.polyAA(chevron, 4, WARM);
  profileMark(PROF_CHEVRON,stamp);
}

// Right panel of the band: does the camera see you. Carried by SHAPE as well as a word, so it still
// reads in sun glare or for a colour-blind driver. Three states, never a guess: an open eye (lens points
// at you), the same eye struck through (lens points away), a "?" disc (direction not mapped).
constexpr int PANEL_X = 192;                 // divider between the distance and the right panel
constexpr float EYE_X = 254.0f, EYE_Y = CARD_Y + 40.0f;
constexpr int CARD_BASE = CARD_Y + 91;       // baseline of the card's bottom line

// The alert card (video look): dark tint, thick coloured border, divider before the right panel. Flat
// fills only, so it costs almost nothing to draw.
void alertCard(Canvas& c, Color tint, Color accent, int divX = PANEL_X) {
  constexpr int x = 4, w = TFT_W - 8, b = 3;
  c.fillRect(x, CARD_Y, w, CARD_H, tint);
  c.fillRect(x, CARD_Y, w, b, accent);
  c.fillRect(x, CARD_Y + CARD_H - b, w, b, accent);
  c.fillRect(x, CARD_Y, b, CARD_H, accent);
  c.fillRect(x + w - b, CARD_Y, b, CARD_H, accent);
  c.vline(divX, CARD_Y + 12, CARD_H - 24, accent, 110);
}

void alertDemoChip(Canvas& c, int y = CAM_STRIP_Y - 22) {
  c.roundRectAA(270, static_cast<float>(y), 42, 17, 4, false, 0, 0, 1.0f, AMBER, 150);
  c.textAligned(kTiny, Align::CENTER, 291, y + 12, "DEMO", AMBER, 2, 200);
}

void eyeIcon(Canvas& c, Color ink, Color bg) {
  float pts[24];                                // 12-point almond: polyAA's limit
  for (int k = 0; k < 12; k++) {
    const float t = k * static_cast<float>(M_PI / 6.0);
    pts[2 * k] = EYE_X + 40.0f * cosf(t);
    pts[2 * k + 1] = EYE_Y - 23.0f * sinf(t);
  }
  c.polyAA(pts, 12, ink);
  c.discAA(EYE_X, EYE_Y, 15.0f, bg);            // iris
  c.discAA(EYE_X, EYE_Y, 7.5f, ink);            // pupil
}

// Right panel: does the camera see you. Carried by SHAPE as well as a word, so it still reads in sun
// glare or for a colour-blind driver. Never a guess: open eye (lens toward you), struck-through eye
// (away), "?" disc (direction not mapped).
void facingIcon(Canvas& c, const TftModel& m, Color ink, Color bg) {
  const int tx = static_cast<int>(EYE_X);
  if (!m.cam.dirKnown) {
    c.discAA(EYE_X, EYE_Y, 25.0f, ink);
    c.textAligned(kHugeBold, Align::CENTER, tx + 1, static_cast<int>(EYE_Y) + 11, "?", bg, 0);
    c.textAligned(kBodyBold, Align::CENTER, tx, CARD_BASE, "UNKNOWN", ink, 1);
  } else if (m.cam.facing) {
    eyeIcon(c, ink, bg);
    c.textAligned(kBodyBold, Align::CENTER, tx, CARD_BASE, "FACES YOU", ink, 1);
  } else {
    eyeIcon(c, ink, bg);
    c.lineAA(EYE_X - 34.0f, EYE_Y - 30.0f, EYE_X + 34.0f, EYE_Y + 30.0f, 10.0f, bg);   // gap around the slash
    c.lineAA(EYE_X - 34.0f, EYE_Y - 30.0f, EYE_X + 34.0f, EYE_Y + 30.0f, 4.5f, ink);
    c.textAligned(kBodyBold, Align::CENTER, tx, CARD_BASE, "AWAY", ink, 1);
  }
}

// Camera warning, in the video's layout: status bar, a dark card with a red/amber border that answers
// "how far, does it see me" in big type, and the heading-up map with your trail underneath.
void drawCamera(Canvas& c, const TftModel& m,TftGeometryCache* cache) {
  c.fill(UIBG);
  const float ox = 160.0f, oy = 196.0f;                 // arrow sits above the bottom strip
  const float range = m.cam.rangeM < 120.0f ? 120.0f : m.cam.rangeM;
  const float s = (oy - MAP_TOP - 12.0f) / range;       // pixels per metre

  c.setClip(0, MAP_TOP, TFT_W, CAM_STRIP_Y);            // the map never reaches the card or the bottom strip
  drawCameraMap(c, m, ox, oy, s,cache);
  c.clearClip();
  c.hline(0, CAM_STRIP_Y, TFT_W, HAIR);
  uint32_t stamp=profileNow();
  const uint32_t cardStart=stamp;
  statusBar(c, m, "");
  profileMark(PROF_STATUS,stamp);

  const bool close = m.cam.close;
  const Color accent = close ? RED : AMBER;
  const Color tint = close ? rgb(64, 18, 14) : rgb(30, 22, 12);
  alertCard(c, tint, accent);
  profileMark(PROF_ROUND,stamp);

  c.text(kLabelBold, 16, CARD_Y + 21, close ? "CAMERA CLOSE" : "CAMERA AHEAD", accent, 2);
  char num[12], unit[6];
  tftFormatDistance(m.cam.distM, num, sizeof(num), unit, sizeof(unit));
  const int endX = c.text(kDigitsBig, 16, CARD_Y + 70, num, WARM, 1);
  c.text(kHugeBold, endX + 8, CARD_Y + 70, unit, WARM, 0);
  const char* maker = "PLATE READER";
  switch (m.cam.maker) {
    case CAM_MAKER_FLOCK: maker = "FLOCK"; break;
    case CAM_MAKER_MOTOROLA: maker = "MOTOROLA"; break;
    case CAM_MAKER_AXON: maker = "AXON"; break;
    default: break;
  }
  char who[32];
  snprintf(who, sizeof(who), "%s, MAPPED", maker);
  c.text(kBodyBold, 16, CARD_BASE, who, LIGHT, 1);
  facingIcon(c, m, WARM, tint);

  // Bottom strip: speed and muted (pop-ups use it too, so nothing covers the map).
  char spd[16];
  snprintf(spd, sizeof(spd), "%d MPH", static_cast<int>(m.cam.speedMph + 0.5f));
  c.textAligned(kLabelBold, Align::RIGHT, TFT_W - 10, CAM_STRIP_Y + 20, spd, WARM, 1);
  if (m.muted) c.text(kLabelBold, 10, CAM_STRIP_Y + 20, "MUTED", AMBER, 2);
  if (m.demo) alertDemoChip(c);
  profileMark(PROF_TEXT,stamp);
  if(gProfileClock)gProfile[PROF_CARD]+=stamp-cardStart;
}

// ---- radio detection ----

const char* categoryTitle(uint8_t cat) {
  static const char* const t[WL_CAT_COUNT] = {"Camera", "Acoustic", "Body cam", "Fleet router", "Gear", "Tactical", "Tracker"};
  return cat < WL_CAT_COUNT ? t[cat] : "RF lead";
}

void upper(char* s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = static_cast<char>(*s - 32); }

// Police gear alert, same card as the camera alert: what it is, who makes it, how sure, how strong.
// Red border = likely on more than a maker match; amber = possible. The tier words say it too.
constexpr int RF_DIV = 210;   // police card: wider left column for long kinds ("FLEET ROUTER")

void drawRf(Canvas& c, const TftModel& m) {
  c.fill(UIBG);
  statusBar(c, m, "");
  const bool likely = m.rf.tier >= WL_LIKELY && !m.rf.vendorOnly;
  const Color accent = likely ? RED : AMBER;
  const Color tint = likely ? rgb(64, 18, 14) : rgb(30, 22, 12);
  alertCard(c, tint, accent, RF_DIV);

  const char* tier = m.rf.tier >= WL_HIGH ? "CONFIRMED" : (likely ? "LIKELY" : "POSSIBLE");
  c.text(kLabelBold, 16, CARD_Y + 21, tier, accent, 2);
  char cat[24];
  snprintf(cat, sizeof(cat), "%s", categoryTitle(m.rf.cat));
  upper(cat);
  c.text(strlen(cat) <= 9 ? kHugeBold : kTitleBold, 16, CARD_Y + 62, cat, WARM, 0);
  // Maker name, cut at a word boundary if it would run into the right panel ("Sierra Wireless" -> "SIERRA").
  char vendor[24];
  snprintf(vendor, sizeof(vendor), "%s", m.rf.vendor[0] ? m.rf.vendor : "Unknown maker");
  upper(vendor);
  while (textWidth(kBodyBold, vendor, 1) > PANEL_X - 26) {
    char* sp = strrchr(vendor, ' ');
    if (!sp) { vendor[strlen(vendor) - 1] = 0; continue; }
    *sp = 0;
  }
  c.text(kBodyBold, 16, CARD_BASE, vendor, LIGHT, 1);

  const int bars = tftRssiBars(m.rf.rssi);
  signalBars(c, RF_DIV + 22, CARD_Y + 66, bars, accent, rgb(60, 52, 46), 5);
  const int dir = trendDir(m.rf.trend, 0);
  char tw[12];
  snprintf(tw, sizeof(tw), "%s", trendWord(dir));
  upper(tw);
  trendGlyph(c, RF_DIV + 14.0f, CARD_BASE - 6.0f, dir, accent, LIGHT, 6.0f);
  c.text(kBodyBold, RF_DIV + 26, CARD_BASE, tw, dir == 1 ? accent : WARM, 0);

  // Everything heard in the last 5 minutes, strongest first (the alerting device is usually row 1).
  const int rows = m.police.n < 3 ? m.police.n : 3;
  for (int k = 0; k < rows; k++) policeRow(c, 12, MAP_TOP + 24 + k * 24, m.police.top[k], true);
  if (m.police.count > 3) {
    char more[24];
    snprintf(more, sizeof(more), "+%u more", m.police.count - 3);
    c.text(kBodyBold, 104, CAM_STRIP_Y + 20, more, LIGHT, 0);
  }
  if (m.muted) c.text(kLabelBold, 10, CAM_STRIP_Y + 20, "MUTED", AMBER, 2);
  if (m.demo) alertDemoChip(c, CAM_STRIP_Y + 5);
}

// ---- follower ----

// Follower: blue border, so it is never mistaken for police gear or a camera. Big count of places.
void drawFollower(Canvas& c, const TftModel& m) {
  c.fill(UIBG);
  statusBar(c, m, "");
  const Color accent = rgb(96, 160, 236), tint = rgb(12, 20, 34);
  alertCard(c, tint, accent);
  c.text(kLabelBold, 16, CARD_Y + 21, "FOLLOWED?", accent, 2);
  c.text(kHugeBold, 16, CARD_Y + 62, m.follow.ble ? "SAME TAG" : "SAME DEVICE", WARM, 0);
  c.text(kBodyBold, 16, CARD_BASE, m.follow.ble ? "BLUETOOTH TAG" : "WI-FI DEVICE", LIGHT, 1);
  char n[8];
  snprintf(n, sizeof(n), "%u", m.follow.places);
  c.textAligned(kDigitsBig, Align::CENTER, 254, CARD_Y + 66, n, WARM, 0);
  c.textAligned(kBodyBold, Align::CENTER, 254, CARD_BASE, "PLACES", LIGHT, 1);

  char line1[48];
  snprintf(line1, sizeof(line1), "Seen at %u places over %u min", m.follow.places, m.follow.minutes);
  c.text(kLabel, 12, MAP_TOP + 24, line1, WARM, 0);
  c.text(kLabel, 12, MAP_TOP + 48, "Check your bag and car", LIGHT, 0);
  if (m.muted) c.text(kLabelBold, 10, CAM_STRIP_Y + 20, "MUTED", AMBER, 2);
  if (m.demo) alertDemoChip(c, CAM_STRIP_Y + 5);
}

// ---- settings menu and toast ----

void drawMenu(Canvas& c, const TftModel& m) {
  c.fill(UIBG);
  statusBar(c, m, "");
  c.text(kSmallBold, 10, 48, "SETTINGS", AMBER, 3);
  for (int i = 0; i < TFT_MENU_ROWS; i++) {
    if (!m.menu.label[i][0]) continue;
    const int y0 = 55 + i * 27;
    const bool sel = i == m.menu.sel;
    if (sel) c.roundRectAA(6, static_cast<float>(y0), 308, 25, 7, true, rgb(30, 23, 13), 255, m.menu.editing ? 2.0f : 1.2f, AMBER, m.menu.editing ? 255 : 150);
    c.text(kLabel, 18, y0 + 18, m.menu.label[i], sel ? WARM : MUTED, 0);
    if (m.menu.value[i][0]) {
      const Color vc = sel && m.menu.editing ? AMBER : (sel ? WARM : MUTED);
      const int vw = textWidth(kLabelBold, m.menu.value[i], 0);
      const int right = 300;
      c.text(kLabelBold, right - vw, y0 + 18, m.menu.value[i], vc, 0);
      if (sel && m.menu.editing) {                      // arrows: you are changing this value
        const float ay = y0 + 12.5f, lx = right - vw - 10.0f, rx = right + 3.0f;
        const float left[6] = {lx, ay, lx + 6, ay - 5, lx + 6, ay + 5};
        const float rightT[6] = {rx + 6, ay, rx, ay - 5, rx, ay + 5};
        c.polyAA(left, 3, AMBER);
        c.polyAA(rightT, 3, AMBER);
      }
    }
  }
  const char* hint = m.menu.editing ? "Turn to change. Press to save. Hold to cancel."
                                    : "Turn to move. Press to select. Hold to exit.";
  const int hw = textWidth(kSmall, hint, 0) > 300 ? 0 : textWidth(kSmall, hint, 0);
  if (hw) c.textAligned(kSmall, Align::CENTER, 160, 236, hint, DIM, 0);
  else c.textAligned(kTiny, Align::CENTER, 160, 236, hint, DIM, 0);
  if (m.demo) demoChip(c);
}

// Volume / muted pop-ups never cover content. They take over a strip reserved for them: the status bar
// on screens that have one (the clock and GPS hide for a moment), and the bottom strip of the camera
// screen, which the map keeps clear (see CAM_STRIP_Y).
void drawToast(Canvas& c, const TftModel& m) {
  if (!m.toast.active || m.screen == TftScreen::BOOT) return;
  const bool camera = m.screen == TftScreen::CAMERA || m.screen == TftScreen::RF || m.screen == TftScreen::FOLLOWER;
  const int y = camera ? CAM_STRIP_Y : 0, h = camera ? TFT_H - CAM_STRIP_Y : BAR_H;
  const int base = y + h / 2 + 7;                          // text baseline, centred in the strip
  c.fillRect(0, y, TFT_W, h, rgb(14, 16, 20));
  c.hline(0, camera ? y : y + h - 1, TFT_W, AMBER, 200);   // edge facing the content
  if (m.toast.pct < 0) {
    c.textAligned(kLabelBold, Align::CENTER, 160, base, m.toast.text, AMBER, 3);
    return;
  }
  const int pct = m.toast.pct > 100 ? 100 : m.toast.pct;
  c.text(kLabelBold, 10, base, m.toast.text, WARM, 2);
  char num[8];
  snprintf(num, sizeof(num), "%d", pct);
  c.textAligned(kLabelBold, Align::RIGHT, TFT_W - 10, base, num, AMBER, 0);
  const int bx = 112, bw = TFT_W - bx - 52, by = y + h / 2 - 5;
  c.roundRectAA(static_cast<float>(bx), static_cast<float>(by), static_cast<float>(bw), 10, 5, true, rgb(40, 34, 26), 255, 0, 0);
  const int fill = bw * pct / 100;
  if (fill > 1) c.roundRectAA(static_cast<float>(bx), static_cast<float>(by), static_cast<float>(fill), 10, 5, true, AMBER, 255, 0, 0);
}

}  // namespace

void tftFormatDistance(uint16_t meters, char* num, size_t numLen, char* unit, size_t unitLen) {
  const float ft = meters * 3.281f;
  if (ft < 1000.0f) {
    const int rounded = static_cast<int>((ft + 5.0f) / 10.0f) * 10;
    snprintf(num, numLen, "%d", rounded < 10 ? 10 : rounded);
    snprintf(unit, unitLen, "ft");
  } else {
    snprintf(num, numLen, "%.1f", meters / 1609.34f);
    snprintf(unit, unitLen, "mi");
  }
}

int tftRssiBars(int rssi) {
  static const int t[5] = {-90, -80, -70, -60, -50};
  int n = 0;
  for (int i = 0; i < 5; i++) if (rssi >= t[i]) n++;
  return n;
}

static void renderImpl(Canvas& canvas, const TftModel& m,TftGeometryCache* cache) {
  canvas.clearClip();
  switch (m.screen) {
    case TftScreen::BOOT: drawBoot(canvas, m); break;
    case TftScreen::LISTENING: drawListening(canvas, m); break;
    case TftScreen::CAMERA: drawCamera(canvas, m,cache); break;
    case TftScreen::RF: drawRf(canvas, m); break;
    case TftScreen::FOLLOWER: drawFollower(canvas, m); break;
    case TftScreen::MENU: drawMenu(canvas, m); break;
  }
  drawToast(canvas, m);
}
void tftRender(Canvas& canvas,const TftModel& m){renderImpl(canvas,m,nullptr);}
void tftRenderPrepared(Canvas& canvas,const TftModel& m,TftGeometryCache& cache){canvas.setCardCoverage(&cache.card);renderImpl(canvas,m,&cache);canvas.setCardCoverage(nullptr);}
void tftProfileBegin(uint32_t (*clock)()) { gProfileClock=clock; for(auto& v:gProfile)v=0; }
uint32_t tftProfileRead(unsigned part) { return part<PROF_COUNT?gProfile[part]:0; }
