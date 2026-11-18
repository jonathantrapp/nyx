#pragma once
#include <Arduino.h>
enum class FlushContext : uint32_t { SERVICE, NEW_SESSION, MARK, META, DUMP, COUNT };
class FlushContextScope {
 public:
  explicit FlushContextScope(FlushContext context);
  ~FlushContextScope();
  FlushContextScope(const FlushContextScope&)=delete;
  FlushContextScope& operator=(const FlushContextScope&)=delete;
 private: FlushContext previous_;
};
void diagnosticsBegin(); // snapshot old RTC flush context, after breadcrumbsBegin()
void diagnosticsStampFlush(const char* session,uint32_t fileBytes,uint32_t bufferBytes);
void diagnosticsPrintPrevious(Print& out);
void diagnosticsLogPrevious();
void diagnosticsPrintHealth(Print& out);
void diagnosticsLogHealth(); // append META without recursive flush
void diagnosticsHeapCheck(Print& out);
void diagnosticsCrashStatus(Print& out);
