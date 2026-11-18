// knob_decode.h - turns the two signals of a rotary encoder (KY-040 CLK and DT) into steps.
// Pure logic, no Arduino. Full-step state machine (the well-known Buxton table): contact bounce and
// half-turns between detents produce no steps, one detent produces exactly one.
// Tested by tools/test_tft_native.py.
#pragma once
#include <stdint.h>

// An interrupt handler may run while flash is busy (a log write), so on the ESP32 the code and the
// table must live in RAM. controls.cpp defines these before including this file; elsewhere they are empty.
#ifndef KNOB_FN_ATTR
#define KNOB_FN_ATTR
#endif
#ifndef KNOB_TABLE_ATTR
#define KNOB_TABLE_ATTR
#endif

class KnobDecoder {
 public:
  // Call with the current level (0/1) of both pins each time either changes. Returns +1, -1 or 0.
  // Which way is "+1" depends on how the encoder is wired; flip it with KNOB_REVERSE in config.h.
  KNOB_FN_ATTR int feed(int clk, int dt) {
    enum : uint8_t { START = 0, CW_FINAL = 1, CW_BEGIN = 2, CW_NEXT = 3, CCW_BEGIN = 4, CCW_FINAL = 5, CCW_NEXT = 6 };
    static const uint8_t kCw = 0x10, kCcw = 0x20;
    static const uint8_t kTable[7][4] KNOB_TABLE_ATTR = {
        {START,    CW_BEGIN,  CCW_BEGIN, START},            // START
        {CW_NEXT,  START,     CW_FINAL,  START | kCw},      // CW_FINAL
        {CW_NEXT,  CW_BEGIN,  START,     START},            // CW_BEGIN
        {CW_NEXT,  CW_BEGIN,  CW_FINAL,  START},            // CW_NEXT
        {CCW_NEXT, START,     CCW_BEGIN, START},            // CCW_BEGIN
        {CCW_NEXT, CCW_FINAL, START,     START | kCcw},     // CCW_FINAL
        {CCW_NEXT, CCW_FINAL, CCW_BEGIN, START},            // CCW_NEXT
    };
    const uint8_t pins = static_cast<uint8_t>(((dt & 1) << 1) | (clk & 1));
    state_ = kTable[state_ & 0x0F][pins];
    const uint8_t dir = state_ & 0x30;
    return dir == kCw ? +1 : (dir == kCcw ? -1 : 0);
  }

 private:
  uint8_t state_ = 0;
};
