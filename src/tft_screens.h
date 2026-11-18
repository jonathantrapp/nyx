// tft_screens.h - the colour screens, drawn from a TftModel onto a Canvas. Portable (no Arduino).
#pragma once
#include "gfx.h"
#include "tft_model.h"

// Draws the whole 320x240 frame for m.screen. The canvas must be TFT_W x TFT_H.
void tftRender(gfx::Canvas& canvas, const TftModel& m);
// Bench-only accumulated microseconds; disabled during ordinary rendering.
enum { PROF_GRID, PROF_TRAIL, PROF_MARKERS, PROF_TARGET, PROF_CHEVRON, PROF_CARD, PROF_STATUS, PROF_ROUND, PROF_TEXT, PROF_COUNT };
void tftProfileBegin(uint32_t (*clock)());
uint32_t tftProfileRead(unsigned part);
struct TftGeometryCache {
  gfx::CardCoverage card;
  float xy[512], triangles[150];
  unsigned xyCount=0, triangleCount=0;
  void reset() { xyCount=triangleCount=0; }
};
void tftRenderPrepared(gfx::Canvas& canvas,const TftModel& m,TftGeometryCache& cache);

// "460 ft" / "0.4 mi" split into number and unit, for a US driver (same rounding as the OLED).
void tftFormatDistance(uint16_t meters, char* num, size_t numLen, char* unit, size_t unitLen);

// How many of the five signal bars light for an RSSI (fixed thresholds -90/-80/-70/-60/-50 dBm,
// the same as the OLED). A signal-strength hint, not a distance or a confidence.
int tftRssiBars(int rssi);
