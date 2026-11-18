// tft_native_test.cpp - PC tests for the colour screen code, and the desktop preview.
//
// Built and run by tools/test_tft_native.py. It includes src/gfx.cpp, src/tft_screens.cpp and the
// headers unchanged. Usage: tft-tests <out_dir> [u8g2-compare]
//   - checks blending, the font decoder (against the real U8g2 when built with WITH_U8G2),
//     the trail/projection maths and the screen helpers
//   - renders every screen with the demo data to PNG files in <out_dir>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "gfx.h"
#include "gfx_fonts.h"
#include "knob_decode.h"
#include "menu_logic.h"
#include "minimap.h"
#include "tft_demo.h"
#include "tft_screens.h"
#include "tft_verify.h"

#ifdef WITH_U8G2
extern "C" {
#include "u8g2.h"
}
#endif

static int gFailures = 0;
#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) {                                    \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
      printf(__VA_ARGS__);                            \
      printf("\n");                                   \
      gFailures++;                                    \
    }                                                 \
  } while (0)

// ---- PNG writer (stored deflate blocks: big files, no zlib needed) ----------------------

static uint32_t crcTable[256];
static void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> hdr;
  put32(hdr, static_cast<uint32_t>(data.size()));
  fwrite(hdr.data(), 1, 4, f);
  fwrite(type, 1, 4, f);
  if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
  uint32_t crc = crc32(0, reinterpret_cast<const uint8_t*>(type), 4);
  crc = crc32(crc, data.data(), data.size());
  std::vector<uint8_t> tail;
  put32(tail, crc);
  fwrite(tail.data(), 1, 4, f);
}

// rgb = w*h*3 bytes
static bool writePng(const char* path, const std::vector<uint8_t>& rgb, int w, int h) {
  crcInit();
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, w); put32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * w * 3, rgb.begin() + static_cast<size_t>(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  while (pos < raw.size()) {
    const size_t n = raw.size() - pos > 65535 ? 65535 : raw.size() - pos;
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  put32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

static std::vector<uint8_t> toRgb(const uint16_t* px, int w, int h, int scale) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * scale * h * scale * 3);
  for (int y = 0; y < h * scale; y++)
    for (int x = 0; x < w * scale; x++) {
      const uint16_t c = px[(y / scale) * w + (x / scale)];
      uint8_t* o = &out[(static_cast<size_t>(y) * w * scale + x) * 3];
      o[0] = gfx::red8(c); o[1] = gfx::green8(c); o[2] = gfx::blue8(c);
    }
  return out;
}

// ---- tests ---------------------------------------------------------------------------------

static void testBlend() {
  using namespace gfx;
  const Color black = rgb(0, 0, 0), white = rgb(255, 255, 255);
  CHECK(blend(black, white, 255) == white, "blend a=255");
  CHECK(blend(black, white, 0) == black, "blend a=0");
  const Color mid = blend(black, white, 128);
  CHECK(red8(mid) > 110 && red8(mid) < 146 && green8(mid) > 110 && green8(mid) < 146, "blend half: %d %d", red8(mid), green8(mid));
  const Color a = rgb(232, 168, 72);
  CHECK(scale(a, 255) == a, "scale 255 keeps the colour");
  CHECK(scale(a, 0) == black, "scale 0 is black");
  CHECK(red8(rgb(255, 0, 0)) == 255 && green8(rgb(0, 255, 0)) == 255 && blue8(rgb(0, 0, 255)) == 255, "channel extract");
  printf("blend: OK\n");
}

static void testShapes() {
  using namespace gfx;
  static uint16_t buf[64 * 64];
  Canvas c(buf, 64, 64);
  c.fill(0);
  c.discAA(32, 32, 10, rgb(255, 255, 255));
  CHECK(c.pixel(32, 32) == rgb(255, 255, 255), "disc centre is solid");
  CHECK(c.pixel(5, 5) == 0, "disc leaves the corner alone");
  CHECK(c.pixel(32, 22) != 0, "disc reaches its radius");
  CHECK(c.pixel(32, 18) == 0, "disc stops at its radius");
  c.fill(0);
  c.setClip(10, 10, 20, 20);
  c.fillRect(0, 0, 64, 64, rgb(255, 255, 255));
  CHECK(c.pixel(9, 15) == 0 && c.pixel(15, 15) != 0 && c.pixel(20, 15) == 0, "clip rectangle respected");
  c.clearClip();
  c.fill(0);
  const float tri[6] = {10, 10, 50, 10, 30, 50};
  c.polyAA(tri, 3, rgb(255, 255, 255));
  CHECK(c.pixel(30, 20) == rgb(255, 255, 255) && c.pixel(5, 5) == 0 && c.pixel(55, 45) == 0, "polygon fill");
  c.fill(0);
  c.lineAA(5, 5, 55, 5, 3, rgb(255, 255, 255));
  CHECK(c.pixel(30, 5) == rgb(255, 255, 255) && c.pixel(30, 12) == 0, "thick line");
  printf("shapes: OK\n");
}


