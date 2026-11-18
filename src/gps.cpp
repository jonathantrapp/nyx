// gps.cpp - see gps.h.
#include "gps.h"
#include <HardwareSerial.h>
#include <time.h>
#include "config.h"
#include "logger.h"
#include "pins.h"
#include "gps_policy.h"

static NmeaParser gParser;
static GpsFix gSim;
static bool gSimOn = false;
static uint32_t gSimLastMs = 0;

// Most modules ship at 9600 (ATGM336H, NEO-6M, BN-220); the rest are common alternatives.
static const uint32_t kBauds[] = {9600, 38400, 115200, 4800, 19200, 57600};
static const int kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);
static const uint32_t PROBE_WINDOW_MS = 1500;   // listen this long at each candidate rate
static const uint32_t PROBE_PAUSE_MS = 5000;    // after a full cycle with no module, wait before retrying
static const uint32_t LOST_AFTER_MS = 10000;    // locked, but silent this long: search again

static uint32_t gBaud = 0;
static int gProbeIdx = -1;
static uint32_t gProbeStartMs = 0, gProbeSentences = 0, gPauseUntilMs = 0;
static uint32_t gLastGoodMs = 0, gLastSentences = 0, gLastRmcMs = 0, gLastRmcCount = 0;

static uint32_t gLastTrackMs = 0, gLastClockLogMs = 0;
static bool gHadFix = false, gClockSet = false;
static bool gLoggedFirstFix = false;
// Antenna status from the module's own "$xxTXT ... ANTENNA OK|OPEN|SHORT" line. Logged as a META row
// "antenna=ok|open|short" when it changes (and once at the first report after boot). Changes are spaced at
// least 1 s apart so a loose connector cannot flood the log; the current state is logged when the gap ends.
static uint8_t gLoggedAntenna = 0;
static uint32_t gAntennaLogMs = 0;
static const uint32_t ANTENNA_LOG_GAP_MS = 1000;

static void openAt(uint32_t baud) {
  Serial1.end();
  Serial1.setRxBufferSize(1024);                                 // must precede begin()
  Serial1.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
}

static void startProbe(int idx, uint32_t now) {
  gProbeIdx = idx;
  openAt(kBauds[idx]);
  gProbeStartMs = now;
  gProbeSentences = gParser.fix().sentences;
}

static uint16_t gRawLines = 0;
void gpsRaw(uint16_t lines) { gRawLines = lines; }
void gpsBegin() { startProbe(0, millis()); }

// Ask the module (ATGM336H, CASIC protocol) to track GPS + BeiDou + GLONASS: more satellites in view means
// faster and steadier fixes, especially through a window. Sent over GPIO 17 -> module RX every time the
// module is found (not saved in the module, so a factory-fresh module behaves the same). A module that
// does not support a system ignores it; `gps` shows satellites in view per system to confirm.
// 2026-11-15: sending PCAS04,7 at every start-up left unit A's module without a fix all night, so nothing is
// sent automatically any more; `gps send <body>` sends one CASIC sentence by hand (e.g. PCAS10,3 = factory reset).
void gpsSendCas(const char* body) {
  uint8_t sum = 0;
  for (const char* p = body; *p; p++) sum ^= static_cast<uint8_t>(*p);
  char line[24];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
  Serial1.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  char note[40]; snprintf(note, sizeof(note), "gps_cas=%.30s", body); loggerMeta(note);
}

