// controls.h - the volume/menu knob, the mute button, and the settings menu behind them.
// Turn = volume (with an on-screen bar). Press = open the menu. Mute button = mute on/off.
// In the menu: turn to move, press to edit, turn to change, press to confirm, hold to leave.
// With nothing wired the pins read "not pressed" and nothing happens.
#pragma once
#include <Arduino.h>
#include "tft_model.h"

void controlsBegin();                          // pins and interrupts
void controlsService();                        // every loop(): buttons, menu timeout, saving
bool controlsMenuOpen();
bool controlsBusy();                           // menu open or an overlay showing: the screen should refresh quickly
void controlsFillModel(TftModel& m, uint32_t now);   // menu rows and the toast, for the colour screen
// Bench testing without the parts: "knob cw|ccw|press|long|mute [count]".
bool controlsSimulate(const String& what, int count);
void controlsPrint(Print& out);
