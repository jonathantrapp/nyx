// watchlist.h - types for the on-device watchlist. The DATA lives in watchlist_data.h,
// which tools/gen_watchlist.py generates from watchlist/signatures.csv. Edit the CSV,
// regenerate, rebuild - never hand-edit the generated header.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Tiers, weakest to strongest. Same words as the CSV's tier column.
enum : uint8_t { WL_NONE = 0, WL_POSSIBLE_LOW = 1, WL_POSSIBLE = 2, WL_LIKELY = 3, WL_HIGH = 4 };
// Benign is a suppression annotation, never a positive evidence rank.
constexpr uint8_t WL_BENIGN = 255;

enum : uint8_t {
  WL_CAT_CAMERA, WL_CAT_ACOUSTIC, WL_CAT_BODYCAM, WL_CAT_FLEET_MODEM,
  WL_CAT_GEAR, WL_CAT_TACTICAL, WL_CAT_TRACKER, WL_CAT_COUNT
};

// Tiny regex subset: [(?i)][^]literal[digits/hex quantifier + literal suffix][$].
// Example: (?i)^IBR900-[0-9a-f]{3}-5g$
enum : uint8_t { WL_KIND_SSID, WL_KIND_BLE_NAME, WL_KIND_FLEET_SSID };
enum : uint8_t { WL_TAIL_NONE, WL_TAIL_DIGIT, WL_TAIL_HEX };
enum : uint8_t { WL_F_CI = 1, WL_F_START = 2, WL_F_END = 4 };

struct WlOui   { uint8_t b[3]; uint8_t tier, cat; const char* vendor; };
struct WlMac   { uint8_t b[6]; uint8_t tier, cat; const char* vendor; const char* rule; };
struct WlName  { uint8_t kind, flags; const char* lit; uint8_t tail, tmin, tmax;
                 const char* suffix;
                 uint8_t tier, cat; const char* vendor; const char* rule; };
struct WlU16   { uint16_t id; uint8_t tier, cat; const char* vendor; const char* rule; };
struct WlRange { uint16_t lo, hi; uint8_t tier, cat; const char* vendor; const char* rule; };
struct WlStr   { const char* s; uint8_t tier, cat; const char* vendor; const char* rule; };

inline const char* wlTierName(uint8_t t) {
  switch (t) {
    case WL_BENIGN:       return "benign";
    case WL_POSSIBLE_LOW: return "possible_low";
    case WL_POSSIBLE:     return "possible";
    case WL_LIKELY:       return "likely";
    case WL_HIGH:         return "high";
  }
  return "none";
}

inline const char* wlCategoryName(uint8_t c) {
  static const char* const kNames[WL_CAT_COUNT] = {
      "camera", "acoustic", "bodycam", "fleet_modem", "gear", "tactical", "tracker"};
  return c < WL_CAT_COUNT ? kNames[c] : "?";
}
