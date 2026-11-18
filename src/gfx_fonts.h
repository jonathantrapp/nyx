// gfx_fonts.h - the U8g2 fonts the colour screen uses. They are the same data the OLED already
// links, so the only extra flash is the fonts that are not used there. _tf = Latin-1 (has the
// middle dot), _tr = ASCII only, _tn = digits and a few symbols only.
#pragma once
#include "gfx.h"

extern "C" {
extern const uint8_t u8g2_font_helvR08_tf[];
extern const uint8_t u8g2_font_helvR10_tf[];
extern const uint8_t u8g2_font_helvB10_tf[];
extern const uint8_t u8g2_font_helvR12_tf[];
extern const uint8_t u8g2_font_helvB12_tf[];
extern const uint8_t u8g2_font_helvR14_tf[];
extern const uint8_t u8g2_font_helvB14_tf[];
extern const uint8_t u8g2_font_helvR18_tr[];
extern const uint8_t u8g2_font_helvB18_tr[];
extern const uint8_t u8g2_font_helvR24_tr[];
extern const uint8_t u8g2_font_helvB24_tr[];
extern const uint8_t u8g2_font_logisoso34_tn[];
extern const uint8_t u8g2_font_logisoso42_tn[];
}

namespace gfx {
namespace fonts {
static const Font kTiny      = {u8g2_font_helvR08_tf};
static const Font kSmall     = {u8g2_font_helvR10_tf};
static const Font kSmallBold = {u8g2_font_helvB10_tf};
static const Font kBody      = {u8g2_font_helvR12_tf};
static const Font kBodyBold  = {u8g2_font_helvB12_tf};
static const Font kLabel     = {u8g2_font_helvR14_tf};
static const Font kLabelBold = {u8g2_font_helvB14_tf};
static const Font kTitle     = {u8g2_font_helvR18_tr};
static const Font kTitleBold = {u8g2_font_helvB18_tr};
static const Font kHuge      = {u8g2_font_helvR24_tr};
static const Font kHugeBold  = {u8g2_font_helvB24_tr};
static const Font kDigits    = {u8g2_font_logisoso34_tn};
static const Font kDigitsBig = {u8g2_font_logisoso42_tn};
}  // namespace fonts
}  // namespace gfx
