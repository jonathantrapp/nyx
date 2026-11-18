#pragma once
// nmea.h - tiny NMEA 0183 parser for a GNSS module on a UART. Pure logic, no Arduino.
// Reads $xxRMC (time, date, position, speed, course) and $xxGGA (satellites, HDOP, altitude).
// Any talker works (GP, GN, GL, GA, BD). Sentences with a bad checksum are dropped.
// Feed it every byte from Serial1; feed() returns true when a sentence just updated the fix.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct GpsFix {
  bool valid = false;          // RMC status 'A': the module has a position fix
  bool timeValid = false;      // date and time both present (can be true before a position fix)
  bool courseValid = false;    // module reported a course (empty while standing still)
  double lat = 0, lon = 0;     // degrees, south/west negative
  float speedMps = 0;
  float courseDeg = 0;
  float hdop = 99.9f;
  float altM = 0;
  uint8_t sats = 0;
  int64_t utcEpoch = 0;        // seconds since 1970-01-01 UTC
  uint32_t sentences = 0;      // every checksum-valid sentence of any type: "is the module talking, at this baud"
  uint32_t rmcCount = 0;       // RMC sentences accepted so far
  uint32_t badChecksums = 0;
  // Satellites in view per system, from $xxGSV (GP = GPS, BD/GB = BeiDou, GL = GLONASS, GA = Galileo).
  uint8_t viewGps = 0, viewBds = 0, viewGlo = 0, viewGal = 0;
  // Antenna status the module reports in "$xxTXT,01,01,01,ANTENNA OK|OPEN|SHORT" (ATGM336H/AT6558 style).
  // 0 = never reported, 1 = ok, 2 = open (nothing connected / cable cut), 3 = short.
  uint8_t antenna = 0;
};

