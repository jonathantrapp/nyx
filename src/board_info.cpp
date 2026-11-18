// board_info.cpp - the "is this really an N16R8?" check from the build plan.
#include "board_info.h"
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_system.h>
#include "config.h"

static char gResetReason[16] = "?";

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_USB:       return "usb";
    default:                return "other";
  }
}

void boardInfoBegin() { strlcpy(gResetReason, resetReasonName(esp_reset_reason()), sizeof(gResetReason)); }
const char* boardResetReason() { return gResetReason; }

bool boardInfoPrint(Print& out, char* line2, char* line3, char* line4) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  uint32_t physFlash = 0;
  esp_flash_get_physical_size(nullptr, &physFlash);  // asks the flash chip itself (JEDEC ID)
  const uint32_t cfgFlash = ESP.getFlashChipSize();   // what the firmware image was built for
  const uint32_t psram = ESP.getPsramSize();

  out.println();
  out.println("==== ESP32 RF detector - Phase 1 capture logger ====");
  out.printf("firmware %s  unit %s\n", FW_VERSION, UNIT_ID);
  out.printf("chip:   %s rev %d, %d core(s) @ %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(), info.cores,
             static_cast<unsigned long>(getCpuFrequencyMhz()));
  out.printf("flash:  %lu MB physical, %lu MB configured\n", static_cast<unsigned long>(physFlash >> 20),
             static_cast<unsigned long>(cfgFlash >> 20));
  out.printf("psram:  %lu MB (%s)\n", static_cast<unsigned long>(psram >> 20), psramFound() ? "found" : "NOT FOUND");
  out.printf("heap:   %lu KB free internal\n", static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
  out.printf("reset:  %s\n", gResetReason);

  const bool flashOk = physFlash >= (16u << 20);
  const bool psramOk = psram >= (7u << 20);
  out.printf("N16R8 check: flash %s, psram %s\n", flashOk ? "OK" : "FAIL (expected 16 MB)",
             psramOk ? "OK" : "FAIL (expected 8 MB)");
  if (line2) snprintf(line2, 22, "%s rev%d", ESP.getChipModel(), ESP.getChipRevision());
  if (line3) snprintf(line3, 22, "Flash %luMB %s", static_cast<unsigned long>(physFlash >> 20), flashOk ? "OK" : "BAD");
  if (line4) snprintf(line4, 22, "PSRAM %luMB %s", static_cast<unsigned long>(psram >> 20), psramOk ? "OK" : "BAD");
  return flashOk && psramOk;
}
