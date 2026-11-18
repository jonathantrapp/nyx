// gfx.h - tiny RGB565 drawing library for the colour screen. Portable on purpose: no Arduino,
// no hardware. The firmware points a Canvas at a PSRAM buffer; tools/tests/tft_native_test.cpp
// points one at a PC buffer and writes PNGs, so every screen can be checked without the display.
//
// Coordinates: pixel (i, j) covers [i, i+1) x [j, j+1). Integer arguments are pixels; the
// "AA" functions take float coordinates in the same system and anti-alias their edges.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace gfx {

typedef uint16_t Color;  // RGB565, native byte order (the driver swaps bytes when sending)

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<Color>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
inline uint8_t red8(Color c)   { const uint8_t v = (c >> 11) & 31; return (v << 3) | (v >> 2); }
inline uint8_t green8(Color c) { const uint8_t v = (c >> 5) & 63;  return (v << 2) | (v >> 4); }
inline uint8_t blue8(Color c)  { const uint8_t v = c & 31;         return (v << 3) | (v >> 2); }

// dst*(1-a) + src*a, a = 0..255.
Color blend(Color dst, Color src, uint8_t a);
// Brightness: k = 255 leaves the colour alone, 0 is black.
Color scale(Color c, uint8_t k);

// A U8g2 font (the same compressed bitmap fonts the OLED uses). Only characters up to U+00FF.
struct Font { const uint8_t* data; };
int fontCapHeight(const Font& f);   // height of a capital 'A', in pixels

enum class Align : uint8_t { LEFT, CENTER, RIGHT };
struct CardCoverage {
  struct Span { uint16_t x,n; uint8_t fill,outline; } spans[768];
  uint16_t rows[119]={};
  bool ready=false;
};

class Canvas {
 public:
  Canvas() {}
  Canvas(uint16_t* pixels, int w, int h) { attach(pixels, w, h); }
  void attach(uint16_t* pixels, int w, int h);
  // A canvas that is h pixels tall for drawing purposes but only holds rows y0..y0+rows-1 in memory
  // (`strip` is the first of those rows). Everything outside the strip is clipped away, so the same
  // drawing code can be run once per strip. Used to render into fast internal RAM.
  void attachStrip(uint16_t* strip, int w, int h, int y0, int rows);
  int width() const { return w_; }
  int height() const { return h_; }
  void setCardCoverage(CardCoverage* cache) { card_=cache; }
  uint16_t* pixels() const { return px_; }
  Color pixel(int x, int y) const { return (x < bx0_ || y < by0_ || x >= bx1_ || y >= by1_) ? 0 : px_[y * w_ + x]; }

  // Everything drawn is clipped to this rectangle (and to the canvas / strip). Reset with clearClip().
  void setClip(int x0, int y0, int x1, int y1);  // x1, y1 exclusive
  void clearClip();

  void fill(Color c);
  void set(int x, int y, Color c);
  void mix(int x, int y, Color c, uint8_t alpha);
  void fillRect(int x, int y, int w, int h, Color c, uint8_t alpha = 255);
  void hline(int x, int y, int w, Color c, uint8_t alpha = 255);
  void vline(int x, int y, int h, Color c, uint8_t alpha = 255);
  void frameRect(int x, int y, int w, int h, Color c, uint8_t alpha = 255);
  void line(int x0, int y0, int x1, int y1, Color c, uint8_t alpha = 255);  // 1 px, aliased

  // Anti-aliased shapes. `alpha` scales the coverage.
  void lineAA(float x0, float y0, float x1, float y1, float width, Color c, uint8_t alpha = 255);
  void discAA(float cx, float cy, float r, Color c, uint8_t alpha = 255);
  void ringAA(float cx, float cy, float r, float width, Color c, uint8_t alpha = 255);
  // Rounded rectangle; fill and/or outline (outline is drawn inside the shape's edge).
  void roundRectAA(float x, float y, float w, float h, float radius, bool filled, Color fillColor,
                   uint8_t fillAlpha, float outlineWidth, Color outlineColor, uint8_t outlineAlpha = 255);
  // Filled polygon (even-odd rule), 4x4 supersampled. xy = x0,y0,x1,y1,...; n = vertex count.
  void polyAA(const float* xy, int n, Color c, uint8_t alpha = 255);

  // Text. `y` is the baseline. Returns the x just past the last character.
  // `tracking` adds pixels between characters. Characters are U+0000..U+00FF (UTF-8 accepted).
  int text(const Font& f, int x, int y, const char* s, Color c, int tracking = 0, uint8_t alpha = 255);
  int textAligned(const Font& f, Align a, int x, int y, const char* s, Color c, int tracking = 0,
                  uint8_t alpha = 255);

 private:
  uint16_t* px_ = nullptr;
  CardCoverage* card_=nullptr;
  Color blendR_[32],blendG_[64],blendB_[32],blendSource_=0;
  bool blendReady_=false;
  int w_ = 0, h_ = 0;
  int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
  int bx0_ = 0, by0_ = 0, bx1_ = 0, by1_ = 0;   // the outer limit: the whole canvas, or the strip
};

int textWidth(const Font& f, const char* s, int tracking = 0);

}  // namespace gfx