static void formatUtc(int64_t epoch, char* out, size_t n) {
  const time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static void onFix(const GpsFix& f, uint32_t now) {
  if (f.valid && !gHadFix) {
    gHadFix = true;
    char note[64];
    if (!gLoggedFirstFix) {
      snprintf(note, sizeof(note), "gps=fix sats=%u ttff_ms=%lu", f.sats, static_cast<unsigned long>(now));
      gLoggedFirstFix = true; // since boot, including startup; UART/parser only, never simulation
    } else snprintf(note, sizeof(note), "gps=fix sats=%u reacquired=1", f.sats);
    loggerMeta(note);
  } else if (!f.valid && gHadFix) {
    gHadFix = false;
    loggerMeta("gps=lost");
  }
  // Satellite time is the first trustworthy wall clock this unit has. Use it (once, then every
  // 30 minutes to catch drift) as the logger's clock reference. Ignore dates before 2024.
  if (f.valid && f.timeValid && f.utcEpoch > 1704067200LL &&
      (!gClockSet || now - gLastClockLogMs > 30UL * 60UL * 1000UL)) {
    char utc[24]; formatUtc(f.utcEpoch, utc, sizeof(utc));
    loggerSetClock(utc);
    gClockSet = true; gLastClockLogMs = now;
  }
  // Track: every 5 s while moving, every 60 s while parked.
  if (f.valid) {
    if (!gLastTrackMs || now - gLastTrackMs >= gpsTrackIntervalMs(f.speedMps)) {
      gLastTrackMs = now ? now : 1;
      char text[96];
      snprintf(text, sizeof(text), "lat=%.6f lon=%.6f spd=%.1f crs=%d sats=%u hdop=%.1f", f.lat, f.lon,
               f.speedMps, f.courseValid ? static_cast<int>(f.courseDeg + 0.5f) : -1, f.sats, f.hdop);
      loggerGps(text);
    }
  }
}

static void advanceSim(uint32_t now) {
  const float dt = (now - gSimLastMs) / 1000.0f;
  gSimLastMs = now;
  if (gSim.speedMps <= 0.0f || dt <= 0.0f) return;
  const double d = gSim.speedMps * dt;
  const double rad = gSim.courseDeg * M_PI / 180.0;
  gSim.lat += d * cos(rad) / 111195.0;
  gSim.lon += d * sin(rad) / (111195.0 * cos(gSim.lat * M_PI / 180.0));
}

void gpsService() {
  const uint32_t now = millis();

  if (gSimOn) { advanceSim(now); }

  // Drain the UART (bounded, so a chatty module can't starve loop()).
  for (int n = 0; n < 256 && Serial1.available(); n++) {
    const char ch = static_cast<char>(Serial1.read());
    if (gRawLines) { Serial.write(ch); if (ch == '\n') gRawLines--; }   // "gps raw N": echo module output
    if (gParser.feed(ch)) {
      const GpsFix& f = gParser.fix();
      if (f.rmcCount != gLastRmcCount) {
        gLastRmcCount = f.rmcCount; gLastRmcMs = now;
        if (!gSimOn) onFix(f, now);
      }
    }
  }
  const uint32_t sentences = gParser.fix().sentences;
  if (sentences != gLastSentences) { gLastSentences = sentences; gLastGoodMs = now; }

  const uint8_t antenna = gParser.fix().antenna;
  if (antenna != gLoggedAntenna && (!gAntennaLogMs || now - gAntennaLogMs >= ANTENNA_LOG_GAP_MS)) {
    char note[24];
    snprintf(note, sizeof(note), "antenna=%s", nmeaAntennaName(antenna));
    loggerMeta(note);
    gLoggedAntenna = antenna;
    gAntennaLogMs = now ? now : 1;
  }

  // Baud search.
  if (gBaud == 0) {
    if (gPauseUntilMs && static_cast<int32_t>(now - gPauseUntilMs) < 0) return;
    if (gPauseUntilMs) { gPauseUntilMs = 0; startProbe(0, now); return; }
    if (sentences - gProbeSentences >= 2) {                      // two valid checksummed sentences
      gBaud = kBauds[gProbeIdx];
      char note[32]; snprintf(note, sizeof(note), "gps_baud=%lu", static_cast<unsigned long>(gBaud));
      loggerMeta(note);
      Serial.printf("[gps] module found at %lu baud\n", static_cast<unsigned long>(gBaud));
    } else if (now - gProbeStartMs >= PROBE_WINDOW_MS) {
      if (gProbeIdx + 1 < kBaudCount) startProbe(gProbeIdx + 1, now);
      else { gPauseUntilMs = now + PROBE_PAUSE_MS; }             // nothing answered: not connected yet
    }
  } else if (now - gLastGoodMs > LOST_AFTER_MS) {
    Serial.println("[gps] module went silent, searching again");
    gBaud = 0; startProbe(0, now);
  }
}

const GpsFix& gpsFix() { return gSimOn ? gSim : gParser.fix(); }
bool gpsSimActive() { return gSimOn; }
uint32_t gpsBaud() { return gBaud; }
bool gpsModuleTalking() { return gBaud != 0 && millis() - gLastGoodMs < 5000; }
bool gpsUsable() {
  if (gSimOn) return true;
  return gParser.fix().valid && gLastRmcMs && millis() - gLastRmcMs < 3000;
}
int64_t gpsUtcNow() {
  const GpsFix& f = gParser.fix();
  if (gSimOn || !f.timeValid || !gLastRmcMs) return 0;
  return f.utcEpoch + static_cast<int64_t>((millis() - gLastRmcMs) / 1000);
}

void gpsSim(double lat, double lon, float speedMps, float courseDeg) {
  gSim = GpsFix();
  gSim.valid = true; gSim.courseValid = true; gSim.timeValid = false;
  gSim.lat = lat; gSim.lon = lon; gSim.speedMps = speedMps; gSim.courseDeg = courseDeg;
  gSim.sats = 9; gSim.hdop = 0.9f;
  gSimLastMs = millis();
  gSimOn = true;
}
void gpsSimOff() { gSimOn = false; }

void gpsPrint(Print& out) {
  const GpsFix& f = gParser.fix();
  if (gSimOn) {
    const GpsFix& s = gSim;
    out.printf("gps: SIMULATED lat %.6f lon %.6f  %.1f m/s  course %.0f\n", s.lat, s.lon, s.speedMps, s.courseDeg);
  }
  if (!gBaud) {
    out.printf("gps module: %s (trying %lu baud; wiring: module TX -> GPIO %d, power 3V3, GND)\n",
               gPauseUntilMs ? "no data" : "searching", static_cast<unsigned long>(gProbeIdx >= 0 ? kBauds[gProbeIdx] : 0),
               PIN_GPS_RX);
    return;
  }
  out.printf("gps module: %lu baud, %lu sentences, %lu bad checksums\n", static_cast<unsigned long>(gBaud),
             static_cast<unsigned long>(f.sentences), static_cast<unsigned long>(f.badChecksums));
  out.printf("in view: GPS %u, BeiDou %u, GLONASS %u, Galileo %u\n", f.viewGps, f.viewBds, f.viewGlo, f.viewGal);
  out.printf("antenna (module status): %s\n", nmeaAntennaName(f.antenna));
  if (f.valid) {
    out.printf("fix: %.6f, %.6f  %.1f m/s  course %s%.0f  sats %u  hdop %.1f  alt %.0f m\n", f.lat, f.lon, f.speedMps,
               f.courseValid ? "" : "(none) ", f.courseDeg, f.sats, f.hdop, f.altM);
  } else {
    out.printf("no position fix yet (sats seen %u). A cold start outdoors can take 1-2 minutes.\n", f.sats);
  }
  if (f.timeValid) { char utc[24]; formatUtc(gpsUtcNow(), utc, sizeof(utc)); out.printf("utc: %s\n", utc); }
}
