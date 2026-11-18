// rotation_policy.h - pure, allocation-free pieces of the 0.3.10 rotation design.
// No filesystem, NVS, clock or Arduino calls here, so the native tests execute the real code.
//
//  * ProtectionTable: which session numbers hold MARK / non-tracker DETECT rows. It is
//    written when such a row is first buffered (never by reading the file back).
//    Sessions >= the watermark that are not listed are plain; sessions below it that are
//    not listed are UNKNOWN (written by firmware before 0.3.10) and get scanned once.
//  * pickVictim: oldest plain first, then oldest protected; an UNKNOWN older than the best
//    plain candidate (or any UNKNOWN when no plain exists) makes the caller wait for the scan.
//  * SpaceCache: free-space estimate between real (expensive) filesystem queries.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum class Protect : uint8_t { UNKNOWN = 0, PLAIN = 1, PROTECTED = 2 };

class ProtectionTable {
 public:
  static constexpr size_t CAP = 96;
  struct Rec { uint32_t number; uint8_t state; uint8_t pad[3]; };  // 8 bytes, stored as an NVS blob

  Protect get(uint32_t number) const {
    for (size_t i = 0; i < count_; i++) if (rec_[i].number == number) return static_cast<Protect>(rec_[i].state);
    return Protect::UNKNOWN;
  }
  bool has(uint32_t number) const { return get(number) != Protect::UNKNOWN; }
  // false when the table is full (caller prunes missing sessions, then falls back to the watermark)
  bool set(uint32_t number, Protect state) {
    if (state == Protect::UNKNOWN) { remove(number); return true; }
    for (size_t i = 0; i < count_; i++)
      if (rec_[i].number == number) { rec_[i].state = static_cast<uint8_t>(state); return true; }
    if (count_ >= CAP) return false;
    rec_[count_] = {number, static_cast<uint8_t>(state), {0, 0, 0}};
    count_++; return true;
  }
  bool remove(uint32_t number) {
    for (size_t i = 0; i < count_; i++)
      if (rec_[i].number == number) { rec_[i] = rec_[--count_]; return true; }
    return false;
  }
  void clear() { count_ = 0; }
  size_t count() const { return count_; }
  // Drop records for which keep(number) is false. Returns how many were dropped.
  template <class F> size_t keepIf(F keep) {
    size_t dropped = 0;
    for (size_t i = 0; i < count_;) {
      if (keep(rec_[i].number)) i++; else { rec_[i] = rec_[--count_]; dropped++; }
    }
    return dropped;
  }
  const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(rec_); }
  size_t bytes() const { return count_ * sizeof(Rec); }
  // Accepts only a well-formed blob; anything else leaves the table empty and returns false.
  bool load(const uint8_t* blob, size_t len) {
    count_ = 0;
    if (len % sizeof(Rec) != 0 || len / sizeof(Rec) > CAP) return false;
    Rec tmp[CAP]; if (len) memcpy(tmp, blob, len);
    const size_t n = len / sizeof(Rec);
    for (size_t i = 0; i < n; i++) {
      if (tmp[i].state != static_cast<uint8_t>(Protect::PLAIN) &&
          tmp[i].state != static_cast<uint8_t>(Protect::PROTECTED)) return false;
      for (size_t j = 0; j < i; j++) if (tmp[j].number == tmp[i].number) return false;
    }
    if (n) memcpy(rec_, tmp, len);
    count_ = n; return true;
  }
 private:
  Rec rec_[CAP] = {};
  size_t count_ = 0;
};

// State of a session that has no table record.
inline Protect unlistedState(uint32_t watermark, uint32_t number) {
  return number >= watermark ? Protect::PLAIN : Protect::UNKNOWN;
}

struct RotCandidate { uint32_t number; Protect state; bool excluded; };
enum class Pick { NONE, WAIT, VICTIM };

// get(i) returns the RotCandidate for entry i. On VICTIM, `index` is the entry to delete.
template <class F> Pick pickVictim(size_t n, F get, size_t& index) {
  bool plain = false, prot = false, unknown = false;
  size_t ip = 0, ir = 0; uint32_t np = 0, nr = 0, nu = 0;
  for (size_t i = 0; i < n; i++) {
    const RotCandidate c = get(i);
    if (c.excluded) continue;
    if (c.state == Protect::PLAIN) { if (!plain || c.number < np) { plain = true; ip = i; np = c.number; } }
    else if (c.state == Protect::PROTECTED) { if (!prot || c.number < nr) { prot = true; ir = i; nr = c.number; } }
    else if (!unknown || c.number < nu) { unknown = true; nu = c.number; }
  }
  if (plain) {
    if (unknown && nu < np) return Pick::WAIT;  // an older unclassified file could be plain
    index = ip; return Pick::VICTIM;
  }
  if (unknown) return Pick::WAIT;               // never delete protected while a plain one may exist
  if (prot) { index = ir; return Pick::VICTIM; }
  return Pick::NONE;
}
// Oldest unclassified, non-excluded entry (the scan target). false if none.
template <class F> bool pickScanTarget(size_t n, F get, size_t& index) {
  bool found = false; uint32_t best = 0;
  for (size_t i = 0; i < n; i++) {
    const RotCandidate c = get(i);
    if (c.excluded || c.state != Protect::UNKNOWN) continue;
    if (!found || c.number < best) { found = true; best = c.number; index = i; }
  }
  return found;
}

inline uint32_t blockRound(uint32_t bytes, uint32_t block = 4096) {
  const uint64_t r = (static_cast<uint64_t>(bytes) + block - 1) / block * block;
  return r > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(r);
}

// Free-space estimate. A real query (esp_littlefs_info walks every block) sets the base; flushes
// subtract bytes written, deletions add back whole blocks. Neither touches the filesystem.
class SpaceCache {
 public:
  void query(uint32_t total, uint32_t used, uint32_t now) {
    total_ = total; base_ = total > used ? total - used : 0;
    written_ = 0; credit_ = 0; at_ = now; valid_ = true; stale_ = false;
  }
  void wrote(uint32_t bytes) { written_ = (UINT32_MAX - written_ < bytes) ? UINT32_MAX : written_ + bytes; }
  void freed(uint32_t bytes) { credit_ = (UINT32_MAX - credit_ < bytes) ? UINT32_MAX : credit_ + bytes; stale_ = true; }
  uint32_t free() const {
    uint64_t f = static_cast<uint64_t>(base_) + credit_;
    f = f > written_ ? f - written_ : 0;
    return f > total_ ? total_ : static_cast<uint32_t>(f);
  }
  uint32_t total() const { return total_; }
  bool valid() const { return valid_; }
  bool stale() const { return stale_; }   // estimate includes un-verified deletions
  // Due after `intervalMs`, or once `writtenLimit` bytes were flushed since the last query.
  bool due(uint32_t now, uint32_t intervalMs, uint32_t writtenLimit) const {
    return !valid_ || written_ >= writtenLimit || static_cast<uint32_t>(now - at_) >= intervalMs;
  }
 private:
  uint32_t total_ = 0, base_ = 0, written_ = 0, credit_ = 0, at_ = 0;
  bool valid_ = false, stale_ = false;
};
