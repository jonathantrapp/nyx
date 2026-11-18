// logger.cpp - buffered capture/control logging, bounded sessions and retention.
#include "logger.h"
#include "breadcrumbs.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <esp_rom_crc.h>
#include <esp_littlefs.h>
#include "csv_writer.h"
#include "ram_buffer.h"
#include "sniffer.h"
#include "loop_watchdog.h"
#include "storage_policy.h"
#include "rotation_policy.h"
#include "summary_writer.h"
#include "diagnostics.h"
#include "storage_estimate.h"

static RamBuffer gBuf;
static Preferences gPrefs;
static LoggerStatus gSt = {};
static MetaProvider gMeta = nullptr;
static char gPath[24] = "";
static char gDumpPath[24] = ""; // pin a requested export while its pre-dump flush rotates
static uint32_t gLastFlushMs=0, gLastFlushReportMs=0;
static char gClock[40] = "";
static uint32_t gClockSetMs=0;
static void (*gRowHook)(const CaptureRow&) = nullptr;
static LogMode gMode=LogMode::RESEARCH;
static AddressSet gApple, gUnique;
static MinuteSummary gMinute;
static uint32_t gHeardStart[4]={};
static bool gCurrentProtected=false;
static bool gProtRecorded=false;   // the current session's "protected" state is already in the NVS table
static uint32_t gCurrentNumber=0;
// Rotation state (0.3.10). The index lives in RAM, is built once at boot, and is then
// maintained incrementally; classification comes from the NVS table, not from file contents.
struct SessionFile { String path; uint32_t number; uint32_t size; Protect state; uint32_t skipUntilMs; };
static std::vector<SessionFile> gFiles;
static bool gIndexReady=false;
static ProtectionTable gTable;
static uint32_t gWatermark=0;      // sessions >= this are plain unless the table says otherwise
static unsigned gUnknown=0;        // entries in gFiles still UNKNOWN (need one background scan)
static SpaceCache gSpace;
static bool gRotating=false, gBgHold=false;
static uint32_t gBgNextMs=0, gNoCandidateLogMs=0;
struct ScanState { bool active; uint32_t number, offset; CsvProtectionScan scan; };
static ScanState gScan = {};
static StorageEstimate gEstimate;

static void recordPhase(const char* phase,uint32_t ms) {
  if (ms>gSt.rotationMaxMs) { gSt.rotationMaxMs=ms; gSt.rotationMaxPhase=phase; }
  if (ms>gSt.rotationWindowMaxMs) { gSt.rotationWindowMaxMs=ms; gSt.rotationWindowPhase=phase; }
}
static void applyFree() { // publish the cached estimate; no filesystem call
  gSt.totalBytes=gSpace.total(); gSt.freeBytes=gSpace.free();
  gSt.full=gSpace.valid() && gSt.freeBytes<FS_MIN_FREE_BYTES;
}
// The ONLY place that asks the filesystem for free space. esp_littlefs_info walks every block
// (hundreds of ms and growing with stored data), and Arduino's totalBytes()+usedBytes() would
// walk twice. One call, never inside loggerFlush(). A failed query keeps the previous estimate.
static void updateSpace() {
  BreadcrumbScope stage(Crumb::Space);
  const uint32_t started=millis(); size_t total=0,used=0;
  if (esp_littlefs_info("spiffs",&total,&used)!=ESP_OK || !total) return;
  const uint32_t ms=millis()-started;
  gSpace.query(static_cast<uint32_t>(total),static_cast<uint32_t>(used),millis());
  gSt.flushSpaceMs=ms; gSt.spaceWindowMaxMs=std::max(gSt.spaceWindowMaxMs,ms); gSt.spaceQueries++;
  recordPhase("space",ms);
  applyFree(); breadcrumbsFreeSpace(gSt.freeBytes);
}
static bool appendRow(const CaptureRow& r) {
  const size_t start=gBuf.length();
  gBuf.clearOverflow(); csvWriteRow(gBuf,r);
  if (gBuf.overflowed()) { gBuf.rewind(start); gSt.bufferOverflows++; return false; }
  if (gBuf.length()==start) return false;
  gSt.rows++;
  if (r.type==FrameType::MARK || (r.type==FrameType::DETECT && !unprotectedDetectExtra(r.extra))) gCurrentProtected=true;
  return true;
}
static void appendNote(FrameType type,const char* note) {
  CaptureRow r={}; r.ms=millis(); r.type=type; strlcpy(r.ssid,note,sizeof(r.ssid)); appendRow(r);
}
static void appendMeta(const char* note) {
  const size_t n=strlen(note);
  for (size_t i=0;i<n || i==0;i+=32) {
    char piece[33]; strlcpy(piece,note+i,sizeof(piece)); appendNote(FrameType::META,piece);
    if (!n) break;
  }
}
static void snapshotHeard(uint32_t* values) {
  const auto& s=snifferStats();
  values[0]=s.seen; values[1]=s.dataSeen; values[2]=s.bleSeen; values[3]=s.dropped;
}
static void resetMinute() {
  gMinute={}; gMinute.startMs=millis(); snapshotHeard(gHeardStart); gUnique.clear();
}
static bool trackerAdvert(const CaptureRow& r) {
  if (r.type!=FrameType::BLE_ADV) return false;
  if (r.findMyType) return true;
  for (unsigned i=0;i<r.bleSvcCount;i++) {
    const uint16_t v=r.bleSvc16[i];
    if (v==0xFD44 || v==0xFCB2 || v==0xFEED || v==0xFEEC || v==0xFD5A || v==0xFEAA) return true;
  }
  return false; // advertised tracker-family leads, never a following-person claim
}
static bool collapsibleApple(const CaptureRow& r) {
  return r.type==FrameType::BLE_ADV && r.bleHasCompany && r.bleCompany==0x004C &&
         !(r.findMyType && r.findMySeparated);
}
static void observe(const CaptureRow& r) {
  int type=-1;
  if (r.type==FrameType::PROBE_REQ) type=0;
  else if (r.type==FrameType::BEACON) type=1;
  else if (r.type==FrameType::DATA) type=2;
  else if (r.type==FrameType::BLE_ADV) type=3;
  if (type<0) return;
  gMinute.rows[type]++;
  if (gUnique.add(addressKey(r.mac,static_cast<uint8_t>(type)))==AddressSet::NEW) gMinute.unique[type]++;
  if (r.type==FrameType::BLE_ADV && r.bleHasCompany && r.bleCompany==0x004C) {
    gMinute.appleRows++;
    if (gUnique.add(addressKey(r.mac,4))==AddressSet::NEW) gMinute.appleUnique++;
  }
  if (trackerAdvert(r)) {
    gMinute.trackerRows++;
    if (gUnique.add(addressKey(r.mac,5))==AddressSet::NEW) gMinute.trackerUnique++;
  }
}
static void emitSummary(bool partial) {
  if (!gSt.fsOk || !gPath[0]) return;
  const uint32_t now=millis();
  if (now==gMinute.startMs) return;
  uint32_t heard[4]; snapshotHeard(heard);
  gMinute.durationMs=now-gMinute.startMs; gMinute.partial=partial;
  for (unsigned i=0;i<3;i++) gMinute.heard[i]=heard[i]-gHeardStart[i];
  gMinute.dropped=heard[3]-gHeardStart[3]; gMinute.freeBytes=gSt.freeBytes;
  gMinute.uniqueCapped=gUnique.capped(); gMinute.appleCacheCapped=gApple.capped();
  const size_t start=gBuf.length(); gBuf.clearOverflow(); csvWriteSummary(gBuf,gMinute,now);
  if (gBuf.overflowed()) { gBuf.rewind(start); gSt.bufferOverflows++; return; }
  gSt.rows++; resetMinute();
}

