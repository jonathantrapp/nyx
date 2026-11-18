#pragma once
// Axon (registered with the Bluetooth SIG as TASER International) company ID 0x034D is also reused by
// cheap smartwatches, so the ID alone is weak evidence. A detector with dated field captures
// (soyboi1312/all-cameras-are-beacons, see watchlist/research/precompiled_lists_survey.md) reports that
// Axon gear's manufacturer data is: company ID (2 bytes, little endian), 0x02, then nine uppercase
// letters/digits (a serial). This is ONE source and has not been seen by this project, so a payload
// without that shape is demoted (to log-only), never dropped.
// Pure functions so tools/test_axon_shape_native.py can compile this exact header on the PC.
#include <stddef.h>
#include <stdint.h>

constexpr uint16_t kAxonCompanyId = 0x034D;

// md = the whole manufacturer-data block as NimBLE gives it: company ID first, then the payload.
inline bool axonMfrShape(const uint8_t* md, size_t len) {
  if (!md || len < 12) return false;
  if (md[0] != 0x4D || md[1] != 0x03) return false;  // 0x034D, little endian
  if (md[2] != 0x02) return false;
  for (size_t i = 3; i < 12; i++) {
    const uint8_t c = md[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

// The tier a company-ID match is allowed to have. Only the Axon ID is gated; only a lower tier is ever returned.
inline uint8_t axonCompanyTier(uint16_t company, uint8_t tier, bool shapeOk, uint8_t demotedTier) {
  if (company != kAxonCompanyId || shapeOk) return tier;
  return tier > demotedTier ? demotedTier : tier;
}
