// commands.cpp - serial console for getting data off the unit and poking at settings.
#include "commands.h"
#include "breadcrumbs.h"
#include <Arduino.h>
#include <LittleFS.h>
#include "board_info.h"
#include "config.h"
#include "csv_writer.h"
#include "detector.h"
#include "logger.h"
#include "loop_watchdog.h"
#include "pins.h"
#include "sniffer.h"
#include "ui.h"
#include "settings.h"
#include "diagnostics.h"
#include "storage_estimate.h"
#include "camalert.h"
#include "gps.h"
#include "tft_ui.h"
#include "controls.h"
#include <esp_rom_crc.h>

void commandsPrintHelp() {
  Serial.println(
      "Commands:\n"
      "  help                 this list\n"
      "  info                 board check: chip, real flash size, PSRAM, last reset reason\n"
      "  status               session, rows, storage, scan settings\n"
      "  stats                frames heard per channel, drops, slowest flush\n"
      "  list                 list session files\n"
      "  dump <name>          print a session (e.g. dump s0001); framed by === BEGIN/END ===\n"
      "  delete <name>        delete one session (not the current one)\n"
      "  delete all yes       delete every session\n"
      "  space                free / total storage\n"
      "  mark [note]          same as a short button press\n"
      "  new                  start a new session file (same as long press)\n"
      "  time <UTC text>      tell the unit the real time, e.g. time 2026-11-05T14:30:00Z\n"
      "  dwell <ms>           channel dwell time (20-5000)\n"
      "  beacon <ms>          min ms between logged beacons per MAC (0 = log all)\n"
      "  data <ms>            min ms between logged data frames per sender (0 = off)\n"
      "  tone <hz> [ms]       play a test tone on the buzzer pin (default 3000 ms, max 10000)\n"
      "  gps                  GPS module status (baud, fix, satellites, UTC)\n"
      "  gps sim <lat> <lon> <course> <mph>   pretend to drive (bench test); 'gps sim off' ends it\n"
      "  cam                  camera-map status; 'cam near' lists the 5 closest mapped cameras\n"
      "  camput <bytes> <crc32hex>   receive /cameras.bin (use tools/push_cameras.py)\n"
      "  detections           every device that matched the watchlist this boot\n"
      "  alert-test           run synthetic alert matching/policy checks; no RF\n"
      "  mute on|off          silence detection beeps (screen + LED still alert)\n"
      "  ble on|off           passive BLE scanning slice\n"
      "  led on|dim|off       RGB LED brightness (default dim; off = no heartbeat)\n"
      "  mode research|car    saved logging policy; starts a new session\n"
      "  volume <0-100>       saved software tone level\n"
      "  brightness <0-100>   saved OLED contrast and colour-screen backlight\n"
      "  tft                  colour screen status; 'tft auto|on|off' saved (on = skip the ID check)\n"
      "  tft demo <screen> [s]   listening|camera|close|away|nodir|rf|follower|boot for s seconds (default 20); 'tft demo off'\n"
      "  tft utc <hours>      clock offset from UTC for the screen, e.g. tft utc -5 (saved)\n"
      "  knob                 pin levels and menu state; 'knob cw|ccw [n]|press|long|mute' simulates the knob and button\n"
      "  tft dump             print the current frame as base64 (see tools/tft_shot.py)\n"
      "  tft chip ili|st, tft rot 1|3, tft bgr 0|1, tft inv 0|1, tft mhz N, tft panel reset   fix the picture (saved)\n"
      "  tft verify           full-area RAM write/read speed test (temporary; no settings saved)\n"
      "  settings [reset]     print saved controls / restore defaults\n"
      "  storage-test         non-destructive storage policy/cache checks\n"
      "  rotation             read-only rotation index (cached; reads no files)\n"
      "  rotation-bench       read-only timing: classification replay + one simulated pass (~15 s)\n"
      "  crash                show saved flash coredump status (read-only)\n"
      "  heap-check           check heap integrity; report and log result\n"
      "  panic-test yes       intentionally abort; replaces saved coredump\n"
      "  format yes           erase the whole filesystem\n"
      "  wdt-test yes         intentionally stall loop; watchdog should reboot\n"
      "  wdt-test long        20 s watchdog-pause test; no files erased\n"
      "  reboot");
}