// Short name for GpsFix::antenna, for the console and the log.
inline const char* nmeaAntennaName(uint8_t a) {
  switch (a) {
    case 1: return "ok";
    case 2: return "open";
    case 3: return "short";
    default: return "unknown";
  }
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
inline int64_t nmeaDaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

class NmeaParser {
 public:
  const GpsFix& fix() const { return fix_; }

  bool feed(char c) {
    if (c == '$') { len_ = 0; active_ = true; return false; }
    if (!active_) return false;
    if (c == '\r' || c == '\n') {
      active_ = false;
      if (len_ == 0) return false;
      buf_[len_] = 0;
      return handle(buf_, len_);
    }
    if (len_ >= sizeof(buf_) - 1) { active_ = false; return false; }   // too long: not a sentence we want
    buf_[len_++] = c;
    return false;
  }

 private:
  GpsFix fix_;
  char buf_[120];
  size_t len_ = 0;
  bool active_ = false;

  static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  // Split on commas in place. Returns the number of fields; fields[i] points into s.
  static int split(char* s, const char** fields, int max) {
    int n = 0; fields[n++] = s;
    for (; *s && n < max; s++) if (*s == ',') { *s = 0; fields[n++] = s + 1; }
    return n;
  }

  // "ddmm.mmmm" / "dddmm.mmmm" plus hemisphere -> signed degrees. False if empty or malformed.
  static bool parseCoord(const char* v, const char* hemi, int degDigits, double& out) {
    if (!*v || !*hemi || strlen(v) < (size_t)degDigits + 3) return false;
    char deg[4] = {0};
    memcpy(deg, v, degDigits);
    char* end;
    const double d = strtod(deg, &end);
    if (*end) return false;
    const double min = strtod(v + degDigits, &end);
    if (*end || min >= 60.0) return false;
    out = d + min / 60.0;
    if (*hemi == 'S' || *hemi == 'W') out = -out;
    return *hemi == 'N' || *hemi == 'S' || *hemi == 'E' || *hemi == 'W';
  }

  static bool parseTime(const char* t, int& h, int& m, int& s) {
    if (strlen(t) < 6) return false;
    for (int i = 0; i < 6; i++) if (t[i] < '0' || t[i] > '9') return false;
    h = (t[0] - '0') * 10 + (t[1] - '0'); m = (t[2] - '0') * 10 + (t[3] - '0'); s = (t[4] - '0') * 10 + (t[5] - '0');
    return h < 24 && m < 60 && s < 61;
  }

  static bool parseDate(const char* d, int& day, int& mon, int& year) {
    if (strlen(d) != 6) return false;
    for (int i = 0; i < 6; i++) if (d[i] < '0' || d[i] > '9') return false;
    day = (d[0] - '0') * 10 + (d[1] - '0'); mon = (d[2] - '0') * 10 + (d[3] - '0');
    year = 2000 + (d[4] - '0') * 10 + (d[5] - '0');
    return day >= 1 && day <= 31 && mon >= 1 && mon <= 12;
  }

  bool handle(char* s, size_t len) {
    // Checksum: XOR of everything between '$' and '*'.
    char* star = (char*)memchr(s, '*', len);
    if (!star || (size_t)(s + len - star) != 3) { fix_.badChecksums++; return false; }
    uint8_t sum = 0;
    for (char* p = s; p < star; p++) sum ^= (uint8_t)*p;
    const int hi = hexVal(star[1]), lo = hexVal(star[2]);
    if (hi < 0 || lo < 0 || sum != (uint8_t)(hi * 16 + lo)) { fix_.badChecksums++; return false; }
    *star = 0;
    fix_.sentences++;
    if (strlen(s) < 6 || s[5] != ',') return false;
    const char* f[20];
    const int n = split(s, f, 20);
    if (!strncmp(s + 2, "RMC", 3)) return rmc(f, n);
    if (!strncmp(s + 2, "GGA", 3)) return gga(f, n);
    if (!strncmp(s + 2, "GSV", 3)) { gsv(s, f, n); return false; }
    if (!strncmp(s + 2, "TXT", 3)) { txt(f, n); return false; }
    return false;
  }

  // $xxTXT,total,part,id,text: the module's own status line. Only "ANTENNA OK|OPEN|SHORT" is used; any other
  // text, or a malformed sentence, leaves the last antenna state alone.
  void txt(const char** f, int n) {
    if (n < 5) return;
    static const char kPrefix[] = "ANTENNA ";
    if (strncmp(f[4], kPrefix, sizeof(kPrefix) - 1) != 0) return;
    const char* state = f[4] + sizeof(kPrefix) - 1;
    if (!strcmp(state, "OK")) fix_.antenna = 1;
    else if (!strcmp(state, "OPEN")) fix_.antenna = 2;
    else if (!strcmp(state, "SHORT")) fix_.antenna = 3;
  }

  // $xxGSV field 3 = satellites in view for that talker's system (repeated on every part of a set).
  void gsv(const char* s, const char** f, int n) {
    if (n < 4 || !f[3][0]) return;
    const int v = atoi(f[3]);
    const uint8_t view = (uint8_t)(v < 0 ? 0 : (v > 99 ? 99 : v));
    if (s[0] == 'G' && s[1] == 'P') fix_.viewGps = view;
    else if ((s[0] == 'B' && s[1] == 'D') || (s[0] == 'G' && s[1] == 'B')) fix_.viewBds = view;
    else if (s[0] == 'G' && s[1] == 'L') fix_.viewGlo = view;
    else if (s[0] == 'G' && s[1] == 'A') fix_.viewGal = view;
  }

  bool rmc(const char** f, int n) {
    // 0 id, 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W, 7 speed kn, 8 course, 9 date
    if (n < 10) return false;
    int h, mi, sec, day, mon, year;
    const bool haveTime = parseTime(f[1], h, mi, sec) && parseDate(f[9], day, mon, year);
    fix_.timeValid = haveTime;
    if (haveTime)
      fix_.utcEpoch = nmeaDaysFromCivil(year, (unsigned)mon, (unsigned)day) * 86400 + h * 3600 + mi * 60 + sec;
    double lat, lon;
    fix_.valid = f[2][0] == 'A' && parseCoord(f[3], f[4], 2, lat) && parseCoord(f[5], f[6], 3, lon);
    if (fix_.valid) {
      fix_.lat = lat; fix_.lon = lon;
      fix_.speedMps = f[7][0] ? (float)(atof(f[7]) * 0.514444) : 0.0f;
      fix_.courseValid = f[8][0] != 0;
      if (fix_.courseValid) fix_.courseDeg = (float)atof(f[8]);
    } else {
      fix_.courseValid = false;
    }
    fix_.rmcCount++;
    return true;
  }

  bool gga(const char** f, int n) {
    // 0 id, 1 time, 2 lat, 3 N/S, 4 lon, 5 E/W, 6 quality, 7 sats, 8 hdop, 9 altitude
    if (n < 10) return false;
    fix_.sats = (uint8_t)atoi(f[7]);
    if (f[8][0]) fix_.hdop = (float)atof(f[8]);
    if (f[9][0]) fix_.altM = (float)atof(f[9]);
    return true;                                    // validity comes from RMC, not GGA quality
  }
};
