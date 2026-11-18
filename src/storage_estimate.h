// Rolling logging-rate estimate. No filesystem queries, allocation, or clock assumptions.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

class StorageEstimate {
 public:
  void reset(uint32_t now) {
    started_=now; currentBytes_=0; next_=0; count_=0;
    for (auto& s:samples_) s={};
  }
  // Call after a flush; count only bytes actually accepted by the filesystem.
  void observe(uint32_t now,uint32_t bytes) {
    currentBytes_+=bytes;
    const uint32_t elapsed=now-started_;
    if (elapsed<60000) return;
    samples_[next_]={currentBytes_,elapsed};
    next_=(next_+1)%5;
    if (count_<5) count_++;
    currentBytes_=0; started_=now;
  }
  // UINT32_MAX means still learning/no measured rate. Reserve is rotation's threshold.
  uint32_t minutes(uint32_t now,uint32_t free,uint32_t pending,uint32_t reserve) const {
    if (static_cast<uint64_t>(free)<=static_cast<uint64_t>(pending)+reserve) return 0;
    if (!count_) return UINT32_MAX;
    uint64_t bytes=currentBytes_,ms=now-started_;
    for (unsigned i=0;i<count_;i++) { bytes+=samples_[i].bytes; ms+=samples_[i].ms; }
    if (!bytes) return UINT32_MAX;
    const uint64_t result=(static_cast<uint64_t>(free)-pending-reserve)*ms/(bytes*60000ULL);
    return result>=UINT32_MAX?UINT32_MAX-1:static_cast<uint32_t>(result);
  }
 private:
  struct Sample { uint64_t bytes; uint32_t ms; };
  Sample samples_[5]={};
  uint64_t currentBytes_=0;
  uint32_t started_=0;
  unsigned next_=0,count_=0;
};

inline void storageEstimateText(char* out,size_t size,uint32_t minutes) {
  if (minutes==UINT32_MAX) snprintf(out,size,"EST. -- LEFT");
  else if (!minutes) snprintf(out,size,"EST. <1M LEFT");
  else if (minutes>=6000) snprintf(out,size,"EST. >99H LEFT");
  else snprintf(out,size,"EST. %luH%02luM LEFT",static_cast<unsigned long>(minutes/60),
                static_cast<unsigned long>(minutes%60));
}
