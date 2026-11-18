#include "loop_watchdog.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"

static TaskHandle_t gWatchedTask = nullptr;
static bool gPaused = false;

static esp_task_wdt_config_t watchdogConfig(uint32_t timeoutMs) {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = timeoutMs;
  // Preserve the SDK's configured idle-task checks.
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  cfg.idle_core_mask |= 1u << 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  cfg.idle_core_mask |= 1u << 1;
#endif
  cfg.trigger_panic = true;  // this SDK's panic handler prints and reboots
  return cfg;
}

void loopWatchdogBegin() {
  const esp_task_wdt_config_t cfg = watchdogConfig(LOOP_WDT_TIMEOUT_MS);
  const bool initialized = esp_task_wdt_status(nullptr) != ESP_ERR_INVALID_STATE;
  const esp_err_t err = initialized ? esp_task_wdt_reconfigure(&cfg)
                                    : esp_task_wdt_init(&cfg);
  ESP_ERROR_CHECK(err);

  if (esp_task_wdt_status(nullptr) != ESP_OK) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
  }
  gWatchedTask = xTaskGetCurrentTaskHandle();
  gPaused = false;
  loopWatchdogFeed();
  Serial.printf("[wdt] loop task armed (%lu ms)\n",
                static_cast<unsigned long>(LOOP_WDT_TIMEOUT_MS));
}

void loopWatchdogFeed() {
  // Another task must never hide a stalled loop by feeding on its behalf.
  if (gWatchedTask && !gPaused && xTaskGetCurrentTaskHandle() == gWatchedTask) {
    ESP_ERROR_CHECK(esp_task_wdt_reset());
  }
}

LoopWatchdogPause::LoopWatchdogPause() {
  if (gWatchedTask && !gPaused && xTaskGetCurrentTaskHandle() == gWatchedTask) {
    ESP_ERROR_CHECK(esp_task_wdt_delete(nullptr));
    gPaused = true;
    resume_ = true;
  }
}

LoopWatchdogPause::~LoopWatchdogPause() {
  if (resume_) {
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    gPaused = false;
  }
}