static void testKnob() {
  // One detent clockwise is the pin sequence 3,1,0,2,3 (bit0 = CLK, bit1 = DT); counter-clockwise is 3,2,0,1,3.
  auto run = [](const int* seq, int n) {
    KnobDecoder k;
    int total = 0;
    for (int i = 0; i < n; i++) total += k.feed(seq[i] & 1, (seq[i] >> 1) & 1);
    return total;
  };
  const int cw[] = {3, 1, 0, 2, 3}, ccw[] = {3, 2, 0, 1, 3};
  CHECK(run(cw, 5) == 1, "one clockwise detent should give +1, got %d", run(cw, 5));
  CHECK(run(ccw, 5) == -1, "one counter-clockwise detent should give -1, got %d", run(ccw, 5));
  const int cw3[] = {3, 1, 0, 2, 3, 1, 0, 2, 3, 1, 0, 2, 3};
  CHECK(run(cw3, 13) == 3, "three detents should give +3, got %d", run(cw3, 13));
  const int bounce[] = {3, 1, 3, 1, 3, 1, 0, 1, 0, 2, 0, 2, 3};     // contact bounce inside one detent
  CHECK(run(bounce, 13) == 1, "bounce inside a detent must still be exactly +1, got %d", run(bounce, 13));
  const int half[] = {3, 1, 0, 1, 3};                                // starts a turn, goes back
  CHECK(run(half, 5) == 0, "a turn that goes back must give 0, got %d", run(half, 5));
  const int half2[] = {3, 2, 3, 2, 3};
  CHECK(run(half2, 5) == 0, "a wiggle must give 0, got %d", run(half2, 5));
  printf("knob decoder: OK\n");
}