static void printSpace() {
  const LoggerStatus& s = loggerStatus();
  Serial.printf("storage: %lu KB free of %lu KB%s\n", static_cast<unsigned long>(s.freeBytes / 1024),
                static_cast<unsigned long>(s.totalBytes / 1024), s.full ? "  ** reserve pressure **" : "");
}

static void printStatus() {
  const LoggerStatus& s = loggerStatus();
  Serial.printf("unit %s  fw %s  session %s  rows %lu  marks %lu\n", UNIT_ID, FW_VERSION, s.session,
                static_cast<unsigned long>(s.rows), static_cast<unsigned long>(s.marks));
  Serial.printf("channel %u  dwell %u ms  beacon limit %lu ms  data limit %lu ms\n", snifferChannel(),
                snifferDwellMs(), static_cast<unsigned long>(snifferBeaconMinMs()),
                static_cast<unsigned long>(snifferDataMinMs()));
  printSpace();
  char estimate[24]; storageEstimateText(estimate,sizeof(estimate),loggerEstimatedMinutes());
  Serial.printf("storage estimate: %s (until rotation; recent logged-byte rate)\n",estimate);
  Serial.printf("mode %s  file_bytes %lu  rotated %lu  apple_collapsed %lu\n",loggerModeName(),
      static_cast<unsigned long>(s.fileBytes),static_cast<unsigned long>(s.rotatedFiles),
      static_cast<unsigned long>(s.appleCollapsed));
  if (s.unclassified) Serial.printf("rotation: %lu older session(s) still being classified in the background\n",
                                    static_cast<unsigned long>(s.unclassified));
  if (!CSV_WRITER_DONE) Serial.println("NOTE: csvWriteRow() is still the stub - nothing is being logged.");
  if (s.writeError) Serial.println("WARNING: last flush failed");
  if (s.bufferOverflows) Serial.printf("WARNING: %lu rows lost to RAM buffer overflow\n",
                                       static_cast<unsigned long>(s.bufferOverflows));
  if (loggerClockString()[0]) Serial.printf("host clock: %s (set at ms=%lu)\n", loggerClockString(),
                                            static_cast<unsigned long>(loggerClockSetMs()));
  else Serial.println("host clock: not set (rows carry millis-since-boot only)");
}

static void printStats() {
  const SnifferStats& st = snifferStats();
  const LoggerStatus& ls = loggerStatus();
  diagnosticsPrintHealth(Serial);
  Serial.printf("flush_last_ms=%lu open_ms=%lu write_ms=%lu close_ms=%lu space_ms=%lu (space_ms = last real query, outside the flush)\n",
      static_cast<unsigned long>(ls.flushLastMs),static_cast<unsigned long>(ls.flushOpenMs),
      static_cast<unsigned long>(ls.flushWriteMs),static_cast<unsigned long>(ls.flushCloseMs),
      static_cast<unsigned long>(ls.flushSpaceMs));
  Serial.printf("flush_win_max_ms=%lu close_win_max_ms=%lu space_win_max_ms=%lu\n",
      static_cast<unsigned long>(ls.flushWindowMaxMs),static_cast<unsigned long>(ls.closeWindowMaxMs),
      static_cast<unsigned long>(ls.spaceWindowMaxMs));
  Serial.printf("mode=%s rotated=%lu rotation_errors=%lu apple_collapsed=%lu write_errors=%lu\n",loggerModeName(),
      static_cast<unsigned long>(ls.rotatedFiles),static_cast<unsigned long>(ls.rotationErrors),
      static_cast<unsigned long>(ls.appleCollapsed),static_cast<unsigned long>(ls.writeErrors));
  Serial.printf("rotation_max_ms=%lu phase=%s  rot_win_max_ms=%lu win_phase=%s  (slowest single background op)\n",
      static_cast<unsigned long>(ls.rotationMaxMs),ls.rotationMaxPhase&&*ls.rotationMaxPhase?ls.rotationMaxPhase:"none",
      static_cast<unsigned long>(ls.rotationWindowMaxMs),ls.rotationWindowPhase&&*ls.rotationWindowPhase?ls.rotationWindowPhase:"none");
  Serial.printf("slowest_delete_ms=%lu (remove=%lu nvs=%lu print=%lu) session=s%04lu  del_win_max_ms=%lu\n",
      static_cast<unsigned long>(ls.deleteMaxMs),static_cast<unsigned long>(ls.deleteMaxRemoveMs),
      static_cast<unsigned long>(ls.deleteMaxNvsMs),static_cast<unsigned long>(ls.deleteMaxPrintMs),
      static_cast<unsigned long>(ls.deleteMaxNumber),static_cast<unsigned long>(ls.deleteWindowMaxMs));
  Serial.printf("rotation: unclassified=%lu classified=%lu scan_kb=%lu space_queries=%lu nvs_writes=%lu\n",
      static_cast<unsigned long>(ls.unclassified),static_cast<unsigned long>(ls.classifiedFiles),
      static_cast<unsigned long>(ls.scanBytes/1024),static_cast<unsigned long>(ls.spaceQueries),
      static_cast<unsigned long>(ls.nvsWrites));
  Serial.printf("flush_max_ms=%lu flush_max_at_ms=%lu (since boot; start time)\n",
                static_cast<unsigned long>(ls.flushMaxMs),
                static_cast<unsigned long>(ls.flushMaxAtMs));
  Serial.printf("wifi mgmt heard %lu  data heard %lu  ble heard %lu  queued %lu  dropped %lu  (BLE %s)\n",
                static_cast<unsigned long>(st.seen), static_cast<unsigned long>(st.dataSeen),
                static_cast<unsigned long>(st.bleSeen),
                static_cast<unsigned long>(st.queued), static_cast<unsigned long>(st.dropped),
                snifferBleEnabled() ? "on" : "off");
  for (int ch = HOP_CHANNEL_MIN; ch <= HOP_CHANNEL_MAX; ch++) {
    Serial.printf("  ch%-2d %lu\n", ch, static_cast<unsigned long>(st.perChannel[ch]));
  }
}