// ---- Rotation (0.3.10) ------------------------------------------------------------------
// Protection is recorded when a MARK / non-tracker DETECT row is buffered (see
// recordProtectedCurrent), never discovered by reading closed files. Sessions written by
// older firmware are UNKNOWN and get ONE chunked scan in the background. Rotation runs from
// loggerService() as ONE bounded operation per loop pass (scan slice, delete, or space query),
// never inside loggerFlush(), and needs no loop-watchdog pause (each operation is < 1 s).
static SessionFile* findFile(uint32_t number) {
  for (auto& f:gFiles) if (f.number==number) return &f;
  return nullptr;
}
static void setState(SessionFile& f,Protect s) {
  if (f.state==Protect::UNKNOWN && s!=Protect::UNKNOWN && gUnknown) gUnknown--;
  else if (f.state!=Protect::UNKNOWN && s==Protect::UNKNOWN) gUnknown++;
  f.state=s;
}
static void recountUnknown() {
  gUnknown=0; for (const auto& f:gFiles) if (f.state==Protect::UNKNOWN) gUnknown++;
}
static uint32_t gLastNvsMs=0; // duration of the most recent table save (for the delete breakdown)
static bool saveTable() {
  const uint32_t started=millis(); bool ok=true;
  if (gTable.bytes()) ok=gPrefs.putBytes("ptab",gTable.data(),gTable.bytes())==gTable.bytes();
  else gPrefs.remove("ptab");
  gLastNvsMs=millis()-started;
  gSt.nvsWrites++; recordPhase("nvs",gLastNvsMs);
  return ok;
}
// Raise the watermark: every session below it that has no table record becomes UNKNOWN again
// (and is re-scanned by content). The safe fallback whenever a record could not be stored.
static void bumpWatermark(uint32_t wm) {
  if (wm<=gWatermark) return;
  gWatermark=wm; gPrefs.putUInt("pwm",wm);
  for (auto& f:gFiles)
    if (f.state==Protect::PLAIN && f.number<gWatermark && f.number!=gCurrentNumber && !gTable.has(f.number))
      setState(f,Protect::UNKNOWN);
}
static bool recordState(uint32_t number,Protect s) {
  if (!gTable.set(number,s)) {
    gTable.keepIf([](uint32_t n) { return findFile(n)!=nullptr; }); // forget deleted sessions
    if (!gTable.set(number,s)) return false;
  }
  return saveTable();
}
// Called at the start of a flush that is about to write protected rows, BEFORE they reach
// flash, so a reset right after the write can never leave a protected file looking plain.
static void recordProtectedCurrent() {
  gProtRecorded=true;
  if (!recordState(gCurrentNumber,Protect::PROTECTED)) {
    gSt.rotationErrors++;
    bumpWatermark(gCurrentNumber+1); // after a reboot this file is UNKNOWN and gets scanned
  }
  if (SessionFile* f=findFile(gCurrentNumber)) setState(*f,Protect::PROTECTED);
}
static void dropFileAt(size_t i) {
  const uint32_t number=gFiles[i].number;
  if (gFiles[i].state==Protect::UNKNOWN && gUnknown) gUnknown--;
  gFiles.erase(gFiles.begin()+i);
  if (gTable.remove(number)) saveTable();
  if (gScan.active && gScan.number==number) gScan.active=false;
}
static bool excludedPath(const String& p) { return p==gPath || p==gDumpPath; }
// Boot-time only (setup, not the loop): one directory listing, then the table decides.
static void buildIndex() {
  const uint32_t started=millis();
  gFiles.clear(); gScan.active=false;
  uint32_t maxNumber=0; bool any=false;
  File root=LittleFS.open("/");
  for (File f=root.openNextFile();f;f=root.openNextFile()) {
    if (f.isDirectory()) continue;
    String name=f.name(); if (name.startsWith("/")) name.remove(0,1);
    uint32_t number;
    if (!sessionNumber(name.c_str(),number)) continue;
    gFiles.push_back({"/"+name,number,static_cast<uint32_t>(f.size()),Protect::UNKNOWN,0});
    any=true; if (number>maxNumber) maxNumber=number;
  }
  root.close();
  bool tableOk=true;
  // Protection rule version: 2 = tier=possible_low DETECT rows no longer protect (0.3.12). When the
  // rule changes, forget old verdicts so every stored session is re-classified once by content.
  const uint32_t RULE_VER=2;
  const bool migrate=gPrefs.getUInt("prv",0)<RULE_VER;
  if (migrate) { gPrefs.remove("ptab"); gPrefs.remove("pwm"); }
  const size_t len=gPrefs.getBytesLength("ptab");
  gTable.clear();
  if (len) {
    uint8_t blob[ProtectionTable::CAP*sizeof(ProtectionTable::Rec)];
    if (len>sizeof(blob) || gPrefs.getBytes("ptab",blob,len)!=len || !gTable.load(blob,len)) {
      gTable.clear(); tableOk=false;
    }
  }
  const uint32_t floorWm=std::max(gPrefs.getUInt("nextSess",1),any?maxNumber+1:0);
  gWatermark=gPrefs.isKey("pwm")?gPrefs.getUInt("pwm",0):floorWm; // first 0.3.10 boot: all existing = unknown
  if (!gPrefs.isKey("pwm") || !tableOk) { // an unreadable table must not turn unknown sessions plain
    gWatermark=std::max(gWatermark,floorWm); gPrefs.putUInt("pwm",gWatermark);
  }
  if (migrate && gPrefs.isKey("pwm")) gPrefs.putUInt("prv",RULE_VER); // only once the new watermark is stored
  const size_t dropped=gTable.keepIf([](uint32_t n) { return findFile(n)!=nullptr; });
  for (auto& f:gFiles) {
    const Protect p=gTable.get(f.number);
    f.state=p!=Protect::UNKNOWN?p:unlistedState(gWatermark,f.number);
  }
  recountUnknown(); gIndexReady=true;
  if (dropped || !tableOk) saveTable();
  recordPhase("index",millis()-started);
}

