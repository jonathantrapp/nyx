#pragma once
// camera_map.h - "is a mapped ALPR camera ahead of me?" Pure logic: no Arduino, no filesystem.
// The firmware loads cameras/cameras.bin into PSRAM and hands the buffer to CamDb::attach().
// Format is documented in tools/build_camera_map.py. Exercised on the PC by
// tools/test_camera_native.py (python tools/test_camera_native.py).
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum CamMaker : uint8_t { CAM_MAKER_UNKNOWN = 0, CAM_MAKER_FLOCK = 1, CAM_MAKER_MOTOROLA = 2,
                          CAM_MAKER_AXON = 3, CAM_MAKER_OTHER = 4 };
static const uint16_t CAM_DIR_UNKNOWN = 0xFFFF;
static const uint8_t CAM_FLAG_DIR = 1, CAM_FLAG_MULTI = 2;

struct CamRec {                      // 12 bytes on disk
  int32_t lat_e6, lon_e6;
  uint16_t dir;                      // direction the camera faces, degrees clockwise from north
  uint8_t maker, flags;
};

// Tuning in one place.
static const float CAM_MIN_SPEED_MPS   = 2.5f;   // below this the GPS course is noise
static const float CAM_NEAR_RADIUS_M   = 100.0f; // radius used while (nearly) stopped
static const float CAM_WARN_SECONDS    = 20.0f;  // how far ahead to warn, in seconds of driving
static const float CAM_WARN_MIN_M      = 300.0f;
static const float CAM_WARN_MAX_M      = 900.0f;
static const float CAM_CLOSE_M         = 150.0f; // second, closer warning
static const float CAM_CROSS_BASE_M    = 50.0f;  // sideways slack: GPS error + node placement + road width
static const float CAM_CROSS_PER_M     = 0.10f;  // ...growing with distance
static const float CAM_FACING_TOL_DEG  = 70.0f;  // camera counts as "facing you" within this of head-on

inline float camWarnRadiusM(float speedMps) {
  float r = speedMps * CAM_WARN_SECONDS;
  return r < CAM_WARN_MIN_M ? CAM_WARN_MIN_M : (r > CAM_WARN_MAX_M ? CAM_WARN_MAX_M : r);
}

struct CamFix {
  double lat, lon;                   // degrees
  float speedMps;
  float courseDeg;                   // direction of travel
  bool courseValid;
};

struct CamHit {
  uint32_t idx;                      // index into the database; stable for one loaded file
  float distM;                       // straight-line distance
  float bearingDeg;                  // absolute bearing to the camera
  float relDeg;                      // bearing relative to travel (-180..180, 0 = dead ahead)
  float crossM;                      // sideways offset from your path
  bool facing;                       // camera lens points back at you (or direction unknown/multiple)
  bool dirKnown;                     // facing was actually computed: single lens, mapped direction, moving
  uint8_t maker;
  double lat, lon;
};

inline float camAngleDiff(float a, float b) {          // a - b wrapped to -180..180
  float d = fmodf(a - b, 360.0f);
  if (d > 180.0f) d -= 360.0f; else if (d < -180.0f) d += 360.0f;
  return d;
}

class CamDb {
 public:
  bool attach(const uint8_t* buf, size_t len) {
    base_ = nullptr; count_ = 0; date_ = 0;
    if (!buf || len < 20 || memcmp(buf, "CAM1", 4) != 0) return false;
    uint16_t version, recSize; uint32_t count, date;
    memcpy(&version, buf + 4, 2); memcpy(&recSize, buf + 6, 2);
    memcpy(&count, buf + 8, 4); memcpy(&date, buf + 12, 4);
    if (version != 1 || recSize != sizeof(CamRec)) return false;
    if (count > (len - 20) / sizeof(CamRec)) return false;      // truncated file
    base_ = buf + 20; count_ = count; date_ = date;
    return true;
  }
  uint32_t count() const { return count_; }
  uint32_t dataDate() const { return date_; }      // YYYYMMDD of the OSM snapshot, 0 if unknown
  CamRec rec(uint32_t i) const { CamRec r; memcpy(&r, base_ + (size_t)i * sizeof(CamRec), sizeof r); return r; }

