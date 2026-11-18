// main.cpp - ESP32-S3 RF detector, Phase 1: receive-only capture logger.
//
// setup(): boot check -> UI -> filesystem/session -> start passive Wi-Fi capture.
// loop():  move frames from the sniffer queue to the log, handle the button, serial
//          commands, and refresh the OLED. Nothing here ever transmits.
#include <Arduino.h>
#include "board_info.h"
#include "breadcrumbs.h"
#include "commands.h"
#include "config.h"
#include "csv_writer.h"
#include "detector.h"
#include "logger.h"
#include "loop_watchdog.h"
#include "sniffer.h"
#include "ui.h"
#include "settings.h"
#include "watchlist.h"
#include "watchlist_data.h"
#include "diagnostics.h"
#include "camalert.h"
#include "gps.h"
#include "tft_ui.h"
#include "controls.h"

SET_LOOP_TASK_STACK_SIZE(LOOP_STACK_BYTES);

// Text for the session's META row. Anything that changes how the data should be read
// (settings, firmware, clock quality) goes here.
static void buildMeta(char* out, size_t n, const char* reason) {
  char clk[60];
  if (loggerClockString()[0]) {
    snprintf(clk, sizeof(clk), "host_utc=%s set_at_ms=%lu", loggerClockString(),
             static_cast<unsigned long>(loggerClockSetMs()));
  } else {
    snprintf(clk, sizeof(clk), "host_utc=unset");
  }
  snprintf(out, n, "unit=%s fw=%s reason=%s reset=%s capture=wifi_ble_passive ch=%d-%d dwell_ms=%u "
                   "beacon_min_ms=%lu data_min_ms=%lu probe_ie=1 ble=%s ble_slice_ms=%u ble_min_ms=%u rules=%u %s",
           UNIT_ID, FW_VERSION, reason, boardResetReason(), HOP_CHANNEL_MIN, HOP_CHANNEL_MAX, snifferDwellMs(),
           static_cast<unsigned long>(snifferBeaconMinMs()),
           static_cast<unsigned long>(snifferDataMinMs()), snifferBleEnabled() ? "passive" : "off",
           BLE_SLICE_MS, BLE_LOG_MIN_MS, static_cast<unsigned>(kWlRuleCount), clk);
}

void setup() {
  breadcrumbsBegin();
  diagnosticsBegin();
  Serial.setRxBufferSize(8192);  // before begin(): lets tools/push_cameras.py send 1 KB chunks safely
  Serial.begin(SERIAL_BAUD);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);  // only matters for native-USB serial
  boardInfoBegin();
  breadcrumbsPrintPrevious(Serial);
  diagnosticsPrintPrevious(Serial);

  uiBegin();
  controlsBegin();  // knob and mute button
  tftUiBegin();    // colour screen: looks for the panel, does nothing if it is not there
  settingsBegin(); // apply persisted UI/radio/logging choices before capture starts

  char chip[22], flash[22], psram[22];
  const bool boardOk = boardInfoPrint(Serial, chip, flash, psram);
  uiShow3(boardOk ? "BOARD OK" : "CHECK BOARD", flash, psram, -1, !boardOk);
  tftUiSplash(boardOk ? "BOARD OK" : "CHECK BOARD");
  uiBeep(boardOk ? Beep::BOOT : Beep::ERROR);
  const uint32_t splashEnd = millis() + 2500;
  while (millis() < splashEnd) { uiService(); delay(5); }  // let the beep play out

  gpsBegin();  // starts listening; finds the module's baud rate in the background
  loggerSetRowHook(detectorProcess);
  const bool logOk = loggerBegin(buildMeta);
  camAlertBegin();  // needs the filesystem; loads /cameras.bin if present
  breadcrumbsLogPrevious();
  diagnosticsLogPrevious();
  snifferBegin();
  Serial.printf("watchlist: %u rules (all unverified), BLE %s\n", static_cast<unsigned>(kWlRuleCount),
                snifferBleEnabled() ? "passive scan on" : "off");
  Serial.println(CSV_WRITER_DONE ? "csv writer: implemented" : "csv writer: STUB - implement csvWriteRow() in src/csv_writer.cpp");
  Serial.println("listening (receive-only). Type 'help' for commands.");
  if (!logOk) uiBeep(Beep::ERROR);
  loopWatchdogBegin();
  char wdtMeta[40];
  snprintf(wdtMeta, sizeof(wdtMeta), "loop_wdt_ms=%lu",
           static_cast<unsigned long>(LOOP_WDT_TIMEOUT_MS));
  loggerMeta(wdtMeta);
}

static void updateDisplayAndLed() {
  static uint32_t lastDrawMs=0;
  static bool wasError=false;
  const uint32_t now=millis();
  if (now-lastDrawMs<DISPLAY_REFRESH_MS) return;
  lastDrawMs=now;
  const LoggerStatus& ls=loggerStatus();
  const bool error=!ls.fsOk || ls.writeError;
  if (error && !wasError) uiBeep(Beep::ERROR);
  wasError=error;
  // GPS status for the home screen: "NO GPS" (nothing wired), "GPS --" (searching), "GPS 9" (fix, sats).
  char gps[10];
  if (gpsSimActive()) snprintf(gps,sizeof(gps),"GPS SIM");
  else if (!gpsModuleTalking()) snprintf(gps,sizeof(gps),"NO GPS");
  else if (!gpsUsable()) snprintf(gps,sizeof(gps),"GPS --");
  else snprintf(gps,sizeof(gps),"GPS %u",gpsFix().sats);
  uiSetGpsStatus(gps);
  // A mapped camera ahead is the most time-critical thing to show while driving.
  const CamAlertView& cv=camAlertView();
  if (cv.valid && now-cv.atMs<CAM_SHOW_MS) {
    uiSetLed(cv.close?LedMode::DETECT_HIGH:LedMode::DETECT_LIKELY);
    uiShowCamera(camMakerName(cv.maker),cv.distM,cv.close,cv.facing,detectorMuted());
    return;
  }
  const DetectView& dv=detectorView();
  if (dv.valid && now-dv.atMs<DETECT_SHOW_MS) {
    uiSetLed(dv.tier>=WL_HIGH?LedMode::DETECT_HIGH:dv.tier>=WL_LIKELY?LedMode::DETECT_LIKELY:LedMode::DETECT_POSSIBLE);
    uiShowAlert(dv,detectorMuted(),error || ls.full);
  } else {
    uiSetLed(error?LedMode::ERROR:ls.full?LedMode::WARN:LedMode::OK);
    uiShowHome(loggerModeName(),snifferBleEnabled(),detectorMuted(),ls.fsOk,ls.writeError,
               ls.full,ls.freeBytes,loggerEstimatedMinutes());
  }
}

void loop() {
  breadcrumbsLoopStarted();
  loggerService();
  gpsService();
  camAlertService();

  switch (uiService()) {
    case ButtonEvent::SHORT_PRESS:
      loggerMark("MARK");
      uiBeep(Beep::MARK);
      uiFlashBlue();
      Serial.printf("[btn] MARK in %s\n", loggerStatus().session);
      break;
    case ButtonEvent::LONG_PRESS:
      loggerNewSession("button");
      uiBeep(Beep::NEW_SESSION);
      break;
    case ButtonEvent::NONE:
      break;
  }

  commandsService();
  updateDisplayAndLed();
  controlsService();
  tftUiService();
  loopWatchdogFeed();
  breadcrumbsLoopProgress();
}
