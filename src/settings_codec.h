#pragma once
#include <stdint.h>
#include <string.h>
#include "config.h"
// Explicit 12-byte encoding: no struct padding, pointer or enum ABI in NVS.
inline bool settingsBlobValid(const uint8_t* b) {
  const unsigned dwell=b[3]|(b[4]<<8);
  return b[0]=='R' && b[1]=='F' && b[2]==1 && dwell>=20 && dwell<=5000 &&
         b[5]<=1 && b[6]<=100 && b[7]<=1 && b[8]<=1 && b[9]<=100 && b[10]<=2 && b[11]==0;
}
inline void settingsDefaultBlob(uint8_t* b) {
  const uint8_t d[12]={'R','F',1,static_cast<uint8_t>(DEFAULT_DWELL_MS&255),
      static_cast<uint8_t>(DEFAULT_DWELL_MS>>8),0,100,0,ENABLE_BLE?1:0,100,1,0};
  memcpy(b,d,sizeof(d));
}