static void scanFinish(SessionFile& f,Protect s,bool persist) {
  setState(f,s); gScan.active=false; gSt.classifiedFiles++;
  if (persist && !recordState(f.number,s)) gSt.rotationErrors++; // RAM state is still right this boot
}
// One bounded slice: reopen, seek, read up to ROTATE_SCAN_SLICE_MS, close. State survives in gScan.
static void scanSlice() {
  BreadcrumbScope stage(Crumb::RotateScan);
  const uint32_t t0=millis();
  if (!gScan.active) {
    size_t idx;
    if (!pickScanTarget(gFiles.size(),[](size_t i) {
          const SessionFile& f=gFiles[i];
          return RotCandidate{f.number,f.state,excludedPath(f.path)};
        },idx)) return;
    gScan.active=true; gScan.number=gFiles[idx].number; gScan.offset=0; gScan.scan=CsvProtectionScan();
  }
  SessionFile* f=findFile(gScan.number);
  if (!f) { gScan.active=false; return; }
  File fh=LittleFS.open(f->path,FILE_READ);
  if (!fh) {
    if (!LittleFS.exists(f->path)) { for (size_t i=0;i<gFiles.size();i++) if (&gFiles[i]==f) { dropFileAt(i); break; } }
    else scanFinish(*f,Protect::PROTECTED,false); // unreadable: conservative, not persisted
    recordPhase("scan",millis()-t0); return;
  }
  static uint8_t buf[ROTATE_SCAN_CHUNK];
  const uint32_t size=fh.size();
  bool done=false,persist=true; Protect result=Protect::PLAIN;
  if (gScan.offset>size || (gScan.offset && !fh.seek(gScan.offset))) { result=Protect::PROTECTED; persist=false; done=true; }
  while (!done) {
    if (gScan.offset>=size) { result=gScan.scan.unsafeAtEof()?Protect::PROTECTED:Protect::PLAIN; done=true; break; }
    if (millis()-t0>=ROTATE_SCAN_SLICE_MS) break;
    const size_t n=fh.read(buf,std::min<size_t>(sizeof(buf),size-gScan.offset));
    if (!n) { result=Protect::PROTECTED; persist=false; done=true; break; }
    gScan.offset+=n; gSt.scanBytes+=n;
    if (gScan.scan.feed(buf,n)) { result=Protect::PROTECTED; done=true; }
  }
  fh.close();
  if (done) scanFinish(*f,result,persist);
  recordPhase("scan",millis()-t0);
}
// Delete exactly one file. ~0.4 s each; the caller pauses afterwards so draining/UI run.
static void deleteOne(size_t i) {
  BreadcrumbScope stage(Crumb::RotateDelete);
  const uint32_t t0=millis();
  const String path=gFiles[i].path; const uint32_t number=gFiles[i].number;
  const uint32_t size=gFiles[i].size; const Protect st=gFiles[i].state;
  // 0.3.10b: time each step on its own. A 2.0 s delete was seen twice with no way to tell whether
  // the file removal, the NVS table save, or the serial print was responsible.
  uint32_t removeMs=0,nvsMs=0,printMs=0;
  const bool removed=LittleFS.remove(path);
  removeMs=millis()-t0; recordPhase("remove",removeMs);
  if (removed) {
    gLastNvsMs=0; dropFileAt(i); nvsMs=gLastNvsMs;       // dropFileAt saves the table if it had a record
    char note[48]; snprintf(note,sizeof(note),"rotated=%s",path.c_str()+1); appendMeta(note);
    const uint32_t p0=millis();
    Serial.printf("[log] %s%s\n",note,st==Protect::PROTECTED?" (protected; no plain sessions left)":"");
    printMs=millis()-p0; recordPhase("print",printMs);
    gSt.rotatedFiles++; gSpace.freed(blockRound(size)); applyFree();
  } else {
    gSt.rotationErrors++;
    gFiles[i].skipUntilMs=(millis()+ROTATE_FAIL_BACKOFF_MS)|1u; // do not hammer the same file
  }
  const uint32_t total=millis()-t0;
  if (total>gSt.deleteMaxMs) {
    gSt.deleteMaxMs=total; gSt.deleteMaxRemoveMs=removeMs; gSt.deleteMaxNvsMs=nvsMs;
    gSt.deleteMaxPrintMs=printMs; gSt.deleteMaxNumber=number;
  }
  gSt.deleteWindowMaxMs=std::max(gSt.deleteWindowMaxMs,total);
}
static bool backoffActive(const SessionFile& f,uint32_t now) {
  return f.skipUntilMs && static_cast<int32_t>(now-f.skipUntilMs)<0;
}
static Pick pickDeletion(size_t& index) {
  const uint32_t now=millis();
  return pickVictim(gFiles.size(),[now](size_t i) {
    const SessionFile& f=gFiles[i];
    return RotCandidate{f.number,f.state,excludedPath(f.path) || backoffActive(f,now)};
  },index);
}
static bool spaceNear() { return gSt.freeBytes<ROTATE_TARGET_BYTES+SPACE_NEAR_MARGIN_BYTES; }
static bool spaceDue(uint32_t now) {
  return gSpace.due(now,spaceNear()?SPACE_QUERY_NEAR_MS:SPACE_QUERY_INTERVAL_MS,SPACE_QUERY_WRITTEN_BYTES);
}
// Called once per loggerService(). Does AT MOST ONE heavy operation, then waits a gap.
static void rotationService() {
  if (!gSt.fsOk || !gIndexReady || !gPath[0] || gBgHold) return;
  const uint32_t now=millis();
  if (static_cast<int32_t>(now-gBgNextMs)<0) return;
  BreadcrumbScope stage(Crumb::Rotation);
  if (!gSpace.valid() || spaceDue(now)) { // never rotate on an unknown free-space figure
    updateSpace(); gBgNextMs=millis()+ROTATE_OP_GAP_MS; return;
  }
  if (gSt.freeBytes<ROTATE_LOW_BYTES+gBuf.length()) gRotating=true;
  uint32_t gap=200; // idle re-check interval
  if (gRotating) {
    if (gSt.freeBytes>=ROTATE_TARGET_BYTES) {
      if (gSpace.stale()) { updateSpace(); gBgNextMs=millis()+ROTATE_OP_GAP_MS; return; } // verify
      gRotating=false;
    } else {
      size_t idx;
      switch (pickDeletion(idx)) {
        case Pick::VICTIM: deleteOne(idx); gBgNextMs=millis()+ROTATE_OP_GAP_MS; return;
        case Pick::WAIT: gap=ROTATE_SCAN_GAP_URGENT_MS; break; // oldest file not classified yet: scan below
        case Pick::NONE:
          if (gSpace.stale()) { updateSpace(); gBgNextMs=millis()+ROTATE_OP_GAP_MS; return; }
          gRotating=false; gap=1000;
          if (now-gNoCandidateLogMs>=60000 || !gNoCandidateLogMs) {
            gNoCandidateLogMs=now|1u; Serial.println("[log] rotation: low space but no eligible session (current/export only)");
          }
          break;
      }
    }
  }
  if (gUnknown) { scanSlice(); gBgNextMs=millis()+(gRotating?gap:ROTATE_SCAN_GAP_MS); return; }
  gBgNextMs=now+gap;
}
void loggerRotationPlan(Print& out) {
  if (!gSt.fsOk || !gIndexReady) { out.println("[rotation] filesystem unavailable"); return; }
  out.printf("[rotation] read-only, cached index (no file is read); free~%lu low=%lu target=%lu session_max=%lu\n",
      static_cast<unsigned long>(gSt.freeBytes),static_cast<unsigned long>(ROTATE_LOW_BYTES),
      static_cast<unsigned long>(ROTATE_TARGET_BYTES),static_cast<unsigned long>(SESSION_MAX_BYTES));
  out.printf("[rotation] rotating=%s unclassified=%u classified=%lu watermark=s%04lu table=%u/%u\n",
      gRotating?"yes":"no",gUnknown,static_cast<unsigned long>(gSt.classifiedFiles),
      static_cast<unsigned long>(gWatermark),static_cast<unsigned>(gTable.count()),
      static_cast<unsigned>(ProtectionTable::CAP));
  if (gScan.active) out.printf("[rotation] scanning s%04lu at %lu bytes\n",
      static_cast<unsigned long>(gScan.number),static_cast<unsigned long>(gScan.offset));
  std::vector<size_t> order; for (size_t i=0;i<gFiles.size();i++) order.push_back(i);
  auto rank=[](Protect p) { return p==Protect::PLAIN?0:p==Protect::PROTECTED?1:2; };
  std::sort(order.begin(),order.end(),[&](size_t a,size_t b) {
    const int ra=rank(gFiles[a].state),rb=rank(gFiles[b].state);
    return ra!=rb ? ra<rb : gFiles[a].number<gFiles[b].number;
  });
  for (size_t i:order) {
    const SessionFile& f=gFiles[i];
    out.printf("%s %lu %s%s\n",f.path.c_str()+1,static_cast<unsigned long>(f.path==gPath?gSt.fileBytes:f.size),
        f.state==Protect::PLAIN?"plain":f.state==Protect::PROTECTED?"protected":"unknown",
        f.path==gPath?" CURRENT (never delete)":"");
  }
  size_t idx;
  switch (pickDeletion(idx)) {
    case Pick::VICTIM: out.printf("[rotation] next victim: %s (%s)\n",gFiles[idx].path.c_str()+1,
        gFiles[idx].state==Protect::PLAIN?"plain":"protected, no plain left"); break;
    case Pick::WAIT: out.println("[rotation] next victim: waiting for background classification"); break;
    case Pick::NONE: out.println("[rotation] next victim: none eligible"); break;
  }
}
void loggerRotationBench(Print& out) {
  if (!gSt.fsOk || !gIndexReady) { out.println("[rotation-bench] filesystem unavailable"); return; }
  struct Hold { Hold() { gBgHold=true; } ~Hold() { gBgHold=false; } } hold; // no real rotation while measuring
  BreadcrumbScope stage(Crumb::Rotation);
  const uint32_t benchStart=millis();
  loggerFlush();
  uint32_t storedBytes=0;
  for (const auto& f:gFiles) storedBytes+=f.path==gPath?gSt.fileBytes:f.size;
  // 1. one real free-space query, exactly what the background service issues (cache untouched)
  size_t total=0,used=0; uint32_t t0=millis();
  const esp_err_t spaceErr=esp_littlefs_info("spiffs",&total,&used); const uint32_t spaceMs=millis()-t0;
  // 2. a directory listing (what the boot-time index build costs)
  t0=millis(); unsigned listed=0;
  { File root=LittleFS.open("/");
    for (File f=root.openNextFile();f;f=root.openNextFile()) listed++;
    root.close(); }
  const uint32_t dirMs=millis()-t0;
  out.printf("[rotation-bench] read-only. stored=%lu KB in %u session files (acceptance target: ~12 MB)\n",
      static_cast<unsigned long>(storedBytes/1024),static_cast<unsigned>(gFiles.size()));
  out.printf("  space_query_ms=%lu%s  dir_list_ms=%lu (%u entries)\n",static_cast<unsigned long>(spaceMs),
      spaceErr==ESP_OK?"":" (FAILED)",static_cast<unsigned long>(dirMs),listed);
  // 3. replay the classification scan over every closed file with the SAME slice budget the
  //    background uses, running a normal loggerService() pass between slices.
  struct Item { String path; uint32_t number; Protect state; uint32_t size; };
  std::vector<Item> items;
  for (const auto& f:gFiles) if (!excludedPath(f.path)) items.push_back({f.path,f.number,f.state,f.size});
  uint32_t totalBytes=0,totalMs=0,maxSlice=0,maxRead=0,maxService=0,slices=0,mismatches=0;
  static uint8_t buf[ROTATE_SCAN_CHUNK];
  for (const auto& it:items) {
    CsvProtectionScan scan; uint32_t off=0; bool done=false; Protect result=Protect::PLAIN;
    const uint32_t fileStart=millis();
    while (!done) {
      const uint32_t s0=millis();
      File fh=LittleFS.open(it.path,FILE_READ);
      if (!fh) { result=Protect::PROTECTED; break; }
      const uint32_t size=fh.size();
      if (off && !fh.seek(off)) { fh.close(); result=Protect::PROTECTED; break; }
      while (!done) {
        if (off>=size) { result=scan.unsafeAtEof()?Protect::PROTECTED:Protect::PLAIN; done=true; break; }
        if (millis()-s0>=ROTATE_SCAN_SLICE_MS) break;
        const uint32_t r0=millis();
        const size_t n=fh.read(buf,std::min<size_t>(sizeof(buf),size-off));
        maxRead=std::max<uint32_t>(maxRead,millis()-r0);
        if (!n) { result=Protect::PROTECTED; done=true; break; }
        off+=n; totalBytes+=n;
        if (scan.feed(buf,n)) { result=Protect::PROTECTED; done=true; }
      }
      fh.close(); slices++;
      maxSlice=std::max<uint32_t>(maxSlice,millis()-s0);
      loopWatchdogFeed();
      const uint32_t v0=millis(); loggerService(); // drain + flush exactly as a normal pass would
      maxService=std::max<uint32_t>(maxService,millis()-v0);
    }
    const uint32_t fileMs=millis()-fileStart; totalMs+=fileMs;
    const char* have=it.state==Protect::PLAIN?"plain":it.state==Protect::PROTECTED?"protected":"unknown";
    const char* got=result==Protect::PROTECTED?"protected":"plain";
    const bool bad=(it.state==Protect::PLAIN && result==Protect::PROTECTED);
    if (bad) mismatches++;
    out.printf("  s%04lu %8lu B  scan_ms=%-6lu index=%-9s scan=%-9s %s\n",static_cast<unsigned long>(it.number),
        static_cast<unsigned long>(it.size),static_cast<unsigned long>(fileMs),have,got,
        bad?"MISMATCH (index says plain but content has MARK/DETECT)":
        (it.state==Protect::UNKNOWN?"(index unknown; first-run scan result)":
         (it.state!=result?"(index stricter than content; harmless)":"ok")));
  }
  out.printf("  classify_total: %lu KB in %lu ms, %lu slices; max_slice_ms=%lu max_single_read_ms=%lu max_service_pass_ms=%lu\n",
      static_cast<unsigned long>(totalBytes/1024),static_cast<unsigned long>(totalMs),
      static_cast<unsigned long>(slices),static_cast<unsigned long>(maxSlice),
      static_cast<unsigned long>(maxRead),static_cast<unsigned long>(maxService));
  // 4. one simulated rotation pass: pick a victim, touch it read-only. Nothing is deleted.
  size_t idx; const uint32_t p0=micros(); const Pick pick=pickDeletion(idx); const uint32_t pickUs=micros()-p0;
  uint32_t openMs=0;
  if (pick==Pick::VICTIM) {
    const uint32_t o0=millis(); File fh=LittleFS.open(gFiles[idx].path,FILE_READ); if (fh) fh.close(); openMs=millis()-o0;
    out.printf("  simulated pass: would delete %s (%s, %lu B); pick=%lu us, open=%lu ms; NOT deleted\n",
        gFiles[idx].path.c_str()+1,gFiles[idx].state==Protect::PLAIN?"plain":"protected",
        static_cast<unsigned long>(gFiles[idx].size),static_cast<unsigned long>(pickUs),static_cast<unsigned long>(openMs));
  } else out.printf("  simulated pass: %s (pick=%lu us)\n",pick==Pick::WAIT?"would wait for classification":"no eligible victim",
      static_cast<unsigned long>(pickUs));
  out.printf("  live since boot: rotation_max_ms=%lu phase=%s  flush_max_ms=%lu  space_ms=%lu  unclassified=%u\n",
      static_cast<unsigned long>(gSt.rotationMaxMs),gSt.rotationMaxPhase?gSt.rotationMaxPhase:"-",
      static_cast<unsigned long>(gSt.flushMaxMs),static_cast<unsigned long>(gSt.flushSpaceMs),gUnknown);
  const uint32_t worst=std::max({spaceMs,dirMs,maxSlice,maxService,gSt.rotationMaxMs});
  out.printf("[rotation-bench] worst single stage=%lu ms (target < 1000 ms): %s; classification mismatches=%lu; bench took %lu ms\n",
      static_cast<unsigned long>(worst),worst<1000?"PASS":"FAIL",static_cast<unsigned long>(mismatches),
      static_cast<unsigned long>(millis()-benchStart));
}

