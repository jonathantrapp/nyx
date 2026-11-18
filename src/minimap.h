// minimap.h - the maths behind the camera screen's heading-up map: a trail of where you have
// been, and the mapped cameras around you, both as metres east/north of your position.
// Pure logic: no Arduino, no screen. Tested by tools/test_tft_native.py.
#pragma once
#include <stdint.h>
#include <math.h>
#include "camera_map.h"

// One mapped camera near you, as metres east/north of your position.
struct MapCam {
  float east, north;
  uint16_t dir;          // direction the lens faces, degrees clockwise from north; CAM_DIR_UNKNOWN if not mapped
  uint8_t maker;         // CamMaker
  uint8_t flags;         // CAM_FLAG_*
  uint32_t idx;          // index in the database
};

inline void mapEnu(double lat0, double lon0, double lat, double lon, float& east, float& north) {
  const double cosLat = cos(lat0 * M_PI / 180.0);
  north = static_cast<float>((lat - lat0) * 111195.0);
  east = static_cast<float>((lon - lon0) * 111195.0 * (cosLat > 0.01 ? cosLat : 0.01));
}

// Rotates an east/north offset so that `headingDeg` points up the screen.
// ahead > 0 is in front of you; right > 0 is to your right.
inline void mapHeadingUp(float east, float north, float headingDeg, float& right, float& ahead) {
  const float h = headingDeg * static_cast<float>(M_PI / 180.0);
  ahead = north * cosf(h) + east * sinf(h);
  right = east * cosf(h) - north * sinf(h);
}

// Cameras within radiusM of (lat, lon), nearest first. Returns how many were stored (<= max).
inline int mapCollectCameras(const CamDb& db, double lat, double lon, float radiusM, MapCam* out, int max) {
  if (!db.count() || max <= 0) return 0;
  const double latPerM = 1.0 / 111195.0;
  const int32_t latLo = static_cast<int32_t>((lat - radiusM * latPerM) * 1e6);
  const int32_t latHi = static_cast<int32_t>((lat + radiusM * latPerM) * 1e6);
  int n = 0;
  float dist[64];
  if (max > 64) max = 64;
  for (uint32_t i = db.lowerBoundLat(latLo); i < db.count(); i++) {
    const CamRec r = db.rec(i);
    if (r.lat_e6 > latHi) break;
    float e, nn;
    mapEnu(lat, lon, r.lat_e6 * 1e-6, r.lon_e6 * 1e-6, e, nn);
    const float d = sqrtf(e * e + nn * nn);
    if (d > radiusM) continue;
    int pos;
    if (n < max) pos = n++;
    else if (d < dist[max - 1]) pos = max - 1;
    else continue;
    out[pos] = MapCam{e, nn, r.dir, r.maker, r.flags, i};
    dist[pos] = d;
    for (int k = pos; k > 0 && dist[k] < dist[k - 1]; k--) {
      const float td = dist[k]; dist[k] = dist[k - 1]; dist[k - 1] = td;
      const MapCam tc = out[k]; out[k] = out[k - 1]; out[k - 1] = tc;
    }
  }
  return n;
}

// Where you have been: a ring of positions, one every MIN_SPACING_M of travel.
class Trail {
 public:
  static const int N = 96;
  static constexpr float MIN_SPACING_M = 12.0f;
  struct Pt { int32_t lat_e6, lon_e6; };

  // Records the position if you have moved far enough from the last one. Returns true if added.
  bool add(double lat, double lon) {
    if (count_) {
      float e, n;
      mapEnu(lat, lon, at(0).lat_e6 * 1e-6, at(0).lon_e6 * 1e-6, e, n);
      if (e * e + n * n < MIN_SPACING_M * MIN_SPACING_M) return false;
      // A jump of more than 2 km is a GPS glitch or a new session, not a drive: start over.
      if (e * e + n * n > 2000.0f * 2000.0f) clear();
    }
    head_ = (head_ + 1) % N;
    pts_[head_] = Pt{static_cast<int32_t>(lat * 1e6), static_cast<int32_t>(lon * 1e6)};
    if (count_ < N) count_++;
    return true;
  }
  void clear() { count_ = 0; head_ = 0; }
  int count() const { return count_; }
  const Pt& at(int age) const { return pts_[(head_ + N - age) % N]; }   // age 0 = newest

 private:
  Pt pts_[N] = {};
  int head_ = 0, count_ = 0;
};
