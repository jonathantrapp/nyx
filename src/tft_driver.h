// tft_driver.h - ST7789 / ILI9341 colour screen over SPI, with explicit diagnostic read-back.
// There is no full-frame buffer: the screen is drawn 16 rows at a time into a small buffer in fast
// internal RAM (see gfx::Canvas::attachStrip) and each strip is sent straight to the panel.
// Unit A ST7789 RAM write/read sweep verified 2026-11-13 (0.5.3); appearance still needs eyes.
#pragma once
#include <Arduino.h>

// Result of looking for the panel.
enum class TftProbe : uint8_t { NOT_TRIED, FOUND, SILENT, ERROR };

// Starts the SPI bus and the panel. `force` skips the ID check (for modules whose SDO is not wired:
// the ID can't be read back, so the panel is assumed to be there). Returns true if the driver is
// running (panel found, or forced).
bool tftDriverBegin(bool force);
void tftDriverEnd();                   // release the bus, pins and buffers
bool tftDriverRunning();
TftProbe tftDriverProbe();             // what the last begin() saw
uint32_t tftDriverId();                // raw ILI9341 ID4 (0x009341 for a genuine one), 0 if unread

static const int TFT_STRIP_ROWS = 16;  // rows per strip; 240 / 16 = 15 strips per frame
uint16_t* tftStripBuffer();            // TFT_W x TFT_STRIP_ROWS RGB565, native byte order; null if not running

void tftFrameBegin();                  // start a frame: panel write window = the whole screen
void tftFrameRows(int rows);           // send the first `rows` rows of the strip buffer as the next rows
void tftFrameEnd();                    // frame complete (counts it, records the time)
uint32_t tftFramesPushed();
uint32_t tftFrameLastMs();             // start-to-end time of the last frame

void tftSetBacklight(uint8_t percent); // PWM on PIN_TFT_BL (no-op if the pin is -1)

// Panel settings that differ between modules. Defaults come from include/config.h (TFT_ROTATION,
// TFT_BGR, TFT_INVERT, TFT_SPI_HZ); tft_ui overrides them from saved settings ("tft rot|bgr|inv|mhz").
// Takes effect on the next tftDriverBegin().
static const uint8_t TFT_CHIP_ILI9341 = 0;
static const uint8_t TFT_CHIP_ST7789 = 1;   // look-alike controller on some red 2.4" boards
struct TftPanelCfg {
  uint8_t rotation;   // 1 = landscape, 3 = landscape upside down
  bool bgr;           // BGR bit in MADCTL (raw)
  bool invert;        // display inversion on (raw; ST7789 panels usually need it on)
  uint32_t hz;        // SPI write clock
  uint8_t chip;       // TFT_CHIP_*; replaced by what the ID read finds when SDO answers
};
bool tftDriverWriteCmd(uint8_t cmd, const uint8_t* data, size_t n);   // raw command + up to 16 data bytes (bus must be idle)
uint32_t tftDriverReadReg(uint8_t cmd);// 32 raw bits after a read command, at 4 MHz (bus must be idle)
bool tftDriverVerify(Print& out, void (*service)()); // caller owns bus/strip buffer; preserves saved settings
uint32_t tftDriverSpiErrors();          // failed SPI transactions since boot
int tftDriverSpiLastError();            // esp_err_t of the last failure
void tftDriverRawIds(uint32_t& d3, uint32_t& r04);   // raw 32-bit replies to 0xD3 and 0x04 at the last probe
TftPanelCfg tftDriverDefaultConfig();
TftPanelCfg tftDriverConfig();
void tftDriverSetConfig(const TftPanelCfg& cfg);
