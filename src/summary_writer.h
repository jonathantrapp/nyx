#pragma once
#include <Arduino.h>
#include "watchlist.h"

struct MinuteSummary {
  uint32_t startMs=0, durationMs=0;
  uint32_t heard[3]={}; // management, DATA, BLE: pre-rate-limit counters
  uint32_t rows[4]={}, unique[4]={}; // queued probe, beacon, DATA, BLE
  uint32_t appleRows=0, appleUnique=0, appleCollapsed=0;
  uint32_t trackerRows=0, trackerUnique=0, detections[WL_CAT_COUNT]={};
  uint32_t dropped=0, freeBytes=0;
  bool uniqueCapped=false, appleCacheCapped=false, partial=false;
};
void csvWriteSummary(Print& out, const MinuteSummary& s, uint32_t now);
