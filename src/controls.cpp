// controls.cpp - see controls.h.
#include <Arduino.h>
#include <esp_attr.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#define KNOB_FN_ATTR inline __attribute__((always_inline))   // inlined into the IRAM interrupt handler
#define KNOB_TABLE_ATTR DRAM_ATTR
#include "knob_decode.h"
#include "controls.h"
#include "camalert.h"
#include "config.h"
#include "detector.h"
#include "logger.h"
#include "menu_logic.h"
#include "pins.h"
#include "settings.h"
#include "sniffer.h"
#include "tft_ui.h"
#include "ui.h"

static KnobDecoder gDecoder;
static volatile int32_t gSteps = 0;               // detents since the last service, written by the interrupt
static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
static MenuLogic gMenu;

static uint32_t gToastUntilMs = 0;
static char gToastText[16] = "";
static int8_t gToastPct = -1;
static uint32_t gSaveAtMs = 0;                    // settings are written once the knob has been still for a moment
static uint32_t gEvents = 0;

static void IRAM_ATTR onKnobEdge() {
  const int clk = gpio_get_level(static_cast<gpio_num_t>(PIN_KNOB_CLK));
  const int dt = gpio_get_level(static_cast<gpio_num_t>(PIN_KNOB_DT));
  const int step = gDecoder.feed(clk, dt);
  if (step) gSteps += KNOB_REVERSE ? -step : step;
}

void controlsBegin() {
  pinMode(PIN_KNOB_CLK, INPUT_PULLUP);
  pinMode(PIN_KNOB_DT, INPUT_PULLUP);
  pinMode(PIN_KNOB_SW, INPUT_PULLUP);
  pinMode(PIN_MUTE_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_KNOB_CLK), onKnobEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_KNOB_DT), onKnobEdge, CHANGE);
}

// ---- helpers --------------------------------------------------------------------------------

static void toast(const char* text, int pct, uint32_t now) {
  strlcpy(gToastText, text, sizeof(gToastText));
  gToastPct = static_cast<int8_t>(pct);
  gToastUntilMs = now + TOAST_MS;
}

static void saveLater(uint32_t now) { gSaveAtMs = now + 1500; }

static void syncMenuValues() {
  MenuValues& v = gMenu.values;
  v.volume = uiVolume();
  v.brightness = uiBrightness() < 10 ? 10 : uiBrightness();
  v.mode = loggerMode() == LogMode::CAR ? 1 : 0;
  v.ble = snifferBleEnabled() ? 1 : 0;
  v.utcHours = tftUiUtcOffsetMinutes() / 60;
}

static void applyLive() {
  uiSetVolume(static_cast<uint8_t>(gMenu.values.volume));
  uiSetBrightness(static_cast<uint8_t>(gMenu.values.brightness));
}

static void applyEvent(const MenuEvent& e, uint32_t now) {
  if (e.cancel) applyLive();                                  // an abandoned edit puts the old values back
  if (e.item == MI_VOLUME || e.item == MI_BRIGHTNESS) {
    applyLive();
    if (e.item == MI_VOLUME && !e.cancel && !detectorMuted()) uiTone(2400, 30);   // hear the new level
  }
  if (e.commit) {
    char note[32];
    switch (e.item) {
      case MI_VOLUME: snprintf(note, sizeof(note), "volume=%d", gMenu.values.volume); loggerMeta(note); break;
      case MI_BRIGHTNESS: snprintf(note, sizeof(note), "brightness=%d", gMenu.values.brightness); loggerMeta(note); break;
      case MI_MODE:
        if (loggerSetMode(gMenu.values.mode ? LogMode::CAR : LogMode::RESEARCH)) {
          snprintf(note, sizeof(note), "mode=%s", gMenu.values.mode ? "car" : "research"); loggerMeta(note);
        } else syncMenuValues();                              // the mode change failed: show the real one
        break;
      case MI_BLE:
        snifferSetBleEnabled(gMenu.values.ble != 0);
        loggerMeta(gMenu.values.ble ? "ble=passive" : "ble=off");
        break;
      case MI_UTC: tftUiSetUtcOffsetMinutes(gMenu.values.utcHours * 60); break;
      default: break;
    }
    saveLater(now - 1000);                                    // a confirmed choice is saved almost at once
  } else if (e.item == MI_VOLUME || e.item == MI_BRIGHTNESS) {
    saveLater(now);
  }
}

static void setMuted(bool muted, uint32_t now) {
  detectorSetMuted(muted);
  loggerMeta(muted ? "mute=on" : "mute=off");
  toast(muted ? "MUTED" : "Sound on", -1, now);
  if (!muted) uiBeep(Beep::MARK);                             // confirms the speaker works
  saveLater(now - 1000);
}

// ---- inputs ---------------------------------------------------------------------------------

struct Pad {
  uint8_t pin;
  bool raw = false, stable = false, longFired = false;
  uint32_t changedMs = 0, startMs = 0;
};
enum class Press : uint8_t { NONE, SHORT, LONG };
static Pad gKnobSw{PIN_KNOB_SW}, gMuteBtn{PIN_MUTE_BTN};
static Press gInjected[2] = {Press::NONE, Press::NONE};       // simulated presses: [0] knob, [1] mute

