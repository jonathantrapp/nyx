// ram_buffer.h - a Print that appends into a fixed block of RAM.
//
// csvWriteRow() writes into this, so formatting a row never touches flash. The logger
// copies the whole block to the file every few seconds (one open/write/close).
#pragma once
#include <Arduino.h>
#include <esp32-hal-psram.h>

class RamBuffer : public Print {
 public:
  // Prefer PSRAM (8 MB, plentiful); fall back to internal heap.
  bool begin(size_t capacity) {
    data_ = static_cast<uint8_t*>(psramFound() ? ps_malloc(capacity) : malloc(capacity));
    cap_ = data_ ? capacity : 0;
    return data_ != nullptr;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  // All-or-nothing per call. If it doesn't fit, flag the overflow so the logger can
  // throw away the half-written row instead of leaving a broken line in the file.
  size_t write(const uint8_t* src, size_t n) override {
    if (len_ > cap_ || n > cap_ - len_) {
      overflow_ = true;
      return 0;
    }
    if (n) memcpy(data_ + len_, src, n);
    len_ += n;
    return n;
  }

  const uint8_t* data() const { return data_; }
  size_t length() const { return len_; }
  void clear() { len_ = 0; }
  void rewind(size_t to) { if (to < len_) len_ = to; }
  bool overflowed() const { return overflow_; }
  void clearOverflow() { overflow_ = false; }

 private:
  uint8_t* data_ = nullptr;
  size_t cap_ = 0;
  size_t len_ = 0;
  bool overflow_ = false;
};
