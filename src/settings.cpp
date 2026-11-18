#include "settings.h"
#include <Preferences.h>
#include "breadcrumbs.h"
#include "config.h"
#include "detector.h"
#include "logger.h"
#include "sniffer.h"
#include "ui.h"
#include "settings_codec.h"

static Preferences prefs;
static bool ready=false;
static bool savedValid=false;
static uint8_t saved[12]={};
static bool apply(const uint8_t* b) {
  detectorSetMuted(b[5]); uiSetVolume(b[6]); uiSetBrightness(b[9]);
  uiSetLedLevel(static_cast<LedLevel>(b[10]));
  snifferSetDwellMs(b[3]|(b[4]<<8)); snifferSetBleEnabled(b[8]);
  return loggerSetMode(b[7] ? LogMode::CAR : LogMode::RESEARCH);
}
void settingsBegin() {
  BreadcrumbScope stage(Crumb::Settings);
  ready=prefs.begin("rf_settings",false);
  uint8_t b[12]; settingsDefaultBlob(b);
  if (ready && prefs.getBytesLength("v1")==sizeof(b)) {
    uint8_t candidate[12];
    if (prefs.getBytes("v1",candidate,sizeof(candidate))==sizeof(candidate) && settingsBlobValid(candidate)) {
      memcpy(b,candidate,sizeof(b));
      savedValid=true;
    } else Serial.println("[settings] invalid saved values; using defaults");
  }
  memcpy(saved,b,sizeof(b)); apply(b);
  Serial.printf("[settings] NVS %s\n",ready ? "ready" : "unavailable; changes are volatile");
}
bool settingsSave() {
  BreadcrumbScope stage(Crumb::Settings);
  uint8_t b[12]={'R','F',1,static_cast<uint8_t>(snifferDwellMs()&255),
      static_cast<uint8_t>(snifferDwellMs()>>8),static_cast<uint8_t>(detectorMuted()),uiVolume(),
      static_cast<uint8_t>(loggerMode()==LogMode::CAR),static_cast<uint8_t>(snifferBleEnabled()),
      uiBrightness(),static_cast<uint8_t>(uiLedLevel()),0};
  if (!ready) return false;
  if (savedValid && memcmp(b,saved,sizeof(b))==0) return true;
  if (prefs.putBytes("v1",b,sizeof(b))!=sizeof(b)) return false;
  memcpy(saved,b,sizeof(b)); savedValid=true; return true;
}
bool settingsReset() { uint8_t b[12]; settingsDefaultBlob(b); return apply(b) && settingsSave(); }
void settingsPrint(Print& out) {
  const char* levels[]={"off","dim","on"};
  out.printf("mode=%s mute=%s volume=%u brightness=%u led=%s dwell_ms=%u ble=%s\n",
      loggerModeName(),detectorMuted()?"on":"off",uiVolume(),uiBrightness(),
      levels[static_cast<unsigned>(uiLedLevel())],snifferDwellMs(),snifferBleEnabled()?"on":"off");
  out.printf("settings_nvs=%s\n",ready?"ready":"unavailable");
}
