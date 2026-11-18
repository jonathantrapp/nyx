// gfx.cpp - see gfx.h. The font decoder follows U8g2's own (u8g2_font.c); the native test
// compares its output with the real U8g2 so the two cannot drift apart unnoticed.
#include "gfx.h"
#include <math.h>

namespace gfx {

// ---- colour ---------------------------------------------------------------------------

Color blend(Color d, Color s, uint8_t a) {
  if (a == 255) return s;
  if (a == 0) return d;
  const uint32_t al = (static_cast<uint32_t>(a) + 4) >> 3;  // 0..32
  const uint32_t D = (d | (static_cast<uint32_t>(d) << 16)) & 0x07E0F81FUL;
  const uint32_t S = (s | (static_cast<uint32_t>(s) << 16)) & 0x07E0F81FUL;
  const uint32_t r = ((((S - D) * al) >> 5) + D) & 0x07E0F81FUL;
  return static_cast<Color>(r | (r >> 16));
}

Color scale(Color c, uint8_t k) {
  return rgb(static_cast<uint8_t>(red8(c) * k / 255), static_cast<uint8_t>(green8(c) * k / 255),
             static_cast<uint8_t>(blue8(c) * k / 255));
}

// ---- canvas basics --------------------------------------------------------------------

void Canvas::attach(uint16_t* pixels, int w, int h) {
  px_ = pixels; w_ = w; h_ = h;
  bx0_ = 0; by0_ = 0; bx1_ = w; by1_ = h;
  clearClip();
}
void Canvas::attachStrip(uint16_t* strip, int w, int h, int y0, int rows) {
  // px_ is a virtual base: row y lives at px_ + y*w, which is inside `strip` only for the strip's rows.
  // Every write is clipped to those rows, so the out-of-range part of the address is never touched.
  px_ = strip - static_cast<ptrdiff_t>(y0) * w; w_ = w; h_ = h;
  bx0_ = 0; by0_ = y0; bx1_ = w; by1_ = y0 + rows > h ? h : y0 + rows;
  clearClip();
}
void Canvas::clearClip() { cx0_ = bx0_; cy0_ = by0_; cx1_ = bx1_; cy1_ = by1_; }
void Canvas::setClip(int x0, int y0, int x1, int y1) {
  cx0_ = x0 < bx0_ ? bx0_ : x0;  cy0_ = y0 < by0_ ? by0_ : y0;
  cx1_ = x1 > bx1_ ? bx1_ : x1;  cy1_ = y1 > by1_ ? by1_ : y1;
}

void Canvas::fill(Color c) {
  if (!px_) return;
  const uint32_t pair = (static_cast<uint32_t>(c) << 16) | c;
  for (int y = by0_; y < by1_; y++) {
    uint16_t* row = px_ + static_cast<ptrdiff_t>(y) * w_;
    int x = bx0_;
    if (x & 1) row[x++] = c;
    for (; x + 1 < bx1_; x += 2) *reinterpret_cast<uint32_t*>(row + x) = pair;
    if (x < bx1_) row[x] = c;
  }
}
void Canvas::set(int x, int y, Color c) {
  if (x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_) return;
  px_[y * w_ + x] = c;
}
void Canvas::mix(int x, int y, Color c, uint8_t alpha) {
  if (!alpha || x < cx0_ || y < cy0_ || x >= cx1_ || y >= cy1_) return;
  uint16_t& p = px_[y * w_ + x];
  p = alpha == 255 ? c : blend(p, c, alpha);
}

void Canvas::fillRect(int x, int y, int w, int h, Color c, uint8_t alpha) {
  if (!px_ || w <= 0 || h <= 0) return;
  const bool table=alpha==238 && w>=32;
  if(table && (!blendReady_ || blendSource_!=c)) {
    for(unsigned i=0;i<32;i++) {blendR_[i]=blend(static_cast<Color>(i<<11),c,238)&0xF800;blendB_[i]=blend(static_cast<Color>(i),c,238)&0x001F;}
    for(unsigned i=0;i<64;i++)blendG_[i]=blend(static_cast<Color>(i<<5),c,238)&0x07E0;
    blendSource_=c;blendReady_=true;
  }
  const int x0 = x < cx0_ ? cx0_ : x, y0 = y < cy0_ ? cy0_ : y;
  const int x1 = x + w > cx1_ ? cx1_ : x + w, y1 = y + h > cy1_ ? cy1_ : y + h;
  for (int j = y0; j < y1; j++) {
    uint16_t* row = px_ + j * w_;
    if (alpha == 255) { for (int i = x0; i < x1; i++) row[i] = c; }
    else if(table){
      Color last=0,mixed=blendR_[0]|blendG_[0]|blendB_[0];
      for(int i=x0;i<x1;i++){
        const Color v=row[i];
        if(v!=last){mixed=blendR_[v>>11]|blendG_[(v>>5)&63]|blendB_[v&31];last=v;}
        row[i]=mixed;
      }
    }
    else if (alpha) { for (int i = x0; i < x1; i++) row[i] = blend(row[i], c, alpha); }
  }
}
void Canvas::hline(int x, int y, int w, Color c, uint8_t alpha) { fillRect(x, y, w, 1, c, alpha); }
void Canvas::vline(int x, int y, int h, Color c, uint8_t alpha) { fillRect(x, y, 1, h, c, alpha); }
void Canvas::frameRect(int x, int y, int w, int h, Color c, uint8_t alpha) {
  if (w <= 0 || h <= 0) return;
  hline(x, y, w, c, alpha);
  hline(x, y + h - 1, w, c, alpha);
  if (h > 2) { vline(x, y + 1, h - 2, c, alpha); vline(x + w - 1, y + 1, h - 2, c, alpha); }
}

// A pixel line defined by a formula (not by stepping), so the result does not depend on the clip
// and a strip can skip straight to the part of the line it needs. i runs along the longer axis.
void Canvas::line(int x0, int y0, int x1, int y1, Color c, uint8_t alpha) {
  const int dx = x1 - x0, dy = y1 - y0;
  const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  const int n = adx > ady ? adx : ady;
  if (n == 0) { mix(x0, y0, c, alpha); return; }
  const int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
  int lo = 0, hi = n;
  auto limit = [&](int p0, int d, int c0, int c1) {           // keep only i where this axis can be inside [c0, c1)
    if (d == 0) { if (p0 < c0 - 1 || p0 > c1) { lo = 1; hi = 0; } return; }
    float ia = (c0 - 1 - p0) * static_cast<float>(n) / d, ib = (c1 + 1 - p0) * static_cast<float>(n) / d;
    if (ia > ib) { const float t = ia; ia = ib; ib = t; }
    const int l = static_cast<int>(floorf(ia)) - 2, h = static_cast<int>(ceilf(ib)) + 2;
    if (l > lo) lo = l;
    if (h < hi) hi = h;
  };
  limit(x0, dx, cx0_, cx1_);
  limit(y0, dy, cy0_, cy1_);
  for (int i = lo; i <= hi; i++) {
    int x, y;
    if (adx >= ady) { x = x0 + sx * i; y = y0 + sy * ((2 * ady * i + n) / (2 * n)); }
    else { y = y0 + sy * i; x = x0 + sx * ((2 * adx * i + n) / (2 * n)); }
    mix(x, y, c, alpha);
  }
}

// ---- anti-aliased shapes ----------------------------------------------------------------

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline uint8_t covAlpha(float cov, uint8_t alpha) {
  return static_cast<uint8_t>(clamp01(cov) * alpha + 0.5f);
}

// Pixel row range of a bounding box, limited to the clip.
static inline void rowSpan(float cy, float ext, int clipLo, int clipHi, int& y0, int& y1) {
  y0 = static_cast<int>(floorf(cy - ext)); y1 = static_cast<int>(ceilf(cy + ext));
  if (y0 < clipLo) y0 = clipLo;
  if (y1 > clipHi) y1 = clipHi;
}
static inline void colSpan(float cx, float half, int clipLo, int clipHi, int& x0, int& x1) {
  x0 = static_cast<int>(floorf(cx - half)) - 1; x1 = static_cast<int>(ceilf(cx + half)) + 1;
  if (x0 < clipLo) x0 = clipLo;
  if (x1 > clipHi) x1 = clipHi;
}

void Canvas::lineAA(float x0, float y0, float x1, float y1, float width, Color c, uint8_t alpha) {
  if (!px_) return;
  const float hw = width * 0.5f;
  if ((x0>x1?x0:x1)+hw+2<cx0_ || (x0<x1?x0:x1)-hw-2>=cx1_ ||
      (y0>y1?y0:y1)+hw+2<cy0_ || (y0<y1?y0:y1)-hw-2>=cy1_)return;
  int bx0 = static_cast<int>(floorf((x0 < x1 ? x0 : x1) - hw - 1.0f));
  int bx1 = static_cast<int>(ceilf((x0 > x1 ? x0 : x1) + hw + 1.0f));
  int by0 = static_cast<int>(floorf((y0 < y1 ? y0 : y1) - hw - 1.0f));
  int by1 = static_cast<int>(ceilf((y0 > y1 ? y0 : y1) + hw + 1.0f));
  if (bx0 < cx0_) bx0 = cx0_;
  if (by0 < cy0_) by0 = cy0_;
  if (bx1 > cx1_) bx1 = cx1_;
  if (by1 > cy1_) by1 = cy1_;
  const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
  const float inv = len2 > 0.0f ? 1.0f / len2 : 0.0f;
  const float outer = (hw + 0.5f) * (hw + 0.5f);
  const float innerR = hw > 0.5f ? hw - 0.5f : 0.0f;
  const float inner = hw > 0.5f ? innerR * innerR : -1.0f;
  for (int y = by0; y < by1; y++) {
    const float py = y + 0.5f;
    for (int x = bx0; x < bx1; x++) {
      const float px = x + 0.5f;
      float t = ((px - x0) * dx + (py - y0) * dy) * inv;
      t = clamp01(t);
      const float qx = x0 + t * dx - px, qy = y0 + t * dy - py;
      const float d2 = qx * qx + qy * qy;
      if (d2 >= outer) continue;                                       // clearly outside: no sqrt
      if (d2 <= inner) { mix(x, y, c, alpha); continue; }              // clearly inside
      mix(x, y, c, covAlpha(hw + 0.5f - sqrtf(d2), alpha));
    }
  }
}

void Canvas::discAA(float cx, float cy, float r, Color c, uint8_t alpha) {
  if (!px_) return;
  if(cx+r+2<cx0_ || cx-r-2>=cx1_ || cy+r+2<cy0_ || cy-r-2>=cy1_)return;
  int by0, by1;
  rowSpan(cy, r + 1.0f, cy0_, cy1_, by0, by1);
  const float outer = (r + 0.5f) * (r + 0.5f);
  const float innerR = r > 0.5f ? r - 0.5f : 0.0f;
  const float inner = r > 0.5f ? innerR * innerR : -1.0f;
  for (int y = by0; y < by1; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= outer) continue;
    int bx0, bx1;
    colSpan(cx, sqrtf(outer - dy2), cx0_, cx1_, bx0, bx1);
    for (int x = bx0; x < bx1; x++) {
      const float dx = x + 0.5f - cx, d2 = dx * dx + dy2;
      if (d2 >= outer) continue;
      if (d2 <= inner) { mix(x, y, c, alpha); continue; }
      mix(x, y, c, covAlpha(r + 0.5f - sqrtf(d2), alpha));
    }
  }
}