// Receives the camera map over USB: "camput <bytes> <crc32 hex>", then the file in ACKed chunks.
// Written to /cameras.tmp, CRC-checked, then renamed to /cameras.bin. Capture is not drained
// meanwhile (like a dump), so expect some dropped frames while it runs.
static void camPut(const String& arg) {
  static const size_t CHUNK = 1024;
  const int sp = arg.indexOf(' ');
  const unsigned long size = sp > 0 ? strtoul(arg.substring(0, sp).c_str(), nullptr, 10) : 0;
  const uint32_t wantCrc = sp > 0 ? strtoul(arg.substring(sp + 1).c_str(), nullptr, 16) : 0;
  if (size < 20 || size > 6UL * 1024UL * 1024UL) { Serial.println("usage: camput <bytes> <crc32 hex>   (use tools/push_cameras.py)"); return; }
  const LoggerStatus& ls = loggerStatus();
  if (!ls.fsOk) { Serial.println("CAMPUT FAIL filesystem not mounted"); return; }
  const unsigned long need = size + 256UL * 1024UL;
  if (ls.freeBytes < need) {
    Serial.printf("CAMPUT FAIL need %lu KB free, have %lu KB. Pull what you want to keep (tools/pull_session.py), then 'delete' old sessions.\n",
                  need / 1024, static_cast<unsigned long>(ls.freeBytes / 1024));
    return;
  }
  uint8_t* buf = static_cast<uint8_t*>(malloc(CHUNK));
  if (!buf) { Serial.println("CAMPUT FAIL out of memory"); return; }
  LittleFS.remove("/cameras.tmp");
  File f = LittleFS.open("/cameras.tmp", "w");
  if (!f) { free(buf); Serial.println("CAMPUT FAIL cannot create file"); return; }
  while (Serial.available()) Serial.read();   // drop a stray newline so it isn't taken as data
  Serial.printf("CAMPUT READY %u\n", static_cast<unsigned>(CHUNK));
  Serial.setTimeout(4000);
  uint32_t crc = 0; size_t off = 0; const char* error = nullptr;
  while (off < size) {
    const size_t n = min<size_t>(CHUNK, size - off);
    if (Serial.readBytes(buf, n) != n) { error = "timeout"; break; }
    if (f.write(buf, n) != n) { error = "write failed"; break; }
    crc = esp_rom_crc32_le(crc, buf, n);
    off += n;
    Serial.printf("ACK %lu\n", static_cast<unsigned long>(off));
    loopWatchdogFeed();
  }
  f.close(); free(buf);
  if (!error && crc != wantCrc) error = "crc mismatch";
  if (error) {
    LittleFS.remove("/cameras.tmp");
    Serial.printf("CAMPUT FAIL %s at byte %lu\n", error, static_cast<unsigned long>(off));
    return;
  }
  LittleFS.remove("/cameras.bin");
  if (!LittleFS.rename("/cameras.tmp", "/cameras.bin")) { Serial.println("CAMPUT FAIL rename"); return; }
  if (!camAlertReload()) { Serial.println("CAMPUT FAIL file saved but is not a valid camera map"); return; }
  loggerMeta("cam_map=updated");
  Serial.println("CAMPUT OK");
  camAlertPrint(Serial);
}