static Press poll(Pad& p, uint32_t now) {
  Press ev = Press::NONE;
  const bool raw = digitalRead(p.pin) == LOW;
  if (raw != p.raw) { p.raw = raw; p.changedMs = now; }
  if (now - p.changedMs >= BUTTON_DEBOUNCE_MS && raw != p.stable) {
    p.stable = raw;
    if (p.stable) { p.startMs = now; p.longFired = false; }
    else if (!p.longFired) ev = Press::SHORT;
  }
  if (p.stable && !p.longFired && now - p.startMs >= BUTTON_LONG_PRESS_MS) { p.longFired = true; ev = Press::LONG; }
  return ev;
}

bool controlsMenuOpen() { return gMenu.isOpen(); }
bool controlsBusy() { return gMenu.isOpen() || static_cast<int32_t>(gToastUntilMs - millis()) > 0; }

void controlsService() {
  const uint32_t now = millis();

  // A camera warning outranks the menu: close it (abandoning any edit) so the warning is on screen.
  const CamAlertView& cv = camAlertView();
  if (gMenu.isOpen() && cv.valid && now - cv.atMs < CAM_SHOW_MS) applyEvent(gMenu.close(), now);
  if (gMenu.isOpen()) applyEvent(gMenu.tick(now), now);

  int32_t steps = 0;
  portENTER_CRITICAL(&gMux);
  steps = gSteps;
  gSteps = 0;
  portEXIT_CRITICAL(&gMux);

  Press knob = poll(gKnobSw, now), mute = poll(gMuteBtn, now);
  if (gInjected[0] != Press::NONE) { knob = gInjected[0]; gInjected[0] = Press::NONE; }
  if (gInjected[1] != Press::NONE) { mute = gInjected[1]; gInjected[1] = Press::NONE; }

  if (steps) {
    gEvents++;
    if (gMenu.isOpen()) {
      applyEvent(gMenu.rotate(static_cast<int>(steps), now), now);
    } else {
      int v = static_cast<int>(uiVolume()) + static_cast<int>(steps) * KNOB_VOLUME_STEP;
      v = v < 0 ? 0 : (v > 100 ? 100 : v);
      uiSetVolume(static_cast<uint8_t>(v));
      toast("VOLUME", v, now);
      if (!detectorMuted() && v > 0) uiTone(2400, 30);
      saveLater(now);
    }
  }
  if (knob == Press::SHORT) {
    gEvents++;
    if (!gMenu.isOpen()) { syncMenuValues(); applyEvent(gMenu.open(now), now); }
    else applyEvent(gMenu.press(now), now);
  } else if (knob == Press::LONG) {
    gEvents++;
    if (gMenu.isOpen()) applyEvent(gMenu.longPress(now), now);
  }
  if (mute == Press::SHORT) { gEvents++; setMuted(!detectorMuted(), now); }

  if (gSaveAtMs && static_cast<int32_t>(now - gSaveAtMs) >= 0) {
    gSaveAtMs = 0;
    if (!settingsSave()) Serial.println("[controls] could not save the settings");
  }
}

void controlsFillModel(TftModel& m, uint32_t now) {
  if (static_cast<int32_t>(gToastUntilMs - now) > 0) {
    m.toast.active = true;
    strlcpy(m.toast.text, gToastText, sizeof(m.toast.text));
    m.toast.pct = gToastPct;
  }
  if (gMenu.isOpen()) {
    m.menu.sel = static_cast<int8_t>(gMenu.selected());
    m.menu.editing = gMenu.editing();
    for (int i = 0; i < TFT_MENU_ROWS; i++) {
      strlcpy(m.menu.label[i], MenuLogic::label(i), sizeof(m.menu.label[i]));
      gMenu.valueText(i, m.menu.value[i], sizeof(m.menu.value[i]));
    }
  }
}

bool controlsSimulate(const String& what, int count) {
  if (count < 1) count = 1;
  if (count > 100) count = 100;
  if (what == "cw" || what == "ccw") {
    portENTER_CRITICAL(&gMux);
    gSteps += (what == "cw" ? count : -count);
    portEXIT_CRITICAL(&gMux);
  } else if (what == "press") gInjected[0] = Press::SHORT;
  else if (what == "long") gInjected[0] = Press::LONG;
  else if (what == "mute") gInjected[1] = Press::SHORT;
  else return false;
  return true;
}

void controlsPrint(Print& out) {
  out.printf("controls: knob CLK %d DT %d SW %d, mute button %d; levels now clk=%d dt=%d sw=%d mute=%d (1 = not pressed)\n",
             PIN_KNOB_CLK, PIN_KNOB_DT, PIN_KNOB_SW, PIN_MUTE_BTN, digitalRead(PIN_KNOB_CLK), digitalRead(PIN_KNOB_DT),
             digitalRead(PIN_KNOB_SW), digitalRead(PIN_MUTE_BTN));
  out.printf("  menu %s, %lu events since boot, volume %u, brightness %u, mute %s\n", gMenu.isOpen() ? "open" : "closed",
             static_cast<unsigned long>(gEvents), uiVolume(), uiBrightness(), detectorMuted() ? "on" : "off");
}

// ---- hooks used by the other files ----