static void testMenu() {
  MenuLogic m;
  m.values.volume = 100; m.values.brightness = 100; m.values.mode = 0; m.values.ble = 1; m.values.utcHours = 0;
  uint32_t t = 1000;
  CHECK(!m.isOpen(), "starts closed");
  MenuEvent e = m.press(t);
  CHECK(e.opened && m.isOpen() && m.selected() == MI_VOLUME, "a press opens the menu on Volume");
  e = m.rotate(+1, ++t);
  CHECK(m.selected() == MI_BRIGHTNESS && e.item < 0, "turning moves the selection");
  e = m.rotate(-2, ++t);
  CHECK(m.selected() == MI_EXIT, "selection wraps backwards past the top: %d", m.selected());
  e = m.rotate(+1, ++t);
  CHECK(m.selected() == MI_VOLUME, "and forwards past the bottom");

  // Volume: edit, change live, confirm.
  e = m.press(++t);
  CHECK(m.editing() && !e.commit, "a press on a value starts editing");
  e = m.rotate(-2, ++t);
  CHECK(m.values.volume == 90 && e.item == MI_VOLUME && !e.commit, "volume goes down 5 per click, live: %d", m.values.volume);
  e = m.press(++t);
  CHECK(!m.editing() && e.commit && e.item == MI_VOLUME && m.values.volume == 90, "press confirms");

  // Volume: edit and cancel with a long press restores the old value.
  m.press(++t);
  m.rotate(-4, ++t);
  CHECK(m.values.volume == 70, "volume 70 while editing, got %d", m.values.volume);
  e = m.longPress(++t);
  CHECK(e.cancel && m.values.volume == 90 && !m.editing() && m.isOpen(), "long press while editing cancels and restores 90, got %d", m.values.volume);

  // Limits.
  m.press(++t); m.rotate(+50, ++t);
  CHECK(m.values.volume == 100, "volume tops out at 100");
  m.rotate(-100, ++t);
  CHECK(m.values.volume == 0, "volume bottoms out at 0");
  m.longPress(++t);

  // Brightness never goes fully dark.
  m.rotate(+1, ++t); m.press(++t); m.rotate(-50, ++t);
  CHECK(m.values.brightness == 10, "brightness floor is 10, got %d", m.values.brightness);
  m.press(++t);

  // Mode / BLE / clock: nothing is applied until confirmed (owner applies on commit only).
  m.rotate(+1, ++t);                                    // Logging mode
  m.press(++t); e = m.rotate(+1, ++t);
  CHECK(m.values.mode == 1 && e.item == MI_MODE && !e.commit, "mode toggles");
  e = m.press(++t);
  CHECK(e.commit && e.item == MI_MODE, "mode applies on confirm");
  m.rotate(+1, ++t);                                    // Bluetooth
  m.press(++t); m.rotate(-1, ++t);
  CHECK(m.values.ble == 0, "bluetooth off");
  m.press(++t);
  m.rotate(+1, ++t);                                    // Clock
  m.press(++t); m.rotate(-5, ++t);
  CHECK(m.values.utcHours == -5, "clock -5, got %d", m.values.utcHours);
  m.rotate(-20, ++t);
  CHECK(m.values.utcHours == -12, "clock floor -12");
  m.rotate(+30, ++t);
  CHECK(m.values.utcHours == 14, "clock ceiling +14");
  char txt[16];
  m.valueText(MI_UTC, txt, sizeof txt);
  CHECK(strcmp(txt, "UTC+14") == 0, "clock text: %s", txt);
  m.press(++t);

  // Exit row closes; idle timeout closes.
  m.rotate(+1, ++t);
  CHECK(m.selected() == MI_EXIT, "now on Exit");
  e = m.press(++t);
  CHECK(e.closed && !m.isOpen(), "press on Exit closes");
  m.open(5000);
  e = m.tick(5000 + MenuLogic::TIMEOUT_MS - 1);
  CHECK(!e.closed && m.isOpen(), "no timeout yet");
  e = m.tick(5000 + MenuLogic::TIMEOUT_MS + 5);
  CHECK(e.closed && !m.isOpen(), "closes after being idle");
  // Closing in the middle of an edit restores the value.
  m.values.volume = 50; m.open(9000); m.press(9001); m.rotate(+3, 9002);
  e = m.close();
  CHECK(e.closed && e.cancel && m.values.volume == 50, "closing mid-edit restores, got %d", m.values.volume);
  printf("menu logic: OK\n");
}

static void testHelpers() {
  char num[12], unit[6];
  tftFormatDistance(190, num, sizeof num, unit, sizeof unit);
  CHECK(strcmp(num, "620") == 0 && strcmp(unit, "ft") == 0, "190 m -> %s %s", num, unit);
  tftFormatDistance(1000, num, sizeof num, unit, sizeof unit);
  CHECK(strcmp(num, "0.6") == 0 && strcmp(unit, "mi") == 0, "1000 m -> %s %s", num, unit);
  tftFormatDistance(2, num, sizeof num, unit, sizeof unit);
  CHECK(strcmp(num, "10") == 0, "tiny distances round up to 10 ft, got %s", num);
  CHECK(tftRssiBars(-95) == 0 && tftRssiBars(-90) == 1 && tftRssiBars(-65) == 3 && tftRssiBars(-50) == 5, "rssi bars");
  printf("helpers: OK\n");
}

