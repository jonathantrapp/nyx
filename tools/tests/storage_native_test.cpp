// Executes the actual portable firmware code and actual SUMMARY writer.
#include "storage_policy.h"
#include "rotation_policy.h"
#include "settings_codec.h"
#include "summary_writer.h"
#include "ram_buffer.h"
#include "storage_estimate.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
static unsigned checks=0;
static void check(bool ok,const char* what) {
  checks++; if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
struct Output : Print {
  std::string data;
  size_t write(uint8_t c) override { data.push_back(static_cast<char>(c)); return 1; }
};
int main() {
  StorageEstimate estimate;
  estimate.reset(1000);
  check(estimate.minutes(1000,1000000,0,100000)==UINT32_MAX,"estimate waits for rate");
  estimate.observe(61000,10000);
  check(estimate.minutes(61000,1000000,0,100000)==90,"estimate uses rotation reserve");
  check(estimate.minutes(61000,1000000,10000,100000)==89,"pending bytes reduce estimate");
  check(estimate.minutes(61000,100000,1,100000)==0,"rotation imminent");
  check(estimate.minutes(61000,UINT32_MAX,UINT32_MAX,100000)==0,"reserve arithmetic cannot overflow");
  for(uint32_t i=2;i<=6;i++) estimate.observe(1000+i*60000,0);
  check(estimate.minutes(361000,1000000,0,100000)==UINT32_MAX,"old traffic ages out of window");
  estimate.reset(UINT32_MAX-30000);
  estimate.observe(29999,10000);
  check(estimate.minutes(29999,1000000,0,100000)==90,"estimate survives millis wrap");
  estimate.reset(30000);
  check(estimate.minutes(30000,1000000,0,100000)==UINT32_MAX,"mode reset discards old rate");
  char eta[24]; storageEstimateText(eta,sizeof(eta),203);
  check(std::string(eta)=="EST. 3H23M LEFT","hours and minutes label");
  storageEstimateText(eta,sizeof(eta),UINT32_MAX);
  check(std::string(eta)=="EST. -- LEFT","learning label");
  storageEstimateText(eta,sizeof(eta),0);
  check(std::string(eta)=="EST. <1M LEFT","imminent label");
  storageEstimateText(eta,sizeof(eta),6000);
  check(std::string(eta)=="EST. >99H LEFT","long estimates do not overflow OLED");
  for(uint32_t i=1;i<6000;i++) {
    storageEstimateText(eta,sizeof(eta),i);
    check(std::string(eta).size()*5+35+5<=128,"estimate and volume fit 128 pixel footer");
  }
  uint32_t n=0;
  check(sessionNumber("/s0045.csv",n)&&n==45,"parse current naming");
  for(const char* s:{"cameras.bin","settings.bin","s1.csv","s0001.csv.bak","s42949672960.csv","/../s0001.csv"})
    check(!sessionNumber(s,n),"non-session excluded");
  check(sessionNumber("s4294967295.csv",n)&&n==UINT32_MAX,"largest valid number");
  struct Candidate { uint32_t seq; bool protectedRows; };
  std::vector<Candidate> files={{1,true},{9,false},{3,true},{8,false},{20,false}};
  std::sort(files.begin(),files.end(),[](const auto& a,const auto& b) {
    return rotationBefore(a.protectedRows,a.seq,b.protectedRows,b.seq);
  });
  const uint32_t expected[]={8,9,20,1,3};
  for(size_t i=0;i<files.size();i++) check(files[i].seq==expected[i],"plain oldest then protected oldest");
  // Simulate the caller's explicit current exclusion in the sorted candidate list.
  std::vector<uint32_t> deleted;
  for(const auto& f:files) if(f.seq!=8) deleted.push_back(f.seq);
  check(deleted[0]==9 && deleted.size()==4,"current candidate excluded");
  const std::string plain="millis,channel,frame_type,mac,locally_administered,oui,ssid,rssi,seq,extra\n"
                          "1,1,BEACON,mac,0,oui,\"MARK,DETECT\",-60,1,\n";
  for(size_t split=0;split<=plain.size();split++) {
    CsvProtectionScan scanner;
    check(!scanner.feed(reinterpret_cast<const uint8_t*>(plain.data()),split) &&
          !scanner.feed(reinterpret_cast<const uint8_t*>(plain.data()+split),plain.size()-split),
          "SSID does not falsely protect");
  }
  for(const char* kind:{"MARK","DETECT"}) {
    const std::string csv=plain+"2,,"+kind+",,,,\"test\",,,\n";
    for(size_t split=0;split<=csv.size();split++) {
      CsvProtectionScan scanner;
      const bool a=scanner.feed(reinterpret_cast<const uint8_t*>(csv.data()),split);
      const bool b=a || scanner.feed(reinterpret_cast<const uint8_t*>(csv.data()+split),csv.size()-split);
      check(b,"protection survives every read boundary");
    }
  }
  for (const auto& entry : std::vector<std::pair<std::string,bool>>{
      {"tier=likely cat=tracker rule=findmy:near owner n=1",false},
      {"tier=likely cat=tracker rule=x,y n=1",false},
      {"tier=possible cat=fleet_modem rule=fleet_ssid:IBR600C-de7",true},
      {"tier=high cat=camera rule=x",true},
      {"tier=likely cat=trackerish rule=x",true},
      {"tier=likely scat=tracker rule=x",true},
      {"tier=likely rule=x",true},
      {"cat=tracker cat=camera",true},
      {"cat=tracker cat=tracker",true},
      {"tier=possible_low cat=camera rule=module_vendor:Liteon n=1",false},
      {"tier=possible_low cat=fleet_modem rule=x",false},
      {"tier=possible cat=camera rule=x",true},
      {"tier=likely cat=camera rule=x",true},
      {"tier=possible_lowx cat=camera rule=x",true},
      {"tier=possible_low tier=high cat=camera rule=x",true},
      {"tier=high tier=possible_low cat=camera rule=x",true},
      {"cat=camera rule=x",true},
      {"xtier=possible_low cat=camera rule=x",true}}) {
    const std::string csv=plain+"2,1,DETECT,mac,0,oui,\"vendor, \"\"quoted\"\"\",-60,0,\""+entry.first+"\"\n";
    for (size_t split=0;split<=csv.size();split++) {
      CsvProtectionScan scanner;
      bool protect=scanner.feed(reinterpret_cast<const uint8_t*>(csv.data()),split);
      if (!protect) protect=scanner.feed(reinterpret_cast<const uint8_t*>(csv.data()+split),csv.size()-split);
      protect=protect || scanner.unfinishedDetect();
      check(protect==entry.second,"tracker exception and quoted CSV across every split");
    }
  }
  CsvProtectionScan torn;
  const char* incomplete="2,,DETECT,,,,\"vendor\",,,\"cat=tracker";
  check(!torn.feed(reinterpret_cast<const uint8_t*>(incomplete),strlen(incomplete)) && torn.unfinishedDetect(),
        "unfinished DETECT protected");
  CsvProtectionScan tornRaw;
  const char* rawTail="3,1,BEACON,mac,0,oui,\"truncated";
  check(!tornRaw.feed(reinterpret_cast<const uint8_t*>(rawTail),strlen(rawTail)) && tornRaw.unsafeAtEof(),
        "torn raw quote cannot conceal later events");
  CsvProtectionScan malformed;
  const char* broken="3,1,BEACON,mac,0,oui,\"truncated\n4,,MARK,,,,note,,,\n";
  check(malformed.feed(reinterpret_cast<const uint8_t*>(broken),strlen(broken)),"malformed quoted row protected");
  check(trackerDetectExtra("tier=likely cat=tracker rule=foo"),"live tracker row unprotected");
  check(!trackerDetectExtra("tier=likely cat=camera rule=cat=tracker"),"rule substring cannot override category");
  check(unprotectedDetectExtra("tier=possible_low cat=camera rule=x") && !unprotectedDetectExtra("tier=possible cat=camera rule=x"),
        "only possible_low is weak");
  RamBuffer buffer; check(buffer.begin(8),"RAM buffer allocated");
  const uint8_t bytes[8]={1,2,3,4,5,6,7,8};
  check(buffer.write(bytes,8)==8 && buffer.length()==8,"RAM buffer exact capacity");
  check(buffer.write(bytes,1)==0 && buffer.length()==8 && buffer.overflowed(),"RAM buffer overflow transactional");
  buffer.rewind(3); buffer.clearOverflow();
  check(buffer.write(bytes,SIZE_MAX)==0 && buffer.length()==3 && buffer.overflowed(),"size_t wrap rejected");
  buffer.clear(); buffer.clearOverflow();
  check(buffer.write(nullptr,0)==0 && !buffer.overflowed(),"zero write safe without dereference");
  uint64_t memory[32]; AddressSet set; set.attach(memory,32);
  uint8_t mac[6]={0,0,0,0,0,0};
  for(unsigned i=0;i<24;i++) { mac[5]=static_cast<uint8_t>(i); check(set.add(addressKey(mac,0))==AddressSet::NEW,"collision-resolving insertion"); }
  for(unsigned i=0;i<24;i++) { mac[5]=static_cast<uint8_t>(i); check(set.add(addressKey(mac,0))==AddressSet::EXISTS,"exact repeats"); }
  mac[5]=99;
  check(set.add(addressKey(mac,0))==AddressSet::FULL && !set.contains(addressKey(mac,0)),"full is not a false duplicate");
  check(set.capped(),"full is reported"); set.clear(); check(!set.capped(),"session reset clears degradation");
  check(set.add(addressKey(mac,0))==AddressSet::NEW && set.add(addressKey(mac,1))==AddressSet::NEW,"row types separated");
  AddressSet missing; check(missing.add(1)==AddressSet::FULL,"allocation failure explicit");
  uint8_t blob[12]; settingsDefaultBlob(blob); check(settingsBlobValid(blob),"defaults valid");
  check(blob[7]==0 && blob[10]==1 && blob[6]==100,"research, dim LED, full volume defaults");
  for(unsigned byte:{0u,1u,2u,5u,6u,7u,8u,9u,10u,11u}) {
    settingsDefaultBlob(blob); blob[byte]=255; check(!settingsBlobValid(blob),"corrupt field rejected");
  }
  settingsDefaultBlob(blob); blob[3]=19; blob[4]=0; check(!settingsBlobValid(blob),"dwell too short");
  blob[3]=0x89; blob[4]=0x13; check(!settingsBlobValid(blob),"dwell above 5000");
  settingsDefaultBlob(blob); blob[7]=1; blob[10]=0; check(settingsBlobValid(blob),"car/off LED round trip");
  MinuteSummary summary; summary.startMs=10; summary.durationMs=60000;
  for(auto& h:summary.heard) h=UINT32_MAX;
  for(auto& u:summary.unique) u=UINT32_MAX;
  for(auto& d:summary.detections) d=UINT32_MAX;
  summary.uniqueCapped=true; summary.partial=true;
  Output output; csvWriteSummary(output,summary,60010);
  check(output.data.find("60010,,SUMMARY,,,,\"minute\",,,\"v=1 start_ms=10")==0,"CSV columns and window anchor");
  check(output.data.find("det_fleet_modem=4294967295")!=std::string::npos,"full-width category counter");
  check(output.data.find("unique_capped=1")!=std::string::npos,"lower-bound flag serialized");
  check(output.data.substr(output.data.size()-2)=="\"\n","one terminated CSV row");
  // ---- 0.3.10 rotation policy: protection table, victim picker, space cache ----
  {
    ProtectionTable t;
    check(t.get(5)==Protect::UNKNOWN && !t.has(5),"unlisted is unknown");
    check(t.set(5,Protect::PROTECTED) && t.get(5)==Protect::PROTECTED,"record protected");
    check(t.set(5,Protect::PLAIN) && t.get(5)==Protect::PLAIN && t.count()==1,"update in place, no duplicate");
    check(t.set(5,Protect::UNKNOWN) && t.count()==0,"setting unknown removes");
    for(uint32_t i=0;i<ProtectionTable::CAP;i++) check(t.set(1000+i,Protect::PROTECTED),"fills to capacity");
    check(!t.set(9999,Protect::PROTECTED) && t.get(9999)==Protect::UNKNOWN,"full table reports failure");
    check(t.keepIf([](uint32_t n){return n>=1000+ProtectionTable::CAP/2;})==ProtectionTable::CAP/2,"prune missing");
    check(t.set(9999,Protect::PROTECTED),"room after pruning");
    uint8_t blob[ProtectionTable::CAP*sizeof(ProtectionTable::Rec)];
    ProtectionTable u; t.set(7,Protect::PLAIN);
    memcpy(blob,t.data(),t.bytes());
    check(u.load(blob,t.bytes()) && u.count()==t.count() && u.get(7)==Protect::PLAIN && u.get(9999)==Protect::PROTECTED,"blob round trip");
    check(u.load(blob,0) && u.count()==0,"empty blob ok");
    check(!u.load(blob,5) && u.count()==0,"odd length rejected");
    ProtectionTable bad; memcpy(blob,t.data(),sizeof(ProtectionTable::Rec)); blob[4]=9;
    check(!bad.load(blob,sizeof(ProtectionTable::Rec)) && bad.count()==0,"invalid state rejected");
    ProtectionTable one; one.set(3,Protect::PLAIN); uint8_t two[16];
    memcpy(two,one.data(),8); memcpy(two+8,one.data(),8);
    check(!bad.load(two,16),"duplicate records rejected");
    uint8_t huge[(ProtectionTable::CAP+1)*sizeof(ProtectionTable::Rec)] = {};
    for(size_t i=0;i<=ProtectionTable::CAP;i++) { huge[i*8]=static_cast<uint8_t>(i+1); huge[i*8+4]=1; }
    check(!bad.load(huge,sizeof(huge)) && bad.count()==0,"oversize blob rejected");
  }
  check(unlistedState(72,71)==Protect::UNKNOWN && unlistedState(72,72)==Protect::PLAIN && unlistedState(72,500)==Protect::PLAIN,
        "watermark: old sessions unknown, new sessions plain");
  {
    // The field layout from unit A before 0.3.10: ten old files, one current. All below the watermark.
    std::vector<RotCandidate> f;
    for(uint32_t n:{50u,52u,53u,62u,63u,64u,67u,68u,69u,70u}) f.push_back({n,Protect::UNKNOWN,false});
    f.push_back({71,Protect::PLAIN,true}); // current
    auto get=[&](size_t i){return f[i];};
    size_t idx=0,scan=0;
    check(pickVictim(f.size(),get,idx)==Pick::WAIT,"all unknown: nothing deleted before classification");
    check(pickScanTarget(f.size(),get,scan) && f[scan].number==50,"scan oldest unknown first");
    f[0].state=Protect::PROTECTED;                       // s0050 classified
    check(pickVictim(f.size(),get,idx)==Pick::WAIT,"still waiting while older-than-nothing unknowns remain");
    for(size_t i=1;i<f.size()-1;i++) f[i].state=Protect::PROTECTED;
    f[9].state=Protect::PLAIN;                           // s0070 plain
    check(pickVictim(f.size(),get,idx)==Pick::VICTIM && f[idx].number==70,"plain s0070 goes before protected s0050");
    f[9].excluded=true;                                  // export pin / delete back-off
    check(pickVictim(f.size(),get,idx)==Pick::VICTIM && f[idx].number==50,"then oldest protected");
    for(auto& c:f) c.excluded=true;
    check(pickVictim(f.size(),get,idx)==Pick::NONE && !pickScanTarget(f.size(),get,scan),"nothing eligible");
  }
  {
    std::vector<RotCandidate> f={{10,Protect::UNKNOWN,false},{12,Protect::PLAIN,false}};
    auto get=[&](size_t i){return f[i];};
    size_t idx=0;
    check(pickVictim(f.size(),get,idx)==Pick::WAIT,"older unknown blocks a newer plain victim");
    f[0].number=15;
    check(pickVictim(f.size(),get,idx)==Pick::VICTIM && idx==1,"newer unknown does not block");
  }
  {
    SpaceCache c;
    check(!c.valid() && c.due(0,60000,1000),"no figure yet: query due");
    c.query(13000000,8000000,1000);
    check(c.free()==5000000 && c.valid() && !c.stale(),"query sets base");
    c.wrote(1200); c.wrote(1200);
    check(c.free()==5000000-2400,"flushes subtract written bytes");
    check(!c.due(30000,60000,131072) && c.due(61001,60000,131072),"due by interval");
    c.wrote(131072); check(c.due(2000,60000,131072),"due by bytes written");
    c.freed(blockRound(1050262)); check(c.stale() && c.free()>5000000-2400-131072,"delete credit marks stale");
    c.query(13000000,13000000,5000);
    check(c.free()==0 && !c.stale(),"full filesystem reads zero");
    c.wrote(UINT32_MAX); c.wrote(10); check(c.free()==0,"counters saturate");
    c.query(100,50,1); c.freed(UINT32_MAX); check(c.free()==100,"free never exceeds total");
    c.query(1000,0,UINT32_MAX-10); check(!c.due(20,60000,1<<20),"interval survives millis wrap");
    check(blockRound(0)==0 && blockRound(1)==4096 && blockRound(4096)==4096 && blockRound(4097)==8192,"block rounding");
    check(blockRound(UINT32_MAX)==UINT32_MAX,"block rounding saturates");
  }
  std::printf("storage native: %u checks passed\n",checks);
  std::printf("%s",output.data.c_str());
}
