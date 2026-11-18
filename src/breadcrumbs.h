#pragma once
#include <Arduino.h>

enum class Crumb : uint32_t {
  Boot, Idle, Drain, Detector, Flush, FileOpen, FileWrite, FileClose,
  Space, UiService, UiDraw, Commands, Dump, WatchdogTest,
  WifiChannel, WifiDwell, BlePause, BleSlice, BleResume, Format, Rotation, Settings, PanicTest, RotateScan, RotateDelete, Count
};
enum class CrumbTask { Loop, Radio };
void breadcrumbsBegin(); // first operation in setup: snapshot before overwriting RTC
void breadcrumbsPrintPrevious(Print& out);
void breadcrumbsLogPrevious(); // after loggerBegin(), before radio startup
void breadcrumbsFlushStarted();
void breadcrumbsFlushSucceeded();
void breadcrumbsFreeSpace(uint32_t bytes);
void breadcrumbsLoopStarted();
void breadcrumbsLoopProgress();

class BreadcrumbScope {
 public:
  explicit BreadcrumbScope(Crumb stage, CrumbTask task = CrumbTask::Loop);
  ~BreadcrumbScope();
  BreadcrumbScope(const BreadcrumbScope&) = delete;
  BreadcrumbScope& operator=(const BreadcrumbScope&) = delete;
 private:
  CrumbTask task_;
  uint32_t previous_, previousMs_;
};