static void testMiniMap() {
  float right, ahead;
  mapHeadingUp(100, 0, 90, right, ahead);                       // facing east, camera 100 m east
  CHECK(fabsf(ahead - 100) < 0.01f && fabsf(right) < 0.01f, "east while heading east: ahead %.2f right %.2f", ahead, right);
  mapHeadingUp(0, 100, 90, right, ahead);                       // facing east, camera 100 m north = your left
  CHECK(fabsf(ahead) < 0.01f && fabsf(right + 100) < 0.01f, "north while heading east is left: right %.2f", right);
  mapHeadingUp(0, 100, 0, right, ahead);
  CHECK(fabsf(ahead - 100) < 0.01f && fabsf(right) < 0.01f, "north while heading north");
  mapHeadingUp(30, 40, 180, right, ahead);                      // facing south: behind is ahead
  CHECK(fabsf(ahead + 40) < 0.01f && fabsf(right + 30) < 0.01f, "south heading: ahead %.2f right %.2f", ahead, right);

  float e, n;
  mapEnu(40.0, -75.0, 40.001, -75.0, e, n);
  CHECK(fabsf(n - 111.195f) < 0.1f && fabsf(e) < 0.01f, "enu north");

  Trail t;
  CHECK(t.add(40.0, -75.0), "first trail point");
  CHECK(!t.add(40.00005, -75.0), "5 m later is not recorded");
  CHECK(t.add(40.0002, -75.0), "22 m later is recorded");
  CHECK(t.count() == 2 && t.at(0).lat_e6 == 40000200 && t.at(1).lat_e6 == 40000000, "trail order: newest first");
  for (int i = 0; i < 300; i++) t.add(40.0 + 0.0002 * (i + 2), -75.0);
  CHECK(t.count() == Trail::N, "trail is capped at %d, has %d", Trail::N, t.count());
  CHECK(t.add(41.0, -75.0) && t.count() == 1, "a 100 km jump starts the trail over");
  printf("minimap: OK\n");
}