bool loggerBegin(MetaProvider meta) {
  gEstimate.reset(millis());
  gMeta=meta;
  if (!gBuf.begin(LOG_BUFFER_BYTES)) Serial.println("[log] ERROR: no log buffer");
  auto allocate=[](size_t slots) -> uint64_t* {
    return static_cast<uint64_t*>(psramFound()?ps_malloc(slots*sizeof(uint64_t)):calloc(slots,sizeof(uint64_t)));
  };
  // Apple set is comfortably larger than the number of first-Apple rows that fit
  // in a 1 MiB session. On OOM/full, log unknown addresses rather than lose them.
  gApple.attach(allocate(65536),65536); gUnique.attach(allocate(8192),8192);
  gPrefs.begin("cap",false);
  const bool everFormatted=gPrefs.getBool("fsInit",false);
  if (!LittleFS.begin(false)) {
    if (everFormatted) { Serial.println("[log] mount failed; captures NOT erased. Recovery: format yes"); return false; }
    if (!LittleFS.format() || !LittleFS.begin(false)) { Serial.println("[log] format/mount failed"); return false; }
    gPrefs.putBool("fsInit",true);
  } else if (!everFormatted) gPrefs.putBool("fsInit",true);
  gSt.fsOk=true;
  gSt.rotationMaxPhase=""; gSt.rotationWindowPhase="";
  updateSpace(); buildIndex(); // boot-time only: one directory listing, classification from NVS
  return loggerNewSession("boot");
}
bool loggerNewSession(const char* reason) {
  FlushContextScope context(FlushContext::NEW_SESSION);
  if (!gSt.fsOk) return false;
  if (gPath[0]) { emitSummary(true); loggerFlush(); if (gBuf.length()) return false; }
  if (gPath[0]) { // the closing session keeps its (already persisted) protection state
    if (SessionFile* old=findFile(gCurrentNumber)) {
      setState(*old,gCurrentProtected?Protect::PROTECTED:Protect::PLAIN); old->size=gSt.fileBytes;
    }
  }
  gSt.rows=0; gSt.marks=0;
  uint32_t n=gPrefs.getUInt("nextSess",1);
  char next[24];
  do { snprintf(next,sizeof(next),"/s%04lu.csv",static_cast<unsigned long>(n++)); }
  while (LittleFS.exists(next));
  gPrefs.putUInt("nextSess",n);
  File f=LittleFS.open(next,FILE_WRITE);
  if (!f) { gSt.writeError=true; return false; }
  const bool headerOk=f.print(CSV_HEADER)==strlen(CSV_HEADER) && f.write('\n')==1;
  const uint32_t bytes=f.size(); f.close();
  if (!headerOk) { gSt.writeError=true; return false; }
  strlcpy(gPath,next,sizeof(gPath));
  snprintf(gSt.session,sizeof(gSt.session),"s%04lu",static_cast<unsigned long>(n-1));
  gCurrentProtected=false; gProtRecorded=false; gCurrentNumber=n-1;
  gSt.fileBytes=bytes; gSt.writeError=false;
  if (gIndexReady) gFiles.push_back({gPath,gCurrentNumber,bytes,Protect::PLAIN,0});
  gSpace.wrote(bytes); applyFree(); // estimate only; real queries run in rotationService()
  gApple.clear(); resetMinute();
  char meta[512]={}; if (gMeta) gMeta(meta,sizeof(meta),reason); appendMeta(meta);
  char mode[32]; snprintf(mode,sizeof(mode),"log_mode=%s",loggerModeName()); appendMeta(mode);
  loggerFlush();
  Serial.printf("[log] session %s started (%s), mode=%s\n",gSt.session,reason,loggerModeName());
  return !gSt.writeError;
}
void loggerFlush() {
  BreadcrumbScope stage(Crumb::Flush);
  const uint32_t started=millis(); gLastFlushMs=started;
  uint32_t writtenBytes=0;
  if (!gSt.fsOk || !gPath[0]) return;
  diagnosticsStampFlush(gSt.session,gSt.fileBytes,gBuf.length());
  gSt.flushOpenMs=0; gSt.flushWriteMs=0; gSt.flushCloseMs=0;
  // Persist "this session holds protected rows" BEFORE those rows can reach flash.
  if (gCurrentProtected && !gProtRecorded) recordProtectedCurrent();
  if (gBuf.length()) {
    diagnosticsStampFlush(gSt.session,gSt.fileBytes,gBuf.length());
    breadcrumbsFlushStarted(); File f;
    { BreadcrumbScope op(Crumb::FileOpen); const uint32_t t=millis();
      f=LittleFS.open(gPath,FILE_APPEND); gSt.flushOpenMs=millis()-t; }
    if (!f) { gSt.writeError=true; gSt.writeErrors++; }
    else {
      const size_t want=gBuf.length(); size_t wrote;
      { BreadcrumbScope op(Crumb::FileWrite); const uint32_t t=millis();
        wrote=f.write(gBuf.data(),want); writtenBytes=static_cast<uint32_t>(wrote); gSt.flushWriteMs=millis()-t; }
      gSt.fileBytes=f.size();
      { BreadcrumbScope op(Crumb::FileClose); const uint32_t t=millis();
        f.close(); gSt.flushCloseMs=millis()-t; }
      gSt.writeError=wrote!=want;
      if (!gSt.writeError) { gBuf.clear(); breadcrumbsFlushSucceeded(); }
      else {
        gSt.writeErrors++;
        // Do not retry an already-written prefix and duplicate rows. A partial
        // write is a filesystem failure, not ordinary retention pressure.
        if (wrote) gBuf.clear();
      }
    }
  }
  gSpace.wrote(writtenBytes); applyFree(); // cached estimate; NO filesystem query in the flush
  gEstimate.observe(millis(),writtenBytes);
  gSt.bufferBytes=gBuf.length();
  const uint32_t elapsed=millis()-started;
  gSt.flushLastMs=elapsed;
  gSt.flushWindowMaxMs=std::max(gSt.flushWindowMaxMs,elapsed);
  gSt.closeWindowMaxMs=std::max(gSt.closeWindowMaxMs,gSt.flushCloseMs);
  if (elapsed>gSt.flushMaxMs) { gSt.flushMaxMs=elapsed; gSt.flushMaxAtMs=started; }
}
void loggerService() {
  BreadcrumbScope stage(Crumb::Drain); CaptureRow r;
  for (int i=0;i<128 && snifferPop(r);i++) {
    observe(r);
    if (gRowHook) gRowHook(r); // always detect, BEFORE Apple collapse/car filtering
    if (!gSt.fsOk || gMode==LogMode::CAR) continue;
    const bool apple=collapsibleApple(r);
    const uint64_t key=addressKey(r.mac,0);
    if (apple && gApple.contains(key)) { gSt.appleCollapsed++; gMinute.appleCollapsed++; continue; }
    if (appendRow(r) && apple) gApple.add(key);
  }
  if (!gSt.fsOk) return;
  const uint32_t now=millis();
  bool force=false;
  if (now-gMinute.startMs>=SUMMARY_INTERVAL_MS) { emitSummary(false); force=true; }
  if (now-gLastFlushReportMs>=LOG_FLUSH_REPORT_INTERVAL_MS) {
    gLastFlushReportMs=now; char note[32];
    snprintf(note,sizeof(note),"flush_max_ms=%lu",static_cast<unsigned long>(gSt.flushMaxMs)); appendMeta(note);
    snprintf(note,sizeof(note),"flush_max_at_ms=%lu",static_cast<unsigned long>(gSt.flushMaxAtMs)); appendMeta(note);
    snprintf(note,sizeof(note),"flush_last_ms=%lu",static_cast<unsigned long>(gSt.flushLastMs)); appendMeta(note);
    snprintf(note,sizeof(note),"flush_win_max_ms=%lu",static_cast<unsigned long>(gSt.flushWindowMaxMs)); appendMeta(note);
    snprintf(note,sizeof(note),"close_win_max_ms=%lu",static_cast<unsigned long>(gSt.closeWindowMaxMs)); appendMeta(note);
    snprintf(note,sizeof(note),"space_win_max_ms=%lu",static_cast<unsigned long>(gSt.spaceWindowMaxMs)); appendMeta(note);
    // Rotation/background work: the slowest single operation and which phase it was.
    snprintf(note,sizeof(note),"rot_win_max_ms=%lu",static_cast<unsigned long>(gSt.rotationWindowMaxMs)); appendMeta(note);
    snprintf(note,sizeof(note),"rot_win_phase=%s",gSt.rotationWindowPhase&&*gSt.rotationWindowPhase?gSt.rotationWindowPhase:"none"); appendMeta(note);
    snprintf(note,sizeof(note),"rot_max_ms=%lu",static_cast<unsigned long>(gSt.rotationMaxMs)); appendMeta(note);
    snprintf(note,sizeof(note),"rot_max_phase=%s",gSt.rotationMaxPhase&&*gSt.rotationMaxPhase?gSt.rotationMaxPhase:"none"); appendMeta(note);
    snprintf(note,sizeof(note),"rot_unclassified=%u",gUnknown); appendMeta(note);
    snprintf(note,sizeof(note),"del_win_max_ms=%lu",static_cast<unsigned long>(gSt.deleteWindowMaxMs)); appendMeta(note);
    snprintf(note,sizeof(note),"del_max=%lu rm=%lu nvs=%lu pr=%lu",static_cast<unsigned long>(gSt.deleteMaxMs),
        static_cast<unsigned long>(gSt.deleteMaxRemoveMs),static_cast<unsigned long>(gSt.deleteMaxNvsMs),
        static_cast<unsigned long>(gSt.deleteMaxPrintMs)); appendMeta(note);
    diagnosticsLogHealth();
    gSt.flushWindowMaxMs=0; gSt.closeWindowMaxMs=0; gSt.spaceWindowMaxMs=0;
    gSt.rotationWindowMaxMs=0; gSt.rotationWindowPhase=""; gSt.deleteWindowMaxMs=0;
    force=true;
  }
  const bool retryAllowed=!gSt.writeError || now-gLastFlushMs>=1000;
  if (retryAllowed && (force || gBuf.length()>=LOG_FLUSH_HIGH_WATER || now-gLastFlushMs>=LOG_FLUSH_INTERVAL_MS)) loggerFlush();
  if (gSt.fileBytes>=SESSION_MAX_BYTES && !gSt.writeError) loggerNewSession("size-limit");
  rotationService(); // its own stage after the flush: at most one bounded operation per pass
  gSt.unclassified=gUnknown;
  gSt.bufferBytes=gBuf.length();
}
void loggerMark(const char* note) {
  FlushContextScope context(FlushContext::MARK);
  if (!gSt.fsOk) return;
  appendNote(FrameType::MARK,note); gSt.marks++; loggerFlush();
}
void loggerSetRowHook(void (*hook)(const CaptureRow& row)) { gRowHook=hook; }
void loggerAppendRow(const CaptureRow& row) {
  if (!gSt.fsOk) return;
  if (gMode==LogMode::CAR && row.type!=FrameType::DETECT && row.type!=FrameType::MARK && row.type!=FrameType::META &&
      row.type!=FrameType::GPS) return;
  appendRow(row);
}
void loggerGps(const char* text) {
  CaptureRow r={}; r.ms=millis(); r.type=FrameType::GPS; strlcpy(r.extra,text,sizeof(r.extra));
  loggerAppendRow(r);
}
void loggerCountDetection(uint8_t cat) { if (cat<WL_CAT_COUNT) gMinute.detections[cat]++; }
void loggerMetaDeferred(const char* note) { if (gSt.fsOk) appendMeta(note); }
void loggerMeta(const char* note) {
  FlushContextScope context(FlushContext::META);
  if (gSt.fsOk) { appendMeta(note); loggerFlush(); }
}
void loggerSetClock(const char* utc) {
  strlcpy(gClock,utc,sizeof(gClock)); gClockSetMs=millis();
  char note[80]; snprintf(note,sizeof(note),"host_utc=%s",gClock); loggerMeta(note);
}
const char* loggerClockString() { return gClock; }
uint32_t loggerClockSetMs() { return gClockSetMs; }
const LoggerStatus& loggerStatus() { return gSt; }
uint32_t loggerEstimatedMinutes() {
  if (!gSt.fsOk || gSt.writeError) return UINT32_MAX;
  return gEstimate.minutes(millis(),gSt.freeBytes,static_cast<uint32_t>(gBuf.length()),ROTATE_LOW_BYTES);
}
LogMode loggerMode() { return gMode; }
const char* loggerModeName() { return gMode==LogMode::CAR?"car":"research"; }
bool loggerSetMode(LogMode mode) {
  if (mode==gMode) return true;
  if (!gSt.fsOk) { gMode=mode; return true; }
  // Close the old policy's interval before switching. The new file carries only
  // its own mode's rows; switching never resets the detector's live state.
  const String oldPath=gPath;
  const LogMode old=gMode; gMode=mode;
  if (loggerNewSession("mode-change")) { gEstimate.reset(millis()); return true; }
  if (String(gPath)==oldPath) gMode=old;
  return false;
}
void loggerStorageTest(Print& out) {
  unsigned passed=0,total=0;
  auto check=[&](bool ok,const char* name) { total++; if(ok) passed++; else out.printf("[storage-test] FAIL %s\n",name); };
  uint32_t n=0;
  check(sessionNumber("s0045.csv",n)&&n==45,"session filename");
  check(!sessionNumber("cameras.bin",n),"database excluded");
  check(!sessionNumber("s0001.csv.bak",n),"backup excluded");
  check(!sessionNumber("s42949672960.csv",n),"overflow rejected");
  check(rotationBefore(false,99,true,1),"plain before protected");
  check(rotationBefore(true,1,true,2),"oldest protected first");
  check(!rotationBefore(false,9,false,8),"oldest plain first");
  uint64_t memory[16]; AddressSet set; set.attach(memory,16);
  uint8_t mac[6]={2,0,0,0,0,1};
  check(set.add(addressKey(mac,0))==AddressSet::NEW && set.add(addressKey(mac,0))==AddressSet::EXISTS,"exact duplicate");
  check(set.add(addressKey(mac,1))==AddressSet::NEW,"type identities separate");
  set.clear(); check(!set.contains(addressKey(mac,0)),"session reset");
  CaptureRow apple={}; apple.type=FrameType::BLE_ADV; apple.bleHasCompany=true; apple.bleCompany=0x004C;
  check(collapsibleApple(apple),"ordinary Apple collapsible");
  apple.findMyType=1; apple.findMySeparated=true;
  check(!collapsibleApple(apple),"separated Find My always passes");
  check(trackerDetectExtra("tier=likely cat=tracker rule=findmy:near owner"),"tracker-only does not protect");
  check(!trackerDetectExtra("tier=possible cat=fleet_modem rule=fleet_ssid:IBR600C-de7"),"non-tracker protects");
  check(!trackerDetectExtra("tier=likely rule=cat=tracker"),"missing category protects");
  check(!trackerDetectExtra("cat=tracker cat=camera"),"ambiguous category protects");
  check(unprotectedDetectExtra("tier=possible_low cat=camera rule=x n=1"),"possible_low does not protect");
  check(!unprotectedDetectExtra("tier=possible cat=camera rule=x") && !unprotectedDetectExtra("tier=possible_low tier=high cat=camera"),
        "other/ambiguous tiers protect");
  { // 0.3.10 rotation policy (pure code from rotation_policy.h; no files, NVS or cache touched)
    ProtectionTable t;
    check(t.get(5)==Protect::UNKNOWN && t.set(5,Protect::PROTECTED) && t.get(5)==Protect::PROTECTED,"table set/get");
    check(t.set(5,Protect::PLAIN) && t.get(5)==Protect::PLAIN && t.count()==1,"table update in place");
    uint8_t blob[16]; memcpy(blob,t.data(),t.bytes());
    ProtectionTable u; check(u.load(blob,t.bytes()) && u.get(5)==Protect::PLAIN,"table blob round trip");
    blob[4]=7; check(!u.load(blob,t.bytes()) && u.count()==0,"corrupt table rejected");
    check(unlistedState(100,99)==Protect::UNKNOWN && unlistedState(100,100)==Protect::PLAIN,"watermark");
    const RotCandidate c[]={{3,Protect::UNKNOWN,false},{5,Protect::PLAIN,false},{9,Protect::PROTECTED,false},{7,Protect::PLAIN,true}};
    size_t idx=99; auto get=[&](size_t i) { return c[i]; };
    check(pickVictim(4,get,idx)==Pick::WAIT,"older unknown blocks deletion");
    const RotCandidate d[]={{6,Protect::UNKNOWN,false},{5,Protect::PLAIN,false},{9,Protect::PROTECTED,false}};
    auto getd=[&](size_t i) { return d[i]; };
    check(pickVictim(3,getd,idx)==Pick::VICTIM && idx==1,"oldest plain before newer unknown");
    const RotCandidate e[]={{2,Protect::PROTECTED,false},{4,Protect::PROTECTED,false}};
    auto gete=[&](size_t i) { return e[i]; };
    check(pickVictim(2,gete,idx)==Pick::VICTIM && idx==0,"protected only after plain exhausted");
    const RotCandidate g[]={{2,Protect::PROTECTED,true}};
    auto getg=[&](size_t i) { return g[i]; };
    check(pickVictim(1,getg,idx)==Pick::NONE,"current/export never candidates");
    SpaceCache sc; check(!sc.valid() && sc.due(0,60000,1),"space cache starts invalid");
    sc.query(1000000,400000,1000);
    sc.wrote(1000); check(sc.free()==599000 && !sc.due(2000,60000,131072),"flush only subtracts");
    sc.freed(8192); check(sc.free()==607192 && sc.stale(),"delete credits whole blocks");
    check(sc.due(61000,60000,131072) && sc.due(2000,60000,500),"due by time or bytes");
    check(blockRound(1)==4096 && blockRound(4096)==4096 && blockRound(4097)==8192,"block rounding");
  }
  out.printf("[storage-test] %u/%u passed (synthetic; no files or settings changed)\n",passed,total);
}

