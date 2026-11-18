#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Pure policy, also exercised by storage-test. Only firmware session filenames
// qualify: camera databases and other LittleFS files are never rotation candidates.
inline bool sessionNumber(const char* name, uint32_t& out) {
  if (*name == '/') name++;
  if (*name++ != 's') return false;
  uint32_t n = 0; size_t digits = 0;
  while (*name >= '0' && *name <= '9') {
    const uint32_t digit = static_cast<uint32_t>(*name++ - '0');
    if (n > (UINT32_MAX - digit) / 10) return false;
    n = n * 10 + digit; digits++;
  }
  if (digits < 4 || strcmp(name,".csv") != 0) return false;
  out = n; return true;
}
inline bool rotationBefore(bool protectedA, uint32_t a, bool protectedB, uint32_t b) {
  return protectedA != protectedB ? !protectedA : a < b;
}
// Only a complete, unambiguous cat=tracker token qualifies for the exception.
// Missing/unknown/duplicate categories are conservatively protected.
inline bool trackerDetectExtra(const char* extra) {
  bool found=false;
  for (const char* p=extra; *p;) {
    while (*p==' ') p++;
    const char* end=strchr(p,' '); if (!end) end=p+strlen(p);
    if (end-p>=4 && memcmp(p,"cat=",4)==0) {
      if (found || end-p!=11 || memcmp(p,"cat=tracker",11)!=0) return false;
      found=true;
    }
    p=end;
  }
  return found;
}
// A single, unambiguous tier=possible_low token (the log-only tier: weak matches such as a lone
// "module vendor" OUI) does not protect a session. Anything else (other tier, missing or
// duplicate tier token) stays protected.
inline bool weakTierDetectExtra(const char* extra) {
  bool found=false;
  for (const char* p=extra; *p;) {
    while (*p==' ') p++;
    const char* end=strchr(p,' '); if (!end) end=p+strlen(p);
    if (end-p>=5 && memcmp(p,"tier=",5)==0) {
      if (found || end-p!=17 || memcmp(p,"tier=possible_low",17)!=0) return false;
      found=true;
    }
    p=end;
  }
  return found;
}
// DETECT rows that do not make a session worth keeping over plain ones.
inline bool unprotectedDetectExtra(const char* extra) {
  return trackerDetectExtra(extra) || weakTierDetectExtra(extra);
}
class CsvProtectionScan {
 public:
  bool feed(const uint8_t* data,size_t n) {
    for (size_t i=0;i<n;i++) {
      const char c=static_cast<char>(data[i]);
      if (quotePending_) {
        quotePending_=false;
        if (c=='"') { append(c); continue; } // doubled quote inside a quoted field
        quoted_=false;
      }
      if (quoted_) {
        // Our writer replaces control characters. A newline inside quotes is
        // a torn/malformed record, not a way to hide later MARK/DETECT rows.
        if (c=='\n') return true;
        if (c=='"') quotePending_=true; else append(c);
        continue;
      }
      if (c=='"') quoted_=true;
      else if (c==',' || c=='\n') {
        if (col_==2) {
          tag_[tagUsed_]=0;
          if (strcmp(tag_,"MARK")==0) return true;
          detect_=strcmp(tag_,"DETECT")==0;
        }
        if (c=='\n') {
          extra_[extraUsed_]=0;
          if (col_!=9) return true;
          if (detect_ && (col_!=9 || truncated_ || !unprotectedDetectExtra(extra_))) return true;
          col_=0; tagUsed_=0; extraUsed_=0; detect_=false; truncated_=false;
          tag_[0]=0; extra_[0]=0;
        } else col_++;
      } else if (c!='\r') append(c);
    }
    return false;
  }
  bool unfinishedDetect() const { return detect_; } // protect a torn DETECT record at EOF
  bool unsafeAtEof() const { return detect_ || quoted_ || quotePending_ || col_!=0 || tagUsed_ || extraUsed_; }
 private:
  void append(char c) {
    if (col_==2 && tagUsed_<sizeof(tag_)-1) tag_[tagUsed_++]=c;
    if (col_==9) {
      if (extraUsed_<sizeof(extra_)-1) extra_[extraUsed_++]=c;
      else truncated_=true;
    }
  }
  char tag_[12]={},extra_[160]={};
  unsigned col_=0,tagUsed_=0,extraUsed_=0;
  bool quoted_=false,quotePending_=false,detect_=false,truncated_=false;
};

// Collision-resolving exact set. Storage is supplied by the caller (PSRAM on S3).
// No fingerprints, evictions or false duplicate claims. Full => report degradation.
class AddressSet {
 public:
  enum Result { NEW, EXISTS, FULL };
  void attach(uint64_t* memory, size_t capacity) { data_=memory; cap_=memory ? capacity : 0; clear(); }
  void clear() { count_=0; capped_=false; if (cap_) memset(data_,0,cap_*sizeof(uint64_t)); }
  bool contains(uint64_t key) const {
    if (!cap_) return false;
    size_t p = hash(key) & (cap_-1);
    for (size_t n=0;n<cap_;n++,p=(p+1)&(cap_-1)) {
      if (!data_[p]) return false;
      if (data_[p]==key) return true;
    }
    return false;
  }
  Result add(uint64_t key) {
    if (contains(key)) return EXISTS;
    if (!key || !cap_ || count_ >= cap_*3/4) { capped_=true; return FULL; }
    size_t p=hash(key)&(cap_-1);
    while (data_[p]) p=(p+1)&(cap_-1);
    data_[p]=key; count_++; return NEW;
  }
  bool capped() const { return capped_; }
 private:
  static uint64_t hash(uint64_t v) { v^=v>>33; v*=0xff51afd7ed558ccdULL; v^=v>>33; return v; }
  uint64_t* data_=nullptr; size_t cap_=0,count_=0; bool capped_=false;
};
inline uint64_t addressKey(const uint8_t* mac, uint8_t kind) {
  uint64_t key=static_cast<uint64_t>(kind+1)<<48;
  for (unsigned i=0;i<6;i++) key|=static_cast<uint64_t>(mac[i])<<(8*i);
  return key;
}