void Canvas::ringAA(float cx, float cy, float r, float width, Color c, uint8_t alpha) {
  if (!px_) return;
  if(cx+r+width+2<cx0_ || cx-r-width-2>=cx1_ || cy+r+width+2<cy0_ || cy-r-width-2>=cy1_)return;
  const float hw = width * 0.5f, ro = r + hw + 0.5f;
  int by0, by1;
  rowSpan(cy, ro + 1.0f, cy0_, cy1_, by0, by1);
  const float outer = ro * ro;
  const float riR = r - hw - 0.5f;
  const float innerHole = riR > 0.0f ? riR * riR : -1.0f;              // pixels closer than this are inside the hole
  for (int y = by0; y < by1; y++) {
    const float dy = y + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= outer) continue;
    int bx0, bx1;
    colSpan(cx, sqrtf(outer - dy2), cx0_, cx1_, bx0, bx1);
    for (int x = bx0; x < bx1; x++) {
      const float dx = x + 0.5f - cx, d2 = dx * dx + dy2;
      if (d2 >= outer || d2 <= innerHole) continue;
      mix(x, y, c, covAlpha(hw + 0.5f - fabsf(sqrtf(d2) - r), alpha));
    }
  }
}

void Canvas::roundRectAA(float x, float y, float w, float h, float radius, bool filled, Color fillColor,
                         uint8_t fillAlpha, float outlineWidth, Color outlineColor, uint8_t outlineAlpha) {
  if (!px_) return;
  // The warning card's coverage never changes with its text, map or close-state colour.
  // Store runs of equal coverage, not pixels, in the caller-owned frame cache.
  if(card_ && x==8 && y==36 && w==160 && h==116 && radius==11 && filled && fillAlpha==238 && outlineWidth==2 && outlineAlpha==255) {
    if(!card_->ready) {
      unsigned count=0;
      for(int row=0;row<118;row++) {
        card_->rows[row]=count;
        for(int col=0;col<162;col++) {
          const float qx=fabsf(col+7.5f-88)-69,qy=fabsf(row+35.5f-94)-47;
          uint8_t a,b;
          if(qx<0 && qy<0 && (qx>qy?qx:qy)<8){a=238;b=0;}
          else {
            const float ox=qx>0?qx:0,oy=qy>0?qy:0,inside=qx>qy?qx:qy;
            const float sd=sqrtf(ox*ox+oy*oy)+(inside<0?inside:0)-11;
            const float edge=clamp01(.5f-sd);
            a=covAlpha(edge,238);b=covAlpha(edge*clamp01(sd+2.5f),255);
          }
          if(count>card_->rows[row] && card_->spans[count-1].fill==a && card_->spans[count-1].outline==b)card_->spans[count-1].n++;
          else {
            if(count>=768)return; // bounded for this fixed shape; host comparison checks coverage
            card_->spans[count++]={static_cast<uint16_t>(col+7),1,a,b};
          }
        }
      }
      card_->rows[118]=count;card_->ready=true;
    }
    int lo=cy0_-35,hi=cy1_-35;if(lo<0)lo=0;if(hi>118)hi=118;
    for(int row=lo;row<hi;row++)for(unsigned i=card_->rows[row];i<card_->rows[row+1];i++) {
      const auto& span=card_->spans[i];
      if(span.fill)fillRect(span.x,row+35,span.n,1,fillColor,span.fill);
      if(span.outline)fillRect(span.x,row+35,span.n,1,outlineColor,span.outline);
    }
    return;
  }
  float r = radius;
  if (r > w * 0.5f) r = w * 0.5f;
  if (r > h * 0.5f) r = h * 0.5f;
  const float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
  int bx0 = static_cast<int>(floorf(x)) - 1, bx1 = static_cast<int>(ceilf(x + w)) + 1;
  int by0 = static_cast<int>(floorf(y)) - 1, by1 = static_cast<int>(ceilf(y + h)) + 1;
  if (bx0 < cx0_) bx0 = cx0_;
  if (by0 < cy0_) by0 = cy0_;
  if (bx1 > cx1_) bx1 = cx1_;
  if (by1 > cy1_) by1 = cy1_;
  const float deepInside = r - outlineWidth - 1.0f;                    // in the core and this far from the edge: plain fill
  for (int j = by0; j < by1; j++) {
    const float rowQ=fabsf(j+0.5f-cy)-hy;
    const float limit=deepInside<0.0f?deepInside:0.0f;
    int core0=bx1,core1=bx1;
    if (rowQ<0.0f && rowQ<deepInside && hx+limit>0.0f) {
      core0=static_cast<int>(floorf(cx-hx-limit))+1;
      core1=static_cast<int>(ceilf(cx+hx+limit))-1;
      if(core0<bx0)core0=bx0;
      if(core1>bx1)core1=bx1;
      if(core1>core0 && filled)fillRect(core0,j,core1-core0,1,fillColor,fillAlpha);
    }
    for (int i = bx0; i < bx1; i++) {
      if(i>=core0 && i<core1){i=core1-1;continue;}
      const float qx = fabsf(i + 0.5f - cx) - hx, qy = fabsf(j + 0.5f - cy) - hy;
      if (qx < 0.0f && qy < 0.0f && (qx > qy ? qx : qy) < deepInside) {
        if (filled) mix(i, j, fillColor, fillAlpha);
        continue;
      }
      const float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
      const float inside = (qx > qy ? qx : qy);
      const float distance=ox==0.0f?oy:oy==0.0f?ox:sqrtf(ox*ox+oy*oy);
      const float sd = distance + (inside < 0.0f ? inside : 0.0f) - r;  // <0 inside
      const float edge = clamp01(0.5f - sd);
      if (edge <= 0.0f) continue;
      if (filled) mix(i, j, fillColor, covAlpha(edge, fillAlpha));
      if (outlineWidth > 0.0f) mix(i, j, outlineColor, covAlpha(edge * clamp01(sd + outlineWidth + 0.5f), outlineAlpha));
    }
  }
}

