// board_info.h - identifies the board (chip, real flash size, PSRAM) and why it last reset.
#pragma once
#include <Arduino.h>

void boardInfoBegin();            // capture the reset reason once, early in setup()
const char* boardResetReason();   // "poweron", "BROWNOUT", "panic", ...

// Prints the board report to `out` (boot, and the "info" command). Returns true only if
// the board looks like a genuine N16R8: 16 MB physical flash and ~8 MB PSRAM.
// If given, line2..line4 (22 chars each) receive short text for the OLED splash.
bool boardInfoPrint(Print& out, char* line2 = nullptr, char* line3 = nullptr, char* line4 = nullptr);