  // First record with lat_e6 >= v (records are sorted by latitude).
  uint32_t lowerBoundLat(int32_t v) const {
    uint32_t lo = 0, hi = count_;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if (rec(mid).lat_e6 < v) lo = mid + 1; else hi = mid; }
    return lo;
  }

  // Nearest camera that is ahead of the vehicle (or merely nearby when stopped).
  bool findAhead(const CamFix& fix, CamHit& best) const {
    if (!count_) return false;
    const bool moving = fix.courseValid && fix.speedMps >= CAM_MIN_SPEED_MPS;
    const float radius = moving ? camWarnRadiusM(fix.speedMps) : CAM_NEAR_RADIUS_M;
    const double latPerM = 1.0 / 111195.0;
    const double cosLat = cos(fix.lat * M_PI / 180.0);
    const double lonPerM = latPerM / (cosLat > 0.01 ? cosLat : 0.01);
    const int32_t latLo = (int32_t)((fix.lat - radius * latPerM) * 1e6);
    const int32_t latHi = (int32_t)((fix.lat + radius * latPerM) * 1e6);
    const float courseRad = fix.courseDeg * (float)(M_PI / 180.0);
    bool found = false;
    for (uint32_t i = lowerBoundLat(latLo); i < count_; i++) {
      const CamRec r = rec(i);
      if (r.lat_e6 > latHi) break;
      const double north = (r.lat_e6 * 1e-6 - fix.lat) / latPerM;
      const double east  = (r.lon_e6 * 1e-6 - fix.lon) / lonPerM;
      const float dist = (float)sqrt(north * north + east * east);
      if (dist > radius) continue;
      float bearing = (float)(atan2(east, north) * 180.0 / M_PI); if (bearing < 0) bearing += 360.0f;
      float rel = moving ? camAngleDiff(bearing, fix.courseDeg) : 0.0f;
      float cross = 0.0f;
      if (moving) {
        const float along = (float)(north * cos(courseRad) + east * sin(courseRad));
        cross = (float)(east * cos(courseRad) - north * sin(courseRad));
        if (along <= 0.0f) continue;                                   // behind you
        if (fabsf(cross) > CAM_CROSS_BASE_M + CAM_CROSS_PER_M * along) continue;   // not on your path
      }
      if (found && dist >= best.distM) continue;
      best.idx = i; best.distM = dist; best.bearingDeg = bearing; best.relDeg = rel; best.crossM = cross;
      best.maker = r.maker; best.lat = r.lat_e6 * 1e-6; best.lon = r.lon_e6 * 1e-6;
      best.facing = true;
      best.dirKnown = false;
      if (moving && (r.flags & CAM_FLAG_DIR) && !(r.flags & CAM_FLAG_MULTI) && r.dir != CAM_DIR_UNKNOWN) {
        best.facing = fabsf(camAngleDiff((float)r.dir, fix.courseDeg + 180.0f)) <= CAM_FACING_TOL_DEG;
        best.dirKnown = true;
      }
      found = true;
    }
    return found;
  }

 private:
  const uint8_t* base_ = nullptr;
  uint32_t count_ = 0, date_ = 0;
};

// Decides when to announce. Call update() about once a second with the latest fix.
// A camera is announced once when it comes into range (AHEAD) and once more when it is close
// (CLOSE). It is not announced again until it has been out of sight for CAM_COOLDOWN_MS. Cameras
// within CAM_CLUSTER_M of one already announced (two lenses on one pole, or a pair at an
// intersection) share its alert and stay quiet.
enum CamStage : uint8_t { CAM_STAGE_NONE = 0, CAM_STAGE_AHEAD = 1, CAM_STAGE_CLOSE = 2 };
static const uint32_t CAM_COOLDOWN_MS = 10UL * 60UL * 1000UL;
static const float CAM_CLUSTER_M = 60.0f;

class CamAlerter {
 public:
  // Returns true when there is a camera to show. `announce` says whether to beep this time.
  bool update(const CamDb& db, const CamFix& fix, uint32_t nowMs, CamHit& hit, CamStage& stage, bool& announce) {
    announce = false; stage = CAM_STAGE_NONE;
    if (!db.findAhead(fix, hit)) return false;
    Seen* s = find(hit.idx, nowMs);
    if (!s) {
      s = alloc(hit.idx, nowMs);
      s->lat = hit.lat; s->lon = hit.lon;
      s->quiet = clusterAnnounced(hit, nowMs);        // a neighbour already spoke for this spot
    }
    s->t = nowMs;                                     // "last seen": the cooldown runs from here
    const CamStage want = hit.distM <= CAM_CLOSE_M ? CAM_STAGE_CLOSE : CAM_STAGE_AHEAD;
    if (want > s->stage) {
      s->stage = want;
      announce = !s->quiet;
      if (announce) s->announcedAt = nowMs;
    }
    stage = s->stage;
    return true;
  }

 private:
  struct Seen { bool used; uint32_t idx; uint32_t t; uint32_t announcedAt; CamStage stage; bool quiet; double lat, lon; };
  static const int N = 12;
  Seen seen_[N] = {};

  Seen* find(uint32_t idx, uint32_t now) {
    for (auto& s : seen_) if (s.used && s.idx == idx && (uint32_t)(now - s.t) <= CAM_COOLDOWN_MS) return &s;
    return nullptr;
  }
  Seen* alloc(uint32_t idx, uint32_t now) {
    Seen* pick = &seen_[0];
    for (auto& s : seen_) {
      if (!s.used || (uint32_t)(now - s.t) > CAM_COOLDOWN_MS) { pick = &s; break; }
      if ((uint32_t)(now - s.t) > (uint32_t)(now - pick->t)) pick = &s;       // else evict the oldest
    }
    *pick = Seen{}; pick->used = true; pick->idx = idx; pick->t = now;
    return pick;
  }
  bool clusterAnnounced(const CamHit& hit, uint32_t now) const {
    for (const auto& s : seen_) {
      if (!s.used || !s.announcedAt || (uint32_t)(now - s.announcedAt) > CAM_COOLDOWN_MS) continue;
      const double dn = (hit.lat - s.lat) * 111195.0;
      const double de = (hit.lon - s.lon) * 111195.0 * cos(hit.lat * M_PI / 180.0);
      if (sqrt(dn * dn + de * de) <= CAM_CLUSTER_M) return true;
    }
    return false;
  }
};
