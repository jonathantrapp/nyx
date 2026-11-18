// Native tests for src/nmea.h and src/camera_map.h.  Run via: python tools/test_camera_native.py
// argv[1] = fixture camera file built by tools/build_camera_map.py (required)
// argv[2] = real cameras/cameras.bin (optional)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include "nmea.h"
#include "gps_policy.h"
#include "camera_map.h"

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> v;
  FILE* f = fopen(path, "rb");
  if (!f) return v;
  uint8_t tmp[65536]; size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) v.insert(v.end(), tmp, tmp + n);
  fclose(f);
  return v;
}

static std::string sentence(const char* body) {          // adds $, *HH and CRLF
  uint8_t sum = 0;
  for (const char* p = body; *p; p++) sum ^= (uint8_t)*p;
  char tail[8]; snprintf(tail, sizeof tail, "*%02X\r\n", sum);
  return std::string("$") + body + tail;
}
static bool feedAll(NmeaParser& p, const std::string& s) {
  bool any = false;
  for (char c : s) any |= p.feed(c);
  return any;
}

static void testNmea() {
  NmeaParser p;
  // The canonical example sentences, with their real checksums.
  CHECK(feedAll(p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"));
  CHECK(feedAll(p, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n"));
  const GpsFix& f = p.fix();
  CHECK(f.valid && f.timeValid && f.courseValid);
  NEAR(f.lat, 48.117300, 1e-6); NEAR(f.lon, 11.516667, 1e-6);
  NEAR(f.speedMps, 11.524, 0.01); NEAR(f.courseDeg, 84.4, 0.01);
  CHECK(f.sats == 8); NEAR(f.hdop, 0.9, 0.001); NEAR(f.altM, 545.4, 0.01);
  CHECK(f.utcEpoch == 3920186119LL);   // date field "94" is read as 2094: two-digit years are 20xx

  // Bad checksum: dropped, counted, fix unchanged.
  const double before = f.lat;
  CHECK(!feedAll(p, "$GPRMC,123520,A,5000.000,N,01131.000,E,022.4,084.4,230394,003.1,W*00\r\n"));
  CHECK(p.fix().badChecksums == 1); CHECK(p.fix().lat == before);

  // Southern/western hemispheres and a GN talker.
  CHECK(feedAll(p, sentence("GNRMC,000000.00,A,3351.000,S,15112.000,W,000.0,,010100,,,A")));
  NEAR(p.fix().lat, -33.85, 1e-6); NEAR(p.fix().lon, -151.2, 1e-6);
  CHECK(p.fix().utcEpoch == 946684800);                     // 2000-01-01 00:00:00 UTC
  CHECK(!p.fix().courseValid);                              // empty course while standing still

  // Time known but no position yet (cold start): clock usable, fix not.
  CHECK(feedAll(p, sentence("GPRMC,213045.00,V,,,,,,,101126,,,N")));
  CHECK(!p.fix().valid && p.fix().timeValid);
  CHECK(p.fix().utcEpoch == 1794346245LL);                  // 2026-11-10 21:30:45 UTC

  // Junk and oversized input do not crash or update.
  CHECK(!feedAll(p, "garbage without a dollar\r\n"));
  std::string huge = "$" + std::string(500, 'A') + "\r\n";
  CHECK(!feedAll(p, huge));
  CHECK(!feedAll(p, "$GPRMC,1*\r\n"));
}

static uint32_t indexAtLat(const CamDb& db, double lat) {
  uint32_t i = db.lowerBoundLat((int32_t)(lat * 1e6 - 0.5));
  return i;
}

static void testCameras(const char* fixturePath) {
  std::vector<uint8_t> buf = readFile(fixturePath);
  CamDb db;
  CHECK(db.attach(buf.data(), buf.size()));
  CHECK(db.count() == 7);
  CHECK(db.dataDate() == 20261110);
  // Corrupt / truncated files are refused.
  { CamDb bad; CHECK(!bad.attach(buf.data(), 10)); CHECK(!bad.attach(buf.data(), buf.size() - 1));
    std::vector<uint8_t> m = buf; m[0] = 'X'; CHECK(!bad.attach(m.data(), m.size())); }
  // Sorted by latitude (the lookup depends on it).
  for (uint32_t i = 1; i < db.count(); i++) CHECK(db.rec(i - 1).lat_e6 <= db.rec(i).lat_e6);

  const uint32_t A = indexAtLat(db, 40.0050), B = indexAtLat(db, 40.0052), C = indexAtLat(db, 40.0030),
                 E = indexAtLat(db, 40.0090);
  CHECK(db.rec(A).maker == CAM_MAKER_FLOCK && db.rec(E).maker == CAM_MAKER_UNKNOWN);
  CHECK(db.rec(A).dir == 180 && (db.rec(A).flags & CAM_FLAG_DIR));
  CHECK(db.rec(E).flags & CAM_FLAG_MULTI);

  // Drive north up the road at 15 m/s, one fix per second, three times.
  CamAlerter alerter;
  int ahead[2] = {0, 0}, close[2] = {0, 0};     // [0]=A [1]=E
  bool sawC = false, sawD = false, sawB = false;
  auto drive = [&](uint32_t startMs) {
    int announces = 0;
    for (int t = 0; t <= 130; t++) {
      CamFix fix{40.0000 + 15.0 * t / 111195.0, -75.0000, 15.0f, 0.0f, true};
      CamHit hit; CamStage stage; bool announce;
      if (!alerter.update(db, fix, startMs + t * 1000u, hit, stage, announce)) continue;
      if (hit.idx == C) sawC = true;
      if (hit.idx == indexAtLat(db, 39.9995)) sawD = true;
      if (hit.idx == B) sawB = true;
      CHECK(hit.distM <= camWarnRadiusM(15.0f) + 1);
      if (announce) {
        announces++;
        int k = hit.idx == A ? 0 : (hit.idx == E ? 1 : -1);
        CHECK(k >= 0);
        if (k >= 0) { if (stage == CAM_STAGE_AHEAD) ahead[k]++; else close[k]++; }
        if (hit.idx == A) { CHECK(hit.facing); NEAR(hit.relDeg, 0.0, 1.0); }
        if (hit.idx == E) CHECK(hit.facing);                          // two directions listed -> assume facing
      }
    }
    return announces;
  };
  CHECK(drive(1000) == 4);                                   // A ahead, A close, E ahead, E close
  CHECK(ahead[0] == 1 && close[0] == 1 && ahead[1] == 1 && close[1] == 1);
  CHECK(!sawC && !sawD);                                     // off-path and behind-you cameras never shown
  CHECK(sawB);                                               // B was shown after passing A, but silently
  CHECK(drive(1000 + 300 * 1000u) == 0);                     // second pass 5 min later: still on cooldown
  CHECK(drive(1000 + 1500 * 1000u) == 4);                    // 25 min later: announced again

  // Southbound toward the back of A's pole, from the north: B (dir 0) faces north, so it sees us.
  { CamAlerter a2; CamHit hit; CamStage st; bool ann;
    CamFix fix{40.0070, -75.0000, 15.0f, 180.0f, true};
    CHECK(a2.update(db, fix, 5000, hit, st, ann));
    CHECK(hit.idx == B && hit.facing && ann && st == CAM_STAGE_AHEAD);
    // Same road, but a camera that faces away from us: A (dir 180) when we come from the north at A's level.
    CamFix fix2{40.0056, -75.0000, 15.0f, 180.0f, true};
    CamHit h2; CHECK(db.findAhead(fix2, h2)); CHECK(h2.idx == B);
    CamFix fix3{40.0051, -75.0000, 15.0f, 180.0f, true};      // B is now behind, A is 11 m ahead
    CamHit h3; CHECK(db.findAhead(fix3, h3)); CHECK(h3.idx == A && !h3.facing); }

  // Stopped (no valid course): proximity only.
  { CamFix still{40.0050 + 30.0 / 111195.0, -75.0000, 0.0f, 0.0f, false};
    CamHit hit; CHECK(db.findAhead(still, hit)); CHECK(hit.idx == B || hit.idx == A); NEAR(hit.distM, 8.0, 12.0);
    CamFix far{40.0050 + 150.0 / 111195.0, -75.0000, 0.0f, 0.0f, false};
    CHECK(!db.findAhead(far, hit));
    CamFix crawl{40.0020, -75.0000, 1.0f, 0.0f, true};         // GPS course is noise this slowly
    CHECK(!db.findAhead(crawl, hit)); }

  // Dateline / pole sanity: nothing found, nothing crashes.
  { CamFix odd{89.9999, 179.9999, 20.0f, 90.0f, true}; CamHit hit; CHECK(!db.findAhead(odd, hit)); }
}

static void testRealDb(const char* path) {
  std::vector<uint8_t> buf = readFile(path);
  if (buf.empty()) { printf("(real camera file not found, skipped)\n"); return; }
  CamDb db;
  CHECK(db.attach(buf.data(), buf.size()));
  CHECK(db.count() > 50000);
  for (uint32_t i = 1; i < db.count(); i++) if (db.rec(i - 1).lat_e6 > db.rec(i).lat_e6) { CHECK(false); break; }
  // A Flock camera in Amarillo, TX (OSM node 31431226): found within 5 m when standing on it.
  CamFix at{35.1616409, -101.9201859, 0.0f, 0.0f, false};
  CamHit hit; CHECK(db.findAhead(at, hit));
  CHECK(hit.distM < 5.0f); CHECK(hit.maker == CAM_MAKER_FLOCK);
  printf("real db: %u cameras, snapshot %u\n", db.count(), db.dataDate());
}

int main(int argc, char** argv) {
  CHECK(gpsDisplayMph(0.0f)==0.0f);
  CHECK(gpsDisplayMph(0.99f)==0.0f);
  NEAR(gpsDisplayMph(1.0f),2.23694f,0.0001);
  CHECK(gpsTrackIntervalMs(1.1f)==60000UL);
  CHECK(gpsTrackIntervalMs(1.99f)==60000UL);
  CHECK(gpsTrackIntervalMs(2.0f)==5000UL);
  if (argc < 2) { printf("usage: camera-tests fixture.bin [cameras.bin]\n"); return 2; }
  testNmea();
  testCameras(argv[1]);
  if (argc > 2) testRealDb(argv[2]);
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