// Scanline polygon fill: 4 sub-rows per pixel row, exact horizontal coverage. Even-odd rule.
void Canvas::polyAA(const float* xy, int n, Color c, uint8_t alpha) {
  if (!px_ || n < 3 || n > 12) return;
  float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
  for (int k = 1; k < n; k++) {
    if (xy[2 * k] < minx) minx = xy[2 * k];
    if (xy[2 * k] > maxx) maxx = xy[2 * k];
    if (xy[2 * k + 1] < miny) miny = xy[2 * k + 1];
    if (xy[2 * k + 1] > maxy) maxy = xy[2 * k + 1];
  }
  int bx0 = static_cast<int>(floorf(minx)), bx1 = static_cast<int>(ceilf(maxx));
  int by0 = static_cast<int>(floorf(miny)), by1 = static_cast<int>(ceilf(maxy));
  if (bx0 < cx0_) bx0 = cx0_;
  if (by0 < cy0_) by0 = cy0_;
  if (bx1 > cx1_) bx1 = cx1_;
  if (by1 > cy1_) by1 = cy1_;
  const int W = bx1 - bx0;
  if (W <= 0 || W > 512) return;
  float acc[512];
  for (int y = by0; y < by1; y++) {
    for (int i = 0; i < W; i++) acc[i] = 0.0f;
    bool any = false;
    for (int sj = 0; sj < 4; sj++) {
      const float sy = y + (sj + 0.5f) * 0.25f;
      float xs[12];
      int k = 0;
      for (int a = 0, b = n - 1; a < n; b = a++) {
        const float ay = xy[2 * a + 1], byv = xy[2 * b + 1];
        if ((ay > sy) != (byv > sy)) xs[k++] = xy[2 * a] + (sy - ay) * (xy[2 * b] - xy[2 * a]) / (byv - ay);
      }
      for (int i = 1; i < k; i++) {                        // insertion sort, k is tiny
        const float v = xs[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
        xs[j + 1] = v;
      }
      for (int i = 0; i + 1 < k; i += 2) {
        float xa = xs[i], xb = xs[i + 1];
        if (xb <= bx0 || xa >= bx1) continue;
        if (xa < bx0) xa = static_cast<float>(bx0);
        if (xb > bx1) xb = static_cast<float>(bx1);
        any = true;
        for (int px = static_cast<int>(floorf(xa)); px < static_cast<int>(ceilf(xb)); px++) {
          const float lo = xa > px ? xa : static_cast<float>(px), hi = xb < px + 1 ? xb : static_cast<float>(px + 1);
          if (hi > lo) acc[px - bx0] += (hi - lo) * 0.25f;
        }
      }
    }
    if (!any) continue;
    for (int i = 0; i < W; i++)
      if (acc[i] > 0.0f) mix(bx0 + i, y, c, covAlpha(acc[i], alpha));
  }
}

// ---- fonts (U8g2 compressed bitmap format) ---------------------------------------------

namespace {

struct FontInfo {
  const uint8_t* base;     // start of the font array
  uint8_t bits0, bits1, bitsW, bitsH, bitsX, bitsY, bitsD;
  int8_t capA;
  uint16_t upperA, lowerA;
};

constexpr int kFontHeader = 23;

FontInfo readInfo(const uint8_t* f) {
  FontInfo i;
  i.base = f;
  i.bits0 = f[2]; i.bits1 = f[3]; i.bitsW = f[4]; i.bitsH = f[5];
  i.bitsX = f[6]; i.bitsY = f[7]; i.bitsD = f[8];
  i.capA = static_cast<int8_t>(f[13]);
  i.upperA = static_cast<uint16_t>((f[17] << 8) | f[18]);
  i.lowerA = static_cast<uint16_t>((f[19] << 8) | f[20]);
  return i;
}

// Returns the glyph's bit stream (just past the 2-byte glyph header), or null.
const uint8_t* findGlyph(const FontInfo& fi, uint16_t enc) {
  const uint8_t* p = fi.base + kFontHeader;
  if (enc > 255) return nullptr;
  if (enc >= 'a') p += fi.lowerA;
  else if (enc >= 'A') p += fi.upperA;
  for (int guard = 0; guard < 600; guard++) {
    if (p[1] == 0) return nullptr;
    if (p[0] == enc) return p + 2;
    p += p[1];
  }
  return nullptr;
}

struct BitReader {
  const uint8_t* ptr;
  uint8_t pos;
  unsigned get(uint8_t cnt) {  // same algorithm as u8g2_font_decode_get_unsigned_bits
    unsigned val = *ptr;
    val >>= pos;
    unsigned end = pos + cnt;
    if (end >= 8) {
      ptr++;
      val |= static_cast<unsigned>(*ptr) << (8 - pos);
      end -= 8;
    }
    val &= (1U << cnt) - 1U;
    pos = static_cast<uint8_t>(end);
    return val;
  }
  int getSigned(uint8_t cnt) { return static_cast<int>(get(cnt)) - (1 << (cnt - 1)); }
};

}  // namespace

int fontCapHeight(const Font& f) { return f.data ? static_cast<int8_t>(f.data[13]) : 0; }

// Reads one character from a UTF-8 string and advances `s`. Anything above U+00FF becomes '?'.
static uint16_t nextChar(const char*& s) {
  const uint8_t b = static_cast<uint8_t>(*s);
  if (b < 0x80) { s++; return b; }
  if ((b & 0xE0) == 0xC0 && (static_cast<uint8_t>(s[1]) & 0xC0) == 0x80) {
    const uint16_t cp = static_cast<uint16_t>(((b & 0x1F) << 6) | (static_cast<uint8_t>(s[1]) & 0x3F));
    s += 2;
    return cp <= 255 ? cp : '?';
  }
  s++;
  return '?';
}

static int glyphAdvance(const FontInfo& fi, const uint8_t* g) {
  BitReader br{g, 0};
  br.get(fi.bitsW); br.get(fi.bitsH);
  br.getSigned(fi.bitsX); br.getSigned(fi.bitsY);
  return br.getSigned(fi.bitsD);
}

int textWidth(const Font& f, const char* s, int tracking) {
  if (!f.data || !s) return 0;
  const FontInfo fi = readInfo(f.data);
  int w = 0, chars = 0;
  while (*s) {
    const uint16_t enc = nextChar(s);
    const uint8_t* g = findGlyph(fi, enc);
    w += g ? glyphAdvance(fi, g) : 4;
    chars++;
  }
  return chars ? w + tracking * (chars - 1) : 0;
}

int Canvas::text(const Font& f, int x, int y, const char* s, Color c, int tracking, uint8_t alpha) {
  if (!px_ || !f.data || !s) return x;
  const FontInfo fi = readInfo(f.data);
  // Entirely above or below the clip (a strip, or a clip rectangle)? Only work out where the text ends.
  const int maxH = static_cast<int>(f.data[10]) + 2;
  if (y + maxH < cy0_ || y - maxH >= cy1_) {
    while (*s) {
      const uint16_t enc = nextChar(s);
      const uint8_t* g = findGlyph(fi, enc);
      x += (g ? glyphAdvance(fi, g) : 4) + tracking;
    }
    return x;
  }
  while (*s) {
    const uint16_t enc = nextChar(s);
    const uint8_t* g = findGlyph(fi, enc);
    if (!g) { x += 4 + tracking; continue; }
    BitReader br{g, 0};
    const int w = static_cast<int>(br.get(fi.bitsW));
    const int h = static_cast<int>(br.get(fi.bitsH));
    const int gx = br.getSigned(fi.bitsX);
    const int gy = br.getSigned(fi.bitsY);
    const int adv = br.getSigned(fi.bitsD);
    if (y-gy<=cy0_ || y-h-gy>=cy1_ || x+gx+w<=cx0_ || x+gx>=cx1_) {
      x+=adv+tracking; continue;
    }
    if (w > 0) {
      const int tx = x + gx, ty = y - (h + gy);
      int lx = 0, ly = 0;
      auto run = [&](unsigned len, bool fg) {
        while (len) {
          const int rem = w - lx;
          const int cur = static_cast<int>(len) < rem ? static_cast<int>(len) : rem;
          if (fg) fillRect(tx + lx, ty + ly, cur, 1, c, alpha);
          lx += cur;
          len -= cur;
          if (lx >= w) { lx = 0; ly++; }
        }
      };
      for (int guard = 0; guard < 8192 && ly < h && ty+ly<cy1_; guard++) {
        const unsigned a = br.get(fi.bits0), b = br.get(fi.bits1);
        do { run(a, false); run(b, true); } while (br.get(1) != 0 && ly < h);
      }
    }
    x += adv + tracking;
  }
  return x;
}

int Canvas::textAligned(const Font& f, Align a, int x, int y, const char* s, Color c, int tracking,
                        uint8_t alpha) {
  const int w = textWidth(f, s, tracking);
  if (a == Align::CENTER) x -= w / 2;
  else if (a == Align::RIGHT) x -= w;
  text(f, x, y, s, c, tracking, alpha);
  return x + w;
}

}  // namespace gfx