// ---- Serial-command helpers ----------------------------------------------------------
static String baseName(const char* raw) {
  String n(raw);
  if (n.startsWith("/")) n.remove(0, 1);
  return n;
}

static bool nameOk(const String& n) {
  return n.length() > 0 && n.indexOf('/') < 0 && n.indexOf("..") < 0;
}

static String pathFor(const char* name) {
  String n(name);
  if (!n.endsWith(".csv")) n += ".csv";
  return "/" + n;
}

void loggerList(Print& out) {
  loggerFlush();
  File root = LittleFS.open("/");
  uint32_t count = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) continue;
    const String name = baseName(f.name());
    const bool current = gPath[0] != '\0' && name == String(gPath + 1);
    out.printf("%s\t%u bytes%s\n", name.c_str(), static_cast<unsigned>(f.size()),
               current ? "\t(current)" : "");
    count++;
  }
  out.printf("%lu file(s)\n", static_cast<unsigned long>(count));
}

bool loggerDump(Print& out, const char* name) {
  FlushContextScope context(FlushContext::DUMP);
  BreadcrumbScope stage(Crumb::Dump);
  if (!nameOk(baseName(name))) return false;
  const String path = pathFor(baseName(name).c_str());
  struct DumpPin {
    explicit DumpPin(const char* p) { strlcpy(gDumpPath,p,sizeof(gDumpPath)); }
    ~DumpPin() { gDumpPath[0]=0; }
  } pin(path.c_str());
  loggerFlush();
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  // Framing lines make this trivial to parse from a script later.
  out.printf("=== BEGIN %s %u ===\n", path.c_str() + 1, static_cast<unsigned>(f.size()));
  // write() can accept fewer bytes than asked when the USB buffer is full (that is what
  // lost bytes before). Keep resending the rest; give up only if the host stops reading.
  uint8_t chunk[512];
  uint32_t crc = 0;
  bool ok = true;
  for (size_t n = f.read(chunk, sizeof(chunk)); ok && n > 0; n = f.read(chunk, sizeof(chunk))) {
    crc = esp_rom_crc32_le(crc, chunk, n);
    size_t off = 0;
    uint32_t lastProgress = millis();
    while (off < n) {
      const size_t w = out.write(chunk + off, n - off);
      if (w) {
        off += w;
        lastProgress = millis();
        loopWatchdogFeed();  // a long export is healthy while bytes keep moving
        continue;
      }
      if (millis() - lastProgress > 3000) { ok = false; break; }
      delay(1);
    }
  }
  f.close();
  out.printf("=== END %s ===\n", path.c_str() + 1);
  // Separate line so older pull scripts still parse; tools/pull_session.py checks it.
  out.printf("=== CRC32 %s %08lx %s ===\n", path.c_str() + 1, static_cast<unsigned long>(crc),
             ok ? "ok" : "ABORTED");
  return ok;
}

