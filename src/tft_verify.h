#pragma once
#include <stdint.h>

// Position-dependent, repeatable random-looking data. Every location is checked, including edges.
inline uint16_t tftVerifyPixel(uint32_t index,uint32_t seed) {
  uint32_t v=index+seed*0x9E3779B9UL;
  v^=v>>16; v*=0x7FEB352DUL; v^=v>>15; v*=0x846CA68BUL; v^=v>>16;
  return static_cast<uint16_t>(v);
}
inline uint8_t tftReadByte(const uint8_t* raw,unsigned bit) {
  const unsigned shift=bit%8;
  return shift?static_cast<uint8_t>((raw[bit/8]<<shift)|(raw[bit/8+1]>>(8-shift))):raw[bit/8];
}
inline uint16_t tftRead565(const uint8_t* raw,unsigned pixel,unsigned dummyBits,bool swapRB) {
  const unsigned bit=dummyBits+pixel*24;
  const uint8_t r=tftReadByte(raw,bit+(swapRB?16:0)),g=tftReadByte(raw,bit+8),b=tftReadByte(raw,bit+(swapRB?0:16));
  return static_cast<uint16_t>(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3));
}
