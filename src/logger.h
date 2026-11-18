// logger.h - session files on LittleFS, with RAM buffering and safe flushing.
#pragma once
#include <Arduino.h>
#include "capture.h"
#include "config.h"
enum class LogMode : uint8_t { RESEARCH, CAR };
LogMode loggerMode();
const char* loggerModeName();
bool loggerSetMode(LogMode mode); // a mode change starts a fresh session when mounted
void loggerCountDetection(uint8_t category);
void loggerStorageTest(Print& out); // non-destructive policy/cache checks
void loggerRotationPlan(Print& out); // read-only: prints the cached index, never touches files
void loggerRotationBench(Print& out); // read-only timing of classification + one simulated pass

struct LoggerStatus {
  bool fsOk;            // LittleFS mounted
  bool full;            // reserve pressure; no longer a permanent logging-stop latch
  bool writeError;      // last flush failed (usually: disk full)
  char session[SESSION_NAME_LEN];  // e.g. "s0003"
  uint32_t rows;        // all CSV rows buffered this session (including control/summary)
  uint32_t marks;       // MARK presses this session
  uint32_t bufferOverflows;
  size_t bufferBytes;   // bytes waiting in RAM
  uint32_t freeBytes;
  uint32_t totalBytes;
  uint32_t flushMaxMs;   // slowest completed loggerFlush() this boot (no space query/rotation since 0.3.10)
  uint32_t flushMaxAtMs; // millis at the START of that call; not reset by a new session
  uint32_t flushLastMs, flushOpenMs, flushWriteMs, flushCloseMs;
  uint32_t flushSpaceMs;  // duration of the LAST real free-space query (runs outside the flush)
  uint32_t flushWindowMaxMs, closeWindowMaxMs, spaceWindowMaxMs; // spaceWindowMaxMs = real queries only
  uint32_t fileBytes, rotatedFiles, rotationErrors, appleCollapsed, writeErrors;
  // Background rotation work (scan slice, delete, space query, index, NVS write): each is ONE call.
  uint32_t rotationMaxMs, rotationWindowMaxMs;       // slowest single operation since boot / 10-min window
  const char* rotationMaxPhase;                      // "scan" "remove" "print" "space" "index" "nvs"; "" if none yet
  // Slowest whole delete since boot, split into its steps (0.3.10b). deleteMaxNumber = session number.
  uint32_t deleteMaxMs, deleteMaxRemoveMs, deleteMaxNvsMs, deleteMaxPrintMs, deleteMaxNumber, deleteWindowMaxMs;
  const char* rotationWindowPhase;
  uint32_t scanBytes, classifiedFiles, unclassified; // bytes read to classify old files / files / still unknown
  uint32_t spaceQueries, nvsWrites;
};

// Fills `out` with the session's META note (unit, firmware, scan settings...). Provided
// by main so the logger doesn't need to know about the sniffer.
typedef void (*MetaProvider)(char* out, size_t outLen, const char* reason);

bool loggerBegin(MetaProvider meta);
void loggerService();                       // call every loop(): drain, buffer, flush
void loggerFlush();                         // force buffer -> flash now
bool loggerNewSession(const char* reason);
void loggerMark(const char* note);
void loggerMeta(const char* note);          // free-text META row (settings changes etc.)
void loggerMetaDeferred(const char* note);  // diagnostics batch; caller flushes once
void loggerGps(const char* text);           // GPS row: "lat=.. lon=.." or a camera-map event; no flush
void loggerAppendRow(const CaptureRow& row); // any extra row (the detector's DETECT rows)
// Called for every row drained from the radios, before it is logged.
void loggerSetRowHook(void (*hook)(const CaptureRow& row));
void loggerSetClock(const char* hostUtc);   // host-provided UTC string, recorded as META
const LoggerStatus& loggerStatus();
uint32_t loggerEstimatedMinutes(); // recent rate -> time until rotation; UINT32_MAX = learning

// Serial-command helpers
void loggerList(Print& out);
bool loggerDump(Print& out, const char* name);
bool loggerDelete(const char* name);
void loggerDeleteAll();
bool loggerFormat();
const char* loggerClockString();            // "" if never set
uint32_t loggerClockSetMs();
