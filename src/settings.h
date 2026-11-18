#pragma once
#include <Arduino.h>
void settingsBegin(); // after uiBegin(), before logger/radio startup
bool settingsSave();  // capture runtime values; write only when changed
bool settingsReset(); // only app settings; never filesystem/session numbering
void settingsPrint(Print& out);
