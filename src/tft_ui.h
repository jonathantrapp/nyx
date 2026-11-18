// tft_ui.h - connects the colour screen to the rest of the firmware: picks the screen, fills the
// TftModel from live state (GPS, camera map, detector, logger), renders it and pushes the frame.
// The OLED keeps working exactly as before; this is an addition, not a replacement.
#pragma once
#include <Arduino.h>

enum class TftMode : uint8_t { AUTO = 0, ON = 1, OFF = 2 };   // auto: use the screen if its ID reads back

void tftUiBegin();                           // after uiBegin(); loads saved mode, looks for the panel
void tftUiSplash(const char* note);          // boot screen, shown at once (blocks ~50 ms)
void tftUiService();                         // every loop(): pump the push, redraw on a timer
bool tftUiActive();
void tftUiSetBrightness(uint8_t percent);    // backlight

bool tftUiSetMode(TftMode mode);             // saved; starts or stops the driver now
bool tftUiDemo(const char* screen, uint32_t seconds);   // "listening|camera|close|rf|follower|boot"; "off" ends it
// Panel fixes without reflashing: key "rot" (1|3), "bgr" (0|1), "inv" (0|1), "mhz" (1..80), or "reset"
// (back to config.h). Saved, and the panel is re-initialised at once. False if key/value is invalid.
bool tftUiSetPanel(const char* key, long value);
void tftUiFreeze(bool on);                   // "tft freeze on|off": stop redrawing (diagnostics; not saved)
bool tftUiFrozen();
bool tftUiWriteCmd(uint8_t cmd, const uint8_t* data, size_t n);   // "tft cmd <hex> [bytes]": live panel tuning, not saved
bool tftUiReadReg(uint8_t cmd, uint32_t& raw);  // "tft read <hex>": 32 raw bits back from the panel
int tftUiUtcOffsetMinutes();
void tftUiSetUtcOffsetMinutes(int minutes);  // saved; the clock on screen is GPS time plus this
void tftUiPrint(Print& out);                 // "tft" command
void tftUiBench(Print& out);                 // "tft bench": time drawing each screen (does not touch the panel)
void tftUiVerify(Print& out);                // "tft verify": temporary write/read speed sweep, resumes rendering
void tftUiDump(Print& out);                  // "tft dump": the current frame as base64, for tools/tft_shot.py
