// config.h - tunable numbers for the Phase 1 capture logger.
#pragma once

#ifndef UNIT_ID
#define UNIT_ID "A"            // overridden per board in platformio.ini
#endif
#define FW_VERSION "0.5.19"
#define LOOP_STACK_BYTES (16 * 1024)

#define SERIAL_BAUD 921600     // must match monitor_speed in platformio.ini

// Recover when loop() makes no progress for this long. USB dumps feed on progress.
#define LOOP_WDT_TIMEOUT_MS 15000

// ---- Wi-Fi channel hopping ----
#define HOP_CHANNEL_MIN 1
#define HOP_CHANNEL_MAX 13     // 12-13 are legal elsewhere; harmless to listen to
#define DEFAULT_DWELL_MS 250   // time spent on each channel (runtime: "dwell <ms>")

// Beacons repeat ~10x/second per access point. Logging all of them would fill flash in
// minutes, so each beaconing MAC is logged at most once per this interval. 0 = log all.
// Probe requests are NEVER rate-limited: their timing is the thing we want to study.
#define DEFAULT_BEACON_MIN_MS 5000

// Data frames are the traffic of devices already connected to a network (a patrol car's
// laptop talking to its in-car router, a body cam syncing). They come in thousands per
// second, so each transmitter is logged at most once per this interval, with a count of
// how many frames it sent since its last row. 0 = don't log data frames at all.
// (runtime: "data <ms>")
#define DEFAULT_DATA_MIN_MS 30000

// ---- Capture pipeline ----
#define SNIFFER_QUEUE_LEN 512            // frames waiting between Wi-Fi task and loop()
#define LOG_BUFFER_BYTES (128 * 1024)    // RAM buffer (PSRAM if present) between flushes
#define LOG_FLUSH_INTERVAL_MS 5000       // worst case you lose this much on power loss
#define LOG_FLUSH_REPORT_INTERVAL_MS (10UL * 60UL * 1000UL)
#define LOG_FLUSH_HIGH_WATER (LOG_BUFFER_BYTES / 2)
#define FS_MIN_FREE_BYTES (128 * 1024)   // error indicator; rotation starts well before this
#ifndef ROTATE_LOW_BYTES               // overridable with -D only for bench-top rotation tests
#define ROTATE_LOW_BYTES (1024UL * 1024UL)
#endif
#ifndef ROTATE_TARGET_BYTES
#define ROTATE_TARGET_BYTES (1536UL * 1024UL)
#endif
#define SESSION_MAX_BYTES (1024UL * 1024UL) // about 15 minutes at 4 MiB/hour
// ---- Rotation / free-space work (0.3.10): background, ONE bounded operation per loop pass ----
#define SPACE_QUERY_INTERVAL_MS 60000UL            // real free-space query at most this often...
#define SPACE_QUERY_NEAR_MS 15000UL                // ...or this often when near the rotation target
#define SPACE_QUERY_WRITTEN_BYTES (128UL * 1024UL) // ...or after this many bytes were flushed
#define SPACE_NEAR_MARGIN_BYTES (512UL * 1024UL)
#define ROTATE_OP_GAP_MS 250UL                     // pause after a delete / space query so draining runs
#define ROTATE_SCAN_GAP_MS 100UL                   // pause between classification slices (low duty)
#define ROTATE_SCAN_GAP_URGENT_MS 20UL             // ...when rotation is waiting on a classification
#define ROTATE_SCAN_SLICE_MS 12UL                  // time budget of one classification slice
#define ROTATE_SCAN_CHUNK 1024                     // bytes per read inside a slice
#define ROTATE_FAIL_BACKOFF_MS 30000UL             // retry a failed delete of the same file after this
#define SUMMARY_INTERVAL_MS 60000UL

// ---- UI ----
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 1000
#define DISPLAY_REFRESH_MS 250
#define SESSION_NAME_LEN 16

// ---- Colour screen (ST7789 default; ILI9341 remains selectable; see pins.h) ----
#define TFT_DEFAULT_CHIP 1       // TFT_CHIP_ST7789; a saved setting/conclusive ID still wins
#define TFT_SPI_HZ 10000000      // conservative jumper-wire default; qualify faster writes with tft verify
#define TFT_ROTATION 1           // 1 = landscape, USB end to the left of the picture; 3 = upside down
#define TFT_BGR 0                // matches unit A's ST7789; tft bgr can override it
#define TFT_INVERT 0             // unit A's ST7789 checked by eye 2026-11-13: inversion on = negative picture
#define TFT_FRAME_MS_ALERT 150   // camera screen: ask for a new frame this often (the task skips if it is still busy)
#define TFT_FRAME_MS_IDLE 400    // listening and the other screens

// ---- Knob and mute button ----
#define KNOB_REVERSE 0           // 1 if turning the knob clockwise lowers the volume
#define KNOB_VOLUME_STEP 5       // percent per click when the knob is turned outside the menu
#define TOAST_MS 1500            // "VOLUME 60" / "MUTED" overlay time

// ---- GPS camera alerts (thresholds live in src/camera_map.h) ----
#define CAM_SHOW_MS 4000       // camera screen stays up this long after the camera was last ahead

// ---- BLE (passive scan, time-shared with Wi-Fi) ----
#ifndef ENABLE_BLE
#define ENABLE_BLE 1           // build flag; "ble off" turns it off at runtime
#endif
#define BLE_SLICE_MS 1000      // BLE listening time after each full Wi-Fi channel sweep
#define BLE_LOG_MIN_MS 20000   // log each BLE address at most this often

// ---- Detection ----
#define DETECT_MAX_TRACKED 64
#define DETECT_COOLDOWN_MS (5UL * 60UL * 1000UL)  // one alert per device per 5 minutes
#define DETECT_SHOW_MS 15000   // detection screen stays up this long after last sighting
// Which categories may beep (only Likely/High ever beep). Trackers stay silent until
// there is "this tag is following me" logic; other people's AirTags are everywhere.
#define BEEP_CAMERA 1
#define BEEP_ACOUSTIC 1
#define BEEP_BODYCAM 1
#define BEEP_FLEET_MODEM 1
#define BEEP_GEAR 1
#define BEEP_TACTICAL 1
#define BEEP_TRACKER 0
// Trackers also stay off the screen/LED (still logged, still in "detections"):
// a crowded building has dozens of other people's tags and they would take over the display.
#define SCREEN_TRACKER 0
