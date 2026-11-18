// ui.h - OLED, buzzer, RGB LED and the single button.
#pragma once
#include <Arduino.h>

enum class ButtonEvent : uint8_t { NONE, SHORT_PRESS, LONG_PRESS };
enum class Beep : uint8_t { BOOT, MARK, NEW_SESSION, ERROR };
enum class LedMode : uint8_t { OFF, OK, WARN, ERROR, DETECT_POSSIBLE, DETECT_LIKELY, DETECT_HIGH };

void uiBegin();                                  // pins, buzzer, OLED (also scans I2C)
ButtonEvent uiService();                         // call every loop(): beeps, LED, button
// Big title (<= 12 chars) + two bold lines (<= 18 chars); optional signal bar on line 3.
void uiShow3(const char* big, const char* line2, const char* line3, int barPct = -1, bool invertTitle = false);
void uiBeep(Beep b);                             // non-blocking
void uiTone(uint16_t hz, uint16_t ms);           // one plain tone, for testing the speaker
void uiBeepCategory(uint8_t cat);                // detection alert, pattern per WL_CAT_*
void uiSetLed(LedMode m);
enum class LedLevel : uint8_t { OFF, DIM, ON };  // OFF = no heartbeat; alerts still show dimmed
void uiSetLedLevel(LedLevel l);
LedLevel uiLedLevel();
void uiFlashBlue(uint16_t ms = 150);
bool uiDisplayFound();
void uiSetVolume(uint8_t percent);
uint8_t uiVolume();
void uiSetBrightness(uint8_t percent); // OLED contrast and colour-screen backlight
uint8_t uiBrightness();
void uiSetGpsStatus(const char* text);           // small text on the home screen, e.g. "GPS 9"
// Mapped-camera warning from the GPS + camera map (not a radio detection).
void uiShowCamera(const char* maker, uint16_t distM, bool close, bool facing, bool muted);
struct DetectView;
void uiShowHome(const char* mode, bool ble, bool muted, bool fsOk, bool writeError,
                bool pressure, uint32_t freeBytes, uint32_t estimatedMinutes);
void uiShowAlert(const DetectView& view, bool muted, bool storageIssue = false);