bool loggerDelete(const char* name) {
  const String n = baseName(name);
  if (!nameOk(n)) return false;
  const String path = pathFor(n.c_str());
  if (path == String(gPath)) return false;  // never delete the file we're writing
  const bool ok = LittleFS.remove(path);
  if (ok) for (size_t i=0;i<gFiles.size();i++) if (gFiles[i].path==path) { dropFileAt(i); break; }
  updateSpace(); // operator command: one real query is fine here
  return ok;
}

void loggerDeleteAll() {
  std::vector<String> names;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    uint32_t number;
    if (!f.isDirectory() && sessionNumber(f.name(),number)) names.push_back("/" + baseName(f.name()));
  }
  root.close();
  gBuf.clear();
  for (const String& n : names) LittleFS.remove(n);
  gPath[0]=0; gCurrentProtected=false; gProtRecorded=false; gRotating=false;
  gFiles.clear(); gUnknown=0; gScan.active=false; gTable.clear(); saveTable();
  updateSpace();
  loggerNewSession("after-delete-all");
}

bool loggerFormat() {
  gEstimate.reset(millis());
  // Caller holds LoopWatchdogPause plus the Format breadcrumb for this whole call.
  gBuf.clear();
  gPath[0]=0; gCurrentProtected=false; gProtRecorded=false; gRotating=false;
  gFiles.clear(); gUnknown=0; gScan.active=false; gTable.clear(); saveTable();
  LittleFS.end();
  const uint32_t startedMs = millis();
  const bool mounted = LittleFS.format() && LittleFS.begin(false);
  Serial.printf("[log] format/mount %s (%lu ms)\n", mounted ? "OK" : "FAILED",
                static_cast<unsigned long>(millis() - startedMs));
  if (!mounted) {
    gSt.fsOk = false;
    return false;
  }
  gPrefs.putBool("fsInit", true);
  gSt.fsOk = true;
  updateSpace();
  return loggerNewSession("after-format");
}
