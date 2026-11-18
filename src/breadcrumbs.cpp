#include "breadcrumbs.h"
#include <esp_attr.h>
#include <esp_system.h>
#include "logger.h"

namespace {
constexpr uint32_t Magic = 0x43524D32;
struct Slot { uint32_t stage, ms, check; };
struct Storage { uint32_t start, done, free, flags, check; };
struct Retained { uint32_t magic; Slot loop, radio, progress; Storage storage; };
RTC_NOINIT_ATTR volatile Retained retained; // no initializer: survive warm reset
Retained previous;
bool reportPrevious = false;
const char* const names[] = {
  "boot", "idle", "sniffer_drain", "detector", "logger_flush", "file_open",
  "file_write", "file_close", "storage_space", "ui_service", "ui_draw",
  "commands", "usb_dump", "wdt_test", "wifi_channel", "wifi_dwell",
  "ble_pause", "ble_slice", "ble_resume", "format", "rotation", "settings", "panic_test", "rotate_scan", "rotate_delete"
};
static_assert(sizeof(names)/sizeof(names[0]) == static_cast<size_t>(Crumb::Count));
void fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
uint32_t stamp(uint32_t a, uint32_t b) { return Magic ^ a ^ (b * 16777619u); }
uint32_t storageStamp(const Storage& s) {
  return stamp(s.start, s.done) ^ (s.free * 31u) ^ (s.flags * 65599u);
}
bool valid(const Slot& s) {
  return s.check && s.stage < static_cast<uint32_t>(Crumb::Count) && s.check == stamp(s.stage,s.ms);
}
volatile Slot& slot(CrumbTask task) {
  return task == CrumbTask::Loop ? retained.loop : retained.radio;
}
Slot read(const volatile Slot& s) { return {s.stage,s.ms,s.check}; }
Storage readStorage() {
  const auto& s = retained.storage;
  return {s.start,s.done,s.free,s.flags,s.check};
}
void write(volatile Slot& s, uint32_t stage, uint32_t ms) {
  // Commit marker last. A reset interrupting this update yields an invalid slot.
  s.check = 0; fence();
  s.stage = stage; s.ms = ms; fence(); s.check = stamp(stage,ms);
}
void writeStorage(Storage s) {
  retained.storage.check = 0; fence();
  retained.storage.start=s.start; retained.storage.done=s.done;
  retained.storage.free=s.free; retained.storage.flags=s.flags;
  fence(); retained.storage.check=storageStamp(s);
}
void emit(Print* out, const char* text) {
  if (out) { out->print("[previous boot] "); out->println(text); }
  else loggerMeta(text);
}
void number(Print* out, const char* key, uint32_t value) {
  char line[64]; snprintf(line,sizeof(line),"%s=%lu",key,static_cast<unsigned long>(value));
  emit(out,line);
}
void report(Print* out) {
  if (!reportPrevious) return;
  if (previous.magic != Magic) { emit(out,"crumb=unavailable"); return; }
  char line[64];
  const Slot slots[] = {previous.loop, previous.radio};
  const char* keys[] = {"loop","radio"};
  for (unsigned i=0;i<2;i++) {
    snprintf(line,sizeof(line),"crumb.%s=%s",keys[i],valid(slots[i]) ? names[slots[i].stage] : "invalid");
    emit(out,line);
    if (valid(slots[i])) {
      snprintf(line,sizeof(line),"crumb.%s_enter_ms",keys[i]); number(out,line,slots[i].ms);
    }
  }
  const Storage& s=previous.storage;
  if (valid(previous.progress)) number(out,"crumb.loop_ok_ms",previous.progress.ms);
  if (!s.check || s.check != storageStamp(s)) { emit(out,"crumb.storage=invalid"); return; }
  number(out,"crumb.storage_flags",s.flags);
  if (s.flags & 1) number(out,"crumb.flush_start_ms",s.start);
  if (s.flags & 2) number(out,"crumb.flush_ok_ms",s.done);
  if (s.flags & 4) number(out,"crumb.free_bytes",s.free);
}
} // namespace

void breadcrumbsBegin() {
  previous.magic=retained.magic;
  previous.loop=read(retained.loop); previous.radio=read(retained.radio);
  previous.progress=read(retained.progress);
  previous.storage=readStorage();
  const auto reason=esp_reset_reason();
  reportPrevious=reason==ESP_RST_TASK_WDT || reason==ESP_RST_PANIC ||
                 reason==ESP_RST_INT_WDT || reason==ESP_RST_WDT;
  retained.magic=0; fence();
  write(retained.loop,static_cast<uint32_t>(Crumb::Boot),millis());
  write(retained.radio,static_cast<uint32_t>(Crumb::Idle),millis());
  retained.progress.check=0; // no completed loop yet
  writeStorage({0,0,0,0,0}); fence(); retained.magic=Magic;
}
void breadcrumbsPrintPrevious(Print& out) { report(&out); }
void breadcrumbsLogPrevious() { report(nullptr); }
void breadcrumbsFlushStarted() {
  Storage s=readStorage(); s.start=millis(); s.flags|=1; writeStorage(s);
}
void breadcrumbsFlushSucceeded() {
  Storage s=readStorage(); s.done=millis(); s.flags|=2; writeStorage(s);
}
void breadcrumbsFreeSpace(uint32_t bytes) {
  Storage s=readStorage(); s.free=bytes; s.flags|=4; writeStorage(s);
}
void breadcrumbsLoopStarted() {
  write(retained.loop,static_cast<uint32_t>(Crumb::Idle),millis());
}
void breadcrumbsLoopProgress() {
  write(retained.progress,static_cast<uint32_t>(Crumb::Idle),millis());
}
BreadcrumbScope::BreadcrumbScope(Crumb stage, CrumbTask task):task_(task) {
  Slot old=read(slot(task));
  previous_=valid(old) ? old.stage : static_cast<uint32_t>(Crumb::Idle);
  previousMs_=valid(old) ? old.ms : millis();
  write(slot(task),static_cast<uint32_t>(stage),millis());
}
BreadcrumbScope::~BreadcrumbScope() { write(slot(task_),previous_,previousMs_); }