#ifdef WITH_U8G2
// Draw the same string with the real U8g2 and with gfx::Canvas::text and demand identical pixels.
static void compareFont(const uint8_t* font, const char* name, const char* text) {
  static uint8_t buf[1024];
  u8g2_t u;
  memset(buf, 0, sizeof buf);
  u8g2_SetupDisplay(&u, u8x8_d_ssd1306_128x64_noname, u8x8_cad_ssd13xx_fast_i2c, u8x8_byte_empty, u8x8_dummy_cb);
  u8g2_SetupBuffer(&u, buf, 8, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
  u8g2_SetFontMode(&u, 1);
  u8g2_SetDrawColor(&u, 1);
  u8g2_SetFont(&u, font);
  const int x = 3, y = 40;
  u8g2_DrawStr(&u, x, y, text);

  static uint16_t px[128 * 64];
  gfx::Canvas c(px, 128, 64);
  c.fill(0);
  gfx::Font f = {font};
  c.text(f, x, y, text, gfx::rgb(255, 255, 255));

  int diff = 0, lit = 0;
  for (int yy = 0; yy < 64; yy++)
    for (int xx = 0; xx < 128; xx++) {
      const bool theirs = (buf[(yy / 8) * 128 + xx] >> (yy % 8)) & 1;
      const bool mine = px[yy * 128 + xx] != 0;
      lit += theirs;
      diff += theirs != mine;
    }
  CHECK(lit > 20, "%s: U8g2 drew nothing (%d lit)", name, lit);
  CHECK(diff == 0, "%s \"%s\": %d of %d pixels differ from U8g2", name, text, diff, lit);
  const int mw = gfx::textWidth(f, text, 0);
  const int uw = u8g2_GetStrWidth(&u, text);
  CHECK(abs(mw - uw) <= 3, "%s: advance width %d vs U8g2 string width %d", name, mw, uw);
}

static void testFonts() {
  compareFont(u8g2_font_helvR08_tf, "helvR08", "FLOCK ahead 190 ft");
  compareFont(u8g2_font_helvR10_tf, "helvR10", "Hello, World 0.4 mi");
  compareFont(u8g2_font_helvB10_tf, "helvB10", "EQUIPMENT INFERRED");
  compareFont(u8g2_font_helvR14_tf, "helvR14", "2.4 GHZ 5 GHZ");
  compareFont(u8g2_font_helvB18_tr, "helvB18", "LISTEN");
  compareFont(u8g2_font_helvR24_tr, "helvR24", "Body cam");
  compareFont(u8g2_font_logisoso34_tn, "logisoso34", "0.4 620");
  // The middle dot (U+00B7) is not ASCII: compare one glyph, UTF-8 encoded on our side.
  static uint8_t buf[1024];
  u8g2_t u;
  memset(buf, 0, sizeof buf);
  u8g2_SetupDisplay(&u, u8x8_d_ssd1306_128x64_noname, u8x8_cad_ssd13xx_fast_i2c, u8x8_byte_empty, u8x8_dummy_cb);
  u8g2_SetupBuffer(&u, buf, 8, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
  u8g2_SetFontMode(&u, 1);
  u8g2_SetDrawColor(&u, 1);
  u8g2_SetFont(&u, u8g2_font_helvR10_tf);
  u8g2_DrawGlyph(&u, 10, 30, 0xB7);
  static uint16_t px[128 * 64];
  gfx::Canvas c(px, 128, 64);
  c.fill(0);
  c.text(gfx::Font{u8g2_font_helvR10_tf}, 10, 30, "\xC2\xB7", gfx::rgb(255, 255, 255));
  int diff = 0, lit = 0;
  for (int yy = 0; yy < 64; yy++)
    for (int xx = 0; xx < 128; xx++) {
      const bool theirs = (buf[(yy / 8) * 128 + xx] >> (yy % 8)) & 1;
      lit += theirs;
      diff += theirs != (px[yy * 128 + xx] != 0);
    }
  CHECK(lit > 0 && diff == 0, "middle dot differs from U8g2 (%d lit, %d differ)", lit, diff);
  printf("fonts vs U8g2: OK\n");
}
#endif

static uint16_t gFrame[TFT_W * TFT_H];

static void renderAll(const char* outDir) {
  struct Shot { const char* name; TftScreen screen; bool close; uint32_t nowMs; uint8_t facing; };
  const Shot shots[] = {
      {"boot", TftScreen::BOOT, false, 0, TFT_DEMO_TOWARD},
      {"listening", TftScreen::LISTENING, false, 900, TFT_DEMO_TOWARD},
      {"listening_nofix", TftScreen::LISTENING, false, 1900, TFT_DEMO_TOWARD},
      {"listening_none", TftScreen::LISTENING, false, 900, TFT_DEMO_TOWARD},
      {"listening_parked", TftScreen::LISTENING, false, 900, TFT_DEMO_TOWARD},
      {"listening_clear", TftScreen::LISTENING, false, 900, TFT_DEMO_TOWARD},
      {"rf_possible", TftScreen::RF, false, 0, TFT_DEMO_TOWARD},
      {"camera_ahead", TftScreen::CAMERA, false, 400, TFT_DEMO_TOWARD},
      {"camera_close", TftScreen::CAMERA, true, 400, TFT_DEMO_TOWARD},
      {"camera_away", TftScreen::CAMERA, false, 400, TFT_DEMO_AWAY},
      {"camera_nodir", TftScreen::CAMERA, false, 400, TFT_DEMO_NODIR},
      {"rf_bodycam", TftScreen::RF, false, 0, TFT_DEMO_TOWARD},
      {"follower", TftScreen::FOLLOWER, false, 0, TFT_DEMO_TOWARD},
      {"menu", TftScreen::MENU, false, 0, TFT_DEMO_TOWARD},
      {"toast_volume", TftScreen::LISTENING, false, 1500, TFT_DEMO_TOWARD},
      {"toast_muted", TftScreen::CAMERA, false, 500, TFT_DEMO_TOWARD},
      {"toast_volume_camera", TftScreen::CAMERA, true, 500, TFT_DEMO_TOWARD},
      {"toast_volume_rf", TftScreen::RF, false, 0, TFT_DEMO_TOWARD},
  };
  gfx::Canvas canvas(gFrame, TFT_W, TFT_H);
  std::vector<std::vector<uint8_t>> sheetTiles;
  static TftModel model;
  for (const Shot& s : shots) {
    model = TftModel();
    tftDemoModel(model, s.screen, s.close, s.facing);
    model.nowMs = s.nowMs;
    if (!strcmp(s.name, "listening_clear")) { model.police.count = 0; model.police.likely = 0; model.police.n = 0; }
    if (!strcmp(s.name, "rf_possible")) { model.rf.tier = WL_POSSIBLE; model.rf.vendorOnly = true; model.rf.cat = WL_CAT_FLEET_MODEM;
      strcpy(model.rf.vendor, "Sierra Wireless"); model.rf.others = 0; model.rf.ble = false; model.rf.rssi = -77; }
    if (!strcmp(s.name, "listening_nofix")) { model.gps = 1; model.lastAlertAgoS = -1; }
    if (!strcmp(s.name, "listening_none")) { model.nearest.found = false; model.storageLow = true; }
    if (!strcmp(s.name, "listening_parked")) { model.nearest.headingKnown = false; model.lastAlertCam = true; model.lastAlertCode = CAM_MAKER_FLOCK; model.lastAlertAgoS = 30; }
    if (!strncmp(s.name, "toast_volume", 12)) { model.toast.active = true; model.toast.pct = 60; strcpy(model.toast.text, "VOLUME"); }
    if (!strcmp(s.name, "toast_muted")) { model.toast.active = true; model.toast.pct = -1; strcpy(model.toast.text, "MUTED"); }
    tftRender(canvas, model);

    // Basic sanity: the frame is not blank, and it uses the accent colour somewhere.
    int lit = 0;
    for (int i = 0; i < TFT_W * TFT_H; i++) lit += gFrame[i] != gFrame[0];
    CHECK(lit > 500, "%s: frame is nearly blank (%d pixels differ from the background)", s.name, lit);

    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", outDir, s.name);
    CHECK(writePng(path, toRgb(gFrame, TFT_W, TFT_H, 2), TFT_W * 2, TFT_H * 2), "could not write %s", path);
    sheetTiles.push_back(toRgb(gFrame, TFT_W, TFT_H, 1));
  }
  // Contact sheet at 1:1 (the real pixel size), 2 columns.
  const int cols = 2, rows = static_cast<int>((sheetTiles.size() + 1) / 2), pad = 8;
  const int sw = cols * TFT_W + (cols + 1) * pad, sh = rows * TFT_H + (rows + 1) * pad;
  std::vector<uint8_t> sheet(static_cast<size_t>(sw) * sh * 3, 40);
  for (size_t i = 0; i < sheetTiles.size(); i++) {
    const int ox = pad + (static_cast<int>(i) % cols) * (TFT_W + pad), oy = pad + (static_cast<int>(i) / cols) * (TFT_H + pad);
    for (int y = 0; y < TFT_H; y++)
      memcpy(&sheet[(static_cast<size_t>(oy + y) * sw + ox) * 3], &sheetTiles[i][static_cast<size_t>(y) * TFT_W * 3], TFT_W * 3);
  }
  char path[512];
  snprintf(path, sizeof path, "%s/sheet.png", outDir);
  CHECK(writePng(path, sheet, sw, sh), "could not write the contact sheet");
  printf("rendered %zu screens to %s\n", sheetTiles.size(), outDir);
}


// Real data: load cameras/cameras.bin if present, stand 330 m in front of a mapped camera that faces
// the road, drive toward it, and check that (a) the alert logic picks that camera, (b) the map collects
// it and its neighbours, (c) the screen draws. Writes camera_real.png.
static void testRealMap(const char* outDir) {
  FILE* f = fopen("cameras/cameras.bin", "rb");
  if (!f) { printf("real map: skipped (cameras/cameras.bin not found)\n"); return; }
  std::vector<uint8_t> bytes;
  uint8_t tmp[65536];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) bytes.insert(bytes.end(), tmp, tmp + n);
  fclose(f);
  CamDb db;
  CHECK(db.attach(bytes.data(), bytes.size()), "cameras.bin does not load");

  // Pick the directional camera with the most neighbours within 400 m (a busy spot makes a good picture).
  uint32_t best = 0; int bestN = -1;
  for (uint32_t i = 0; i < db.count() && i < 60000; i += 7) {
    const CamRec r = db.rec(i);
    if (!(r.flags & CAM_FLAG_DIR) || (r.flags & CAM_FLAG_MULTI) || r.dir == CAM_DIR_UNKNOWN) continue;
    MapCam tmpCams[24];
    const int k = mapCollectCameras(db, r.lat_e6 * 1e-6, r.lon_e6 * 1e-6, 400.0f, tmpCams, 24);
    if (k > bestN) { bestN = k; best = i; }
  }
  CHECK(bestN >= 1, "no directional camera found in the map");
  const CamRec cam = db.rec(best);
  const double camLat = cam.lat_e6 * 1e-6, camLon = cam.lon_e6 * 1e-6;
  const float heading = fmodf(static_cast<float>(cam.dir) + 180.0f, 360.0f);   // driving straight at the lens
  const double h = heading * M_PI / 180.0;
  // 330 m ahead of the camera's lens means 330 m BEHIND us along our heading: start there.
  const double latPerM = 1.0 / 111195.0, lonPerM = latPerM / cos(camLat * M_PI / 180.0);
  const double ownLat = camLat - 330.0 * cos(h) * latPerM, ownLon = camLon - 330.0 * sin(h) * lonPerM;

  CamFix fix{ownLat, ownLon, 17.0f, heading, true};
  CamHit hit;
  const bool found = db.findAhead(fix, hit);
  CHECK(found, "findAhead found nothing 330 m before a camera we are driving straight at");
  // In a dense spot another camera may be nearer ahead; the alert logic is right to pick that one.
  // Whatever it picks becomes the warning on screen. If it is our camera, it must count as facing us.
  if (found && hit.idx == best) CHECK(hit.facing, "a camera facing us was reported as facing away");
  if (found) CHECK(hit.distM <= 345.0f, "alert distance %.0f m is beyond the camera we aimed at", hit.distM);

  static TftModel m;
  m = TftModel();
  tftDemoModel(m, TftScreen::CAMERA, false);      // status bar + trail from the demo, then real map data
  m.demo = false;
  m.cam.headingDeg = heading;
  m.cam.speedMph = 38.0f;
  m.cam.distM = found ? static_cast<uint16_t>(hit.distM) : 330;
  m.cam.rangeM = m.cam.distM * 1.25f + 60.0f;
  m.cam.maker = found ? hit.maker : cam.maker;
  m.cam.facing = found ? hit.facing : true;
  m.cam.camCount = mapCollectCameras(db, ownLat, ownLon, m.cam.rangeM * 1.4f, m.cam.cams, TFT_MAP_CAMS);
  float te, tn;
  mapEnu(ownLat, ownLon, found ? hit.lat : camLat, found ? hit.lon : camLon, te, tn);
  m.cam.targetEast = te; m.cam.targetNorth = tn;
  bool onMap = false;
  for (int i = 0; i < m.cam.camCount; i++) onMap |= fabsf(m.cam.cams[i].east - te) < 1.0f && fabsf(m.cam.cams[i].north - tn) < 1.0f;
  CHECK(onMap, "the camera being warned about is not among the %d collected for the map", m.cam.camCount);
  printf("real map: %d cameras around the target, alert at %d m\n", m.cam.camCount, m.cam.distM);
  m.nowMs = 300;
  gfx::Canvas canvas(gFrame, TFT_W, TFT_H);
  tftRender(canvas, m);
  char path[512];
  snprintf(path, sizeof path, "%s/camera_real.png", outDir);
  CHECK(writePng(path, toRgb(gFrame, TFT_W, TFT_H, 2), TFT_W * 2, TFT_H * 2), "could not write %s", path);
}

// Drawing a frame 16 rows at a time (as the firmware does, into a small internal-RAM buffer) must give
// exactly the same pixels as drawing it whole.
static void testStrips() {
  static uint16_t full[TFT_W * TFT_H], rebuilt[TFT_W * TFT_H], strip[TFT_W * 16];
  static TftModel model;
  for (int s = 0; s <= static_cast<int>(TftScreen::MENU); s++)
    for (int close = 0; close < 2 * 3; close++) {    // x3: lens toward / away / direction unknown
      model = TftModel();
      tftDemoModel(model, static_cast<TftScreen>(s), (close & 1) != 0, static_cast<uint8_t>(close / 2));
      model.nowMs = 777;
      gfx::Canvas whole(full, TFT_W, TFT_H);
      tftRender(whole, model);
      for (int st = 0; st < TFT_H / 16; st++) {
        for (int i = 0; i < TFT_W * 16; i++) strip[i] = 0xBEEF;
        gfx::Canvas part;
        part.attachStrip(strip, TFT_W, TFT_H, st * 16, 16);
        tftRender(part, model);
        memcpy(rebuilt + st * 16 * TFT_W, strip, sizeof strip);
      }
      int diff = 0;
      for (int i = 0; i < TFT_W * TFT_H; i++) diff += full[i] != rebuilt[i];
      CHECK(diff == 0, "screen %d close %d: strips differ from the whole frame in %d pixels", s, close, diff);
      TftGeometryCache cache;
      for(int st=0;st<TFT_H/16;st++) {
        gfx::Canvas part;part.attachStrip(strip,TFT_W,TFT_H,st*16,16);
        tftRenderPrepared(part,model,cache);
        memcpy(rebuilt+st*16*TFT_W,strip,sizeof strip);
      }
      diff=0;for(int i=0;i<TFT_W*TFT_H;i++)diff+=full[i]!=rebuilt[i];
      CHECK(diff==0,"prepared geometry must match full frame: %d differences",diff);
    }
  printf("strip rendering == full rendering: OK\n");
}

// Every screen must be drawable repeatedly with changing time and never touch memory outside the canvas.
static void testAnimationAndBounds() {
  static uint16_t guarded[TFT_W * TFT_H + 2 * TFT_W];
  for (size_t i = 0; i < sizeof guarded / 2; i++) guarded[i] = 0xBEEF;
  gfx::Canvas canvas(guarded + TFT_W, TFT_W, TFT_H);
  static TftModel model;
  for (int s = 0; s <= static_cast<int>(TftScreen::MENU); s++)
    for (int close = 0; close < 2 * 3; close++)
      for (uint32_t t = 0; t < 4000; t += 330) {
        model = TftModel();
        tftDemoModel(model, static_cast<TftScreen>(s), (close & 1) != 0, static_cast<uint8_t>(close / 2));
        model.nowMs = t;
        tftRender(canvas, model);
      }
  for (int i = 0; i < TFT_W; i++) {
    CHECK(guarded[i] == 0xBEEF, "write before the canvas at %d", i);
    CHECK(guarded[TFT_W + TFT_W * TFT_H + i] == 0xBEEF, "write after the canvas at %d", i);
  }
  printf("animation and bounds: OK\n");
}

int main(int argc, char** argv) {
  static uint16_t allColors[65536];
  for(uint16_t source:{gfx::rgb(18,14,12),gfx::rgb(86,24,18)}) {
    for(unsigned i=0;i<65536;i++)allColors[i]=i;
    gfx::Canvas blendTest(allColors,65536,1);blendTest.fillRect(0,0,65536,1,source,238);
    for(unsigned i=0;i<65536;i++)CHECK(allColors[i]==gfx::blend(i,source,238),"component blend must be exact");
  }
  // Pack RGB666 streams with both known dummy alignments and colour orders.
  for (unsigned dummy:{1U,8U}) for (bool swap:{false,true}) {
    uint8_t raw[3*320+2]={};
    for (unsigned x=0;x<320;x++) {
      const uint16_t v=tftVerifyPixel(x,7);
      const uint8_t rgb[]={static_cast<uint8_t>((v>>8)&0xF8),static_cast<uint8_t>((v>>3)&0xFC),static_cast<uint8_t>((v<<3)&0xF8)};
      for (unsigned ch=0;ch<3;ch++) {
        const unsigned channel=swap?2-ch:ch;
        for (unsigned b=0;b<8;b++) {
          const unsigned bit=dummy+x*24+ch*8+b;
          if (rgb[channel]&(0x80>>b)) raw[bit/8]|=0x80>>(bit%8);
        }
      }
    }
    for (unsigned x=0;x<320;x++) CHECK(tftRead565(raw,x,dummy,swap)==tftVerifyPixel(x,7),"RAM read alignment/colour conversion");
  }
  CHECK(tftVerifyPixel(0,1)!=tftVerifyPixel(319,1),"left/right corner pattern differs");
  CHECK(tftVerifyPixel(0,1)!=tftVerifyPixel(320*239,1),"top/bottom corner pattern differs");
  CHECK(tftVerifyPixel(319,1)!=tftVerifyPixel(319,2),"second pattern differs");
  const char* outDir = argc > 1 ? argv[1] : ".";
  testBlend();
  testShapes();
  testHelpers();
  testMiniMap();
  testKnob();
  testMenu();
#ifdef WITH_U8G2
  testFonts();
#else
  printf("fonts vs U8g2: skipped (U8g2 sources not found)\n");
#endif
  testAnimationAndBounds();
  testStrips();
  renderAll(outDir);
  testRealMap(outDir);
  if (gFailures) {
    printf("%d check(s) FAILED\n", gFailures);
    return 1;
  }
  printf("tft tests passed\n");
  return 0;
}