static void saveSettings() {
  Serial.println(settingsSave()?"[settings] saved":"[settings] SAVE FAILED; runtime values are volatile");
}
static bool percentArg(const String& arg) {
  if (arg.isEmpty() || arg.length()>3) return false;
  for (unsigned i=0;i<arg.length();i++) if (arg[i]<'0' || arg[i]>'9') return false;
  return arg.toInt()<=100;
}
static void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  const int sp = line.indexOf(' ');
  const String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  arg.trim();

  if (cmd == "help" || cmd == "?") commandsPrintHelp();
  else if (cmd == "info") boardInfoPrint(Serial);
  else if (cmd == "status") printStatus();
  else if (cmd == "stats") printStats();
  else if (cmd == "space") printSpace();
  else if (cmd=="crash") diagnosticsCrashStatus(Serial);
  else if (cmd=="heap-check") diagnosticsHeapCheck(Serial);
  else if (cmd=="panic-test") {
    if (arg!="yes") Serial.println("usage: panic-test yes (overwrites any saved coredump)");
    else {
      loggerMeta("panic_test=intentional");
      Serial.println("[panic] aborting intentionally; expect reboot and flash coredump"); Serial.flush();
      BreadcrumbScope stage(Crumb::PanicTest); abort();
    }
  }
  else if (cmd == "settings") {
    if (arg=="reset") {
      Serial.println(settingsReset()?"[settings] defaults saved":"[settings] RESET/SAVE FAILED");
      loggerMeta("settings=reset");
    } else if (!arg.isEmpty()) { Serial.println("usage: settings [reset]"); return; }
    settingsPrint(Serial);
  } else if (cmd == "mode") {
    if (arg!="research" && arg!="car") Serial.println("usage: mode research|car");
    else if (loggerSetMode(arg=="car"?LogMode::CAR:LogMode::RESEARCH)) {
      saveSettings(); Serial.printf("mode = %s\n",loggerModeName());
    } else Serial.println("[mode] session change failed; check status");
  } else if (cmd == "volume" || cmd == "brightness") {
    if (!percentArg(arg)) Serial.printf("usage: %s <0-100>\n",cmd.c_str());
    else {
      const uint8_t value=static_cast<uint8_t>(arg.toInt());
      if (cmd=="volume") uiSetVolume(value); else uiSetBrightness(value);
      saveSettings();
      char note[32]; snprintf(note,sizeof(note),"%s=%u",cmd.c_str(),value); loggerMeta(note);
      Serial.println(note);
    }
  } else if (cmd=="storage-test") loggerStorageTest(Serial);
  else if (cmd=="rotation") loggerRotationPlan(Serial);
  else if (cmd=="rotation-bench") loggerRotationBench(Serial);
  else if (cmd == "list") loggerList(Serial);
  else if (cmd == "dump") {
    if (arg.length() == 0 || !loggerDump(Serial, arg.c_str())) Serial.println("usage: dump <name>  (see: list)");
  } else if (cmd == "delete") {
    if (arg == "all yes") { loggerDeleteAll(); Serial.println("all sessions deleted"); }
    else if (arg == "all") Serial.println("this erases every capture. Type: delete all yes");
    else if (arg.length() && loggerDelete(arg.c_str())) Serial.println("deleted");
    else Serial.println("could not delete (missing, or it is the current session)");
  } else if (cmd == "mark") {
    loggerMark(arg.length() ? arg.c_str() : "MARK");
    uiBeep(Beep::MARK);
    uiFlashBlue();
    Serial.println("MARK recorded");
  } else if (cmd == "new") {
    loggerNewSession("serial");
    uiBeep(Beep::NEW_SESSION);
  } else if (cmd == "time") {
    if (arg.length() < 8) Serial.println("usage: time 2026-11-05T14:30:00Z");
    else { loggerSetClock(arg.c_str()); Serial.println("clock reference recorded in the session"); }
  } else if (cmd == "dwell") {
    const long v = arg.toInt();
    if (v < 20 || v > 5000) Serial.println("usage: dwell <20-5000>");
    else {
      snifferSetDwellMs(static_cast<uint16_t>(v));
      saveSettings();
      char note[40];
      snprintf(note, sizeof(note), "dwell_ms=%ld", v);
      loggerMeta(note);  // a settings change mid-session must be visible in the data
      Serial.printf("dwell = %ld ms\n", v);
    }
  } else if (cmd == "beacon") {
    snifferSetBeaconMinMs(static_cast<uint32_t>(arg.toInt()));
    char note[40];
    snprintf(note, sizeof(note), "beacon_min_ms=%lu", static_cast<unsigned long>(snifferBeaconMinMs()));
    loggerMeta(note);
    Serial.printf("beacon limit = %lu ms\n", static_cast<unsigned long>(snifferBeaconMinMs()));
  } else if (cmd == "data") {
    snifferSetDataMinMs(static_cast<uint32_t>(arg.toInt()));
    char note[40];
    snprintf(note, sizeof(note), "data_min_ms=%lu", static_cast<unsigned long>(snifferDataMinMs()));
    loggerMeta(note);
    Serial.printf("data limit = %lu ms%s\n", static_cast<unsigned long>(snifferDataMinMs()),
                  snifferDataMinMs() ? "" : " (data frames not logged)");
  } else if (cmd == "gps") {
    if (arg.startsWith("raw")) {
      gpsRaw(static_cast<uint16_t>(constrain(arg.substring(3).toInt() > 0 ? arg.substring(3).toInt() : 40, 1, 400)));
    } else if (arg.startsWith("send ")) {   // gps send PCAS10,3  (checksum added); module config, not saved here
      gpsSendCas(arg.substring(5).c_str());
      Serial.println("sent to GPS module");
    } else if (arg.startsWith("sim")) {
      String rest = arg.substring(3); rest.trim();
      if (rest == "off") { gpsSimOff(); Serial.println("gps simulation off"); }
      else {
        double v[4]; int count = 0; const char* p = rest.c_str(); char* end;
        while (count < 4) { v[count] = strtod(p, &end); if (end == p) break; p = end; count++; }
        if (count != 4 || v[0] < -90 || v[0] > 90 || v[1] < -180 || v[1] > 180 || v[2] < 0 || v[2] > 360 || v[3] < 0 || v[3] > 200)
          Serial.println("usage: gps sim <lat> <lon> <course 0-360> <mph>   |   gps sim off");
        else {
          gpsSim(v[0], v[1], static_cast<float>(v[3] * 0.44704), static_cast<float>(v[2]));
          loggerMeta("gps_sim=on");
          Serial.println("simulating a drive from there; watch the screen. 'gps sim off' to stop");
        }
      }
    } else gpsPrint(Serial);
  } else if (cmd == "knob") {
    if (arg.isEmpty()) controlsPrint(Serial);
    else {
      const int sp4 = arg.indexOf(' ');
      const String what = sp4 < 0 ? arg : arg.substring(0, sp4);
      const int n = sp4 < 0 ? 1 : arg.substring(sp4 + 1).toInt();
      if (!controlsSimulate(what, n)) Serial.println("usage: knob [cw|ccw [clicks] | press | long | mute]  (simulates the parts)");
      else Serial.println("ok");
    }
  } else if (cmd == "tft") {
    if (arg.isEmpty()) tftUiPrint(Serial);
    else if (arg == "auto" || arg == "on" || arg == "off") {
      const bool saved = tftUiSetMode(arg == "on" ? TftMode::ON : arg == "off" ? TftMode::OFF : TftMode::AUTO);
      if (!saved) Serial.println("[tft] could not save the setting; it applies until reboot");
      tftUiPrint(Serial);
    } else if (arg.startsWith("demo")) {
      String rest = arg.substring(4); rest.trim();
      const int sp3 = rest.indexOf(' ');
      const String which = sp3 < 0 ? rest : rest.substring(0, sp3);
      const long secs = sp3 < 0 ? 20 : rest.substring(sp3 + 1).toInt();
      if (!tftUiActive()) Serial.println("tft not running (try: tft on)");
      else if (!tftUiDemo(which.c_str(), static_cast<uint32_t>(constrain(secs, 1L, 600L))))
        Serial.println("usage: tft demo listening|camera|close|away|nodir|rf|follower|boot [seconds]  |  tft demo off");
      else Serial.println(which == "off" ? "demo off" : "demo screen up (sample data, marked DEMO)");
    } else if (arg.startsWith("utc")) {
      String rest = arg.substring(3); rest.trim();
      const float hours = rest.toFloat();
      if (rest.isEmpty() || hours < -12.0f || hours > 14.0f) Serial.println("usage: tft utc <-12..14> (hours from UTC, e.g. -5)");
      else {
        tftUiSetUtcOffsetMinutes(static_cast<int>(hours * 60.0f + (hours < 0 ? -0.5f : 0.5f)));
        Serial.printf("clock offset %.2f h\n", hours);
      }
    } else if (arg == "dump") tftUiDump(Serial);
    else if (arg == "bench") tftUiBench(Serial);
    else if (arg == "verify") tftUiVerify(Serial);
    else if (arg == "panel reset") { tftUiSetPanel("reset", 0); tftUiPrint(Serial); }
    else if (arg.startsWith("read ")) {
      const long reg = strtol(arg.substring(5).c_str(), nullptr, 16);
      uint32_t raw = 0;
      if (reg < 0 || reg > 0xFF) Serial.println("usage: tft read <hex register>, e.g. tft read 0b");
      else if (!tftUiReadReg(static_cast<uint8_t>(reg), raw)) Serial.println("tft not running (try: tft on)");
      else Serial.printf("tft read %02lX: raw %08lX  (1 dummy clock: %06lX, 8 dummy clocks: %06lX)\n", reg,
                         static_cast<unsigned long>(raw), static_cast<unsigned long>((raw >> 7) & 0xFFFFFF),
                         static_cast<unsigned long>(raw & 0xFFFFFF));
    }
    else if (arg == "freeze on" || arg == "freeze off") {
      tftUiFreeze(arg == "freeze on");
      Serial.println(tftUiFrozen() ? "screen frozen: no redraws until 'tft freeze off' (radio still running)" : "screen redrawing again");
    }
    else if (arg.startsWith("cmd ")) {
      // tft cmd <hex cmd> [hex data bytes...]  e.g. "tft cmd c6 05" (frame rate). Live only, not saved.
      uint8_t bytes[17];
      size_t n = 0;
      const char* p = arg.c_str() + 4;
      char* end = nullptr;
      while (*p && n < sizeof(bytes)) {
        const long v = strtol(p, &end, 16);
        if (end == p || v < 0 || v > 0xFF) break;
        bytes[n++] = static_cast<uint8_t>(v);
        p = end;
        while (*p == ' ') p++;
      }
      if (n == 0) Serial.println("usage: tft cmd <hex cmd> [hex bytes...]   (live panel tuning, not saved)");
      else if (!tftUiWriteCmd(bytes[0], bytes + 1, n - 1)) Serial.println("tft cmd failed (screen not running?)");
      else Serial.printf("tft cmd %02X sent with %u data byte(s)\n", bytes[0], static_cast<unsigned>(n - 1));
    }
    else if (arg == "chip ili" || arg == "chip st") {
      tftUiSetPanel("chip", arg == "chip st" ? 1 : 0);
      tftUiPrint(Serial);
    }
    else if (arg.startsWith("rot ") || arg.startsWith("bgr ") || arg.startsWith("inv ") || arg.startsWith("mhz ")) {
      const String key = arg.substring(0, 3);
      String rest = arg.substring(4); rest.trim();
      if (rest.isEmpty() || !tftUiSetPanel(key.c_str(), rest.toInt()))
        Serial.println("usage: tft rot 1|3 | tft bgr 0|1 | tft inv 0|1 | tft mhz 1..80 | tft panel reset");
      else tftUiPrint(Serial);
    }
    else Serial.println("usage: tft [auto|on|off | verify | demo <screen> [s] | utc <hours> | dump | rot|bgr|inv|mhz <v> | panel reset]");
  } else if (cmd == "cam") {
    if (arg == "near") camAlertNearby(Serial); else camAlertPrint(Serial);
  } else if (cmd == "camput") {
    camPut(arg);
  } else if (cmd == "detections") {
    detectorList(Serial);
  } else if (cmd == "alert-test") {
    detectorSelfTest(Serial);
  } else if (cmd == "led") {
    if (arg == "on") uiSetLedLevel(LedLevel::ON);
    else if (arg == "dim") uiSetLedLevel(LedLevel::DIM);
    else if (arg == "off") uiSetLedLevel(LedLevel::OFF);
    if (!arg.isEmpty() && arg != "on" && arg != "dim" && arg != "off") Serial.println("usage: led on|dim|off");
    static const char* const kLevels[] = {"off (no heartbeat; alerts dimmed)", "dim", "on (full)"};
    Serial.printf("led %s\n", kLevels[static_cast<uint8_t>(uiLedLevel())]);
    if (arg=="on" || arg=="dim" || arg=="off") {
      saveSettings(); char note[24]; snprintf(note,sizeof(note),"led=%s",arg.c_str()); loggerMeta(note);
    }
  } else if (cmd == "mute") {
    if (arg != "on" && arg != "off") Serial.println("usage: mute on|off");
    else {
      detectorSetMuted(arg == "on"); saveSettings();
      loggerMeta(arg=="on"?"mute=on":"mute=off");
      Serial.printf("detection beeps %s\n", arg == "on" ? "muted" : "on");
    }
  } else if (cmd == "ble") {
    if (arg != "on" && arg != "off") Serial.println("usage: ble on|off");
    else {
      snifferSetBleEnabled(arg == "on");
      saveSettings();
      loggerMeta(arg == "on" ? "ble=passive" : "ble=off");
      Serial.printf("BLE %s\n", snifferBleEnabled() ? "on" : "off");
    }
  } else if (cmd == "tone") {
    const int sp2 = arg.indexOf(' ');
    const long hz = arg.toInt();
    long ms = sp2 < 0 ? 3000 : arg.substring(sp2 + 1).toInt();
    if (hz < 20 || hz > 20000) Serial.println("usage: tone <20-20000 hz> [ms]");
    else {
      ms = constrain(ms, 1L, 10000L);
      uiTone(static_cast<uint16_t>(hz), static_cast<uint16_t>(ms));
      Serial.printf("tone %ld Hz for %ld ms on GPIO %d\n", hz, ms, PIN_BUZZER);
    }
  } else if (cmd == "format") {
    if (arg == "yes") {
      bool ok;
      {
        BreadcrumbScope stage(Crumb::Format);
        LoopWatchdogPause watchdogPause;
        ok = loggerFormat();
      } // re-subscribe before replying, on both success and failure
      Serial.println(ok ? "formatted" : "format FAILED");
    }
    else Serial.println("this erases every capture. Type: format yes");
  } else if (cmd == "wdt-test") {
    if (arg == "long") {
      loggerMeta("wdt_test=long");
      Serial.println("[wdt] 20 s unsubscribed-loop test; capture draining pauses");
      Serial.flush();
      {
        BreadcrumbScope stage(Crumb::WatchdogTest);
        LoopWatchdogPause watchdogPause;
        delay(20000);  // exceeds 15 s while only this task's subscription is paused
      }
      loggerMeta("wdt_test=long_done");
      Serial.println("[wdt] long test finished; loop watchdog re-armed (15 s)");
    } else if (arg != "yes") Serial.println("usage: wdt-test yes|long");
    else {
      loggerMeta("wdt_test=intentional");  // flush evidence before the test
      Serial.println("[wdt] stalling loop; expect a reboot in about 15 seconds");
      Serial.flush();
      loopWatchdogFeed();
      BreadcrumbScope stalled(Crumb::WatchdogTest);
      for (;;) delay(1000);  // other tasks run, but loop's watchdog is never fed
    }
  } else if (cmd == "reboot") {
    Serial.println("rebooting...");
    Serial.flush();
    ESP.restart();
  } else {
    Serial.println("unknown command - type: help");
  }
}

void commandsService() {
  BreadcrumbScope stage(Crumb::Commands);
  static String line;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      handleLine(line);
      line = "";
      return;
    }
    if (line.length() < 96) line += c;
  }
}
