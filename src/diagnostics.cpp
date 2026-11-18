#include "diagnostics.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_core_dump.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "logger.h"

#if !CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH || !CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#error "This release needs the installed SDK's flash/ELF coredump support"
#endif
namespace {
constexpr uint32_t Magic=0x464C4331;
struct FlushRecord { uint32_t magic,context,ms,fileBytes,bufferBytes; char session[16]; uint32_t check; };
RTC_NOINIT_ATTR volatile FlushRecord retained;
FlushRecord previous={};
bool abnormal=false;
FlushContext context=FlushContext::SERVICE;
const char* names[]={"service","new_session","mark","meta","dump"};
uint32_t checksum(const FlushRecord& r) {
  uint32_t v=Magic^r.context^r.ms^(r.fileBytes*31u)^(r.bufferBytes*65599u);
  for (unsigned i=0;i<sizeof(r.session);i++) v=(v^static_cast<uint8_t>(r.session[i]))*16777619u;
  return v;
}
void emit(Print* out,const char* text) {
  if (out) { out->print("[diag] "); out->println(text); }
  else loggerMetaDeferred(text);
}
void number(Print* out,const char* key,uint32_t value) {
  char text[48]; snprintf(text,sizeof(text),"%s=%lu",key,static_cast<unsigned long>(value)); emit(out,text);
}
void previousReport(Print* out) {
  if (!abnormal) return;
  if (previous.magic!=Magic || previous.context>=static_cast<uint32_t>(FlushContext::COUNT) ||
      !previous.check || previous.check!=checksum(previous) || previous.session[15]) {
    emit(out,"flush_diag=unavailable"); return;
  }
  char line[48]; snprintf(line,sizeof(line),"flush_context=%s",names[previous.context]); emit(out,line);
  snprintf(line,sizeof(line),"flush_session=%s",previous.session); emit(out,line);
  number(out,"flush_diag_ms",previous.ms); number(out,"flush_file_bytes",previous.fileBytes);
  number(out,"flush_buffer_bytes",previous.bufferBytes);
}
void health(Print* out) {
  number(out,"loop_stack_bytes",getArduinoLoopTaskStackSize());
  // ESP-IDF's Xtensa port uses uint8_t StackType_t: this value is bytes, not words.
  number(out,"loop_stack_hwm_bytes",uxTaskGetStackHighWaterMark(nullptr));
  number(out,"heap_int_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  number(out,"heap_int_min",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  number(out,"heap_int_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  number(out,"heap_psram_free",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
}
FlushContextScope::FlushContextScope(FlushContext c):previous_(context) { context=c; }
FlushContextScope::~FlushContextScope() { context=previous_; }
void diagnosticsBegin() {
  previous.magic=retained.magic; previous.context=retained.context; previous.ms=retained.ms;
  previous.fileBytes=retained.fileBytes; previous.bufferBytes=retained.bufferBytes;
  for (unsigned i=0;i<sizeof(previous.session);i++) previous.session[i]=retained.session[i];
  previous.check=retained.check;
  const auto r=esp_reset_reason();
  abnormal=r==ESP_RST_PANIC || r==ESP_RST_TASK_WDT || r==ESP_RST_INT_WDT || r==ESP_RST_WDT;
  retained.magic=0;
}
void diagnosticsStampFlush(const char* session,uint32_t fileBytes,uint32_t bufferBytes) {
  FlushRecord r={}; r.context=static_cast<uint32_t>(context); r.ms=millis();
  r.fileBytes=fileBytes; r.bufferBytes=bufferBytes; strlcpy(r.session,session,sizeof(r.session));
  r.check=checksum(r);
  retained.magic=0; __atomic_thread_fence(__ATOMIC_SEQ_CST);
  retained.context=r.context; retained.ms=r.ms; retained.fileBytes=r.fileBytes; retained.bufferBytes=r.bufferBytes;
  for (unsigned i=0;i<sizeof(r.session);i++) retained.session[i]=r.session[i];
  retained.check=r.check; __atomic_thread_fence(__ATOMIC_SEQ_CST); retained.magic=Magic;
}
void diagnosticsPrintPrevious(Print& out) { previousReport(&out); }
void diagnosticsLogPrevious() { if (abnormal) { previousReport(nullptr); loggerFlush(); } }
void diagnosticsPrintHealth(Print& out) { health(&out); }
void diagnosticsLogHealth() { health(nullptr); }
void diagnosticsHeapCheck(Print& out) {
  const bool ok=heap_caps_check_integrity_all(false);
  out.printf("[heap] integrity=%s\n",ok?"OK":"FAILED");
  loggerMeta(ok?"heap_integrity=OK":"heap_integrity=FAILED");
}
void diagnosticsCrashStatus(Print& out) {
  const esp_partition_t* p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_COREDUMP,nullptr);
  if (!p) { out.println("[crash] coredump partition missing; upload the partition table too"); return; }
  const esp_err_t err=esp_core_dump_image_check(); size_t addr=0,size=0;
  const bool valid=err==ESP_OK && esp_core_dump_image_get(&addr,&size)==ESP_OK;
  out.printf("[crash] flash=0x%08lx capacity=%lu image=%s status=%s bytes=%lu\n",
      static_cast<unsigned long>(p->address),static_cast<unsigned long>(p->size),
      valid?"valid":"empty-or-invalid",esp_err_to_name(err),static_cast<unsigned long>(size));
  const auto* fs=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"spiffs");
  if (fs) out.printf("[crash] LittleFS offset=0x%08lx size=0x%08lx\n",
      static_cast<unsigned long>(fs->address),static_cast<unsigned long>(fs->size));
}
