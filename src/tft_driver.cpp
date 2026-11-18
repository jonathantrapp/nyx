// tft_driver.cpp - see tft_driver.h.
#include "tft_driver.h"
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "pins.h"
#include "tft_model.h"
#include "tft_verify.h"

static const spi_host_device_t kHost = SPI2_HOST;
static const size_t kChunkBytes = static_cast<size_t>(TFT_STRIP_ROWS) * TFT_W * 2;

static spi_device_handle_t gDev = nullptr;
static bool gBusUp = false, gRunning = false;
static TftProbe gProbe = TftProbe::NOT_TRIED;
static uint32_t gId = 0;
static uint16_t* gStrip = nullptr;   // where the screen code draws one strip (internal RAM)
static uint8_t* gBounce = nullptr;   // internal DMA-capable RAM: one chunk, byte-swapped for the panel
static uint8_t* gBounceOther=nullptr;
static spi_transaction_t gPixelTx[2]={};
static bool gPending[2]={};
static unsigned gNextPixel=0;
static uint32_t gFrames = 0, gFrameStartMs = 0, gFrameLastMs = 0;
static bool gBacklightReady = false;
static TftPanelCfg gCfg = {TFT_ROTATION, TFT_BGR != 0, TFT_INVERT != 0, TFT_SPI_HZ, TFT_DEFAULT_CHIP};

TftPanelCfg tftDriverDefaultConfig() { return {TFT_ROTATION, TFT_BGR != 0, TFT_INVERT != 0, TFT_SPI_HZ, TFT_DEFAULT_CHIP}; }
TftPanelCfg tftDriverConfig() { return gCfg; }
void tftDriverSetConfig(const TftPanelCfg& cfg) { gCfg = cfg; }

// D/C is set per transaction: user = 0 for a command byte, 1 for data.
static void IRAM_ATTR preTransfer(spi_transaction_t* t) {
  gpio_set_level(static_cast<gpio_num_t>(PIN_TFT_DC), static_cast<int>(reinterpret_cast<intptr_t>(t->user)));
}

static bool addDevice(int hz) {
  spi_device_interface_config_t dc = {};
  dc.clock_speed_hz = hz;
  dc.mode = 0;
  dc.spics_io_num = PIN_TFT_CS;
  dc.queue_size = 2;
  dc.pre_cb = preTransfer;
  return spi_bus_add_device(kHost, &dc, &gDev) == ESP_OK;
}

static uint32_t gSpiErrors = 0;
static esp_err_t gSpiLastErr = ESP_OK;
static void noteErr(esp_err_t e) { if (e != ESP_OK) { gSpiErrors++; gSpiLastErr = e; } }
static bool waitPixel() {
  spi_transaction_t* done=nullptr;
  const esp_err_t e=spi_device_get_trans_result(gDev,&done,portMAX_DELAY);noteErr(e);
  if(e!=ESP_OK)return false;
  for(unsigned i=0;i<2;i++)if(done==&gPixelTx[i])gPending[i]=false;
  return true;
}
static bool finishPixels() {
  while(gPending[0]||gPending[1])if(!waitPixel())return false;
  return true;
}

static void sendCmd(uint8_t cmd) {
  if(!finishPixels())return;
  spi_transaction_t t = {};
  t.length = 8;
  t.flags = SPI_TRANS_USE_TXDATA;
  t.tx_data[0] = cmd;
  t.user = reinterpret_cast<void*>(0);
  noteErr(spi_device_polling_transmit(gDev, &t));
}
// Command parameters. Up to 4 bytes travel inside the transaction itself; longer ones (gamma tables)
// are copied into the DMA-capable bounce buffer first. Never hand the SPI DMA a pointer into flash
// (the static tables in CMD()) or the stack.
static void sendData(const uint8_t* d, size_t n) {
  if(!finishPixels())return;
  if (!n) return;
  spi_transaction_t t = {};
  t.length = n * 8;
  t.user = reinterpret_cast<void*>(1);
  if (n <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, d, n);
  } else {
    if (!gBounce || n > kChunkBytes) { gSpiErrors++; return; }
    if (d != gBounce) memcpy(gBounce, d, n);   // pixel strips are already in the bounce buffer
    t.tx_buffer = gBounce;
  }
  noteErr(spi_device_polling_transmit(gDev, &t));
}
static void cmdData(uint8_t cmd, const uint8_t* d, size_t n) { sendCmd(cmd); sendData(d, n); }
#define CMD(c, ...)                                  \
  do {                                               \
    static const uint8_t p_[] = {__VA_ARGS__};       \
    cmdData(c, p_, sizeof(p_));                      \
  } while (0)

// Sends a read command and clocks in 4 bytes (MSB first) in the same chip-select.
static uint32_t readRaw32(uint8_t cmd) {
  // CS_KEEP_ACTIVE (command and reply in one chip-select) is only allowed while holding the bus.
  if (spi_device_acquire_bus(gDev, portMAX_DELAY) != ESP_OK) return 0;
  spi_transaction_t c = {};
  c.length = 8;
  c.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE;
  c.tx_data[0] = cmd;
  c.user = reinterpret_cast<void*>(0);
  spi_device_polling_transmit(gDev, &c);
  spi_transaction_t r = {};
  r.length = 32;
  r.rxlength = 32;
  r.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
  r.user = reinterpret_cast<void*>(1);
  spi_device_polling_transmit(gDev, &r);
  spi_device_release_bus(gDev);
  return (static_cast<uint32_t>(r.rx_data[0]) << 24) | (static_cast<uint32_t>(r.rx_data[1]) << 16) |
         (static_cast<uint32_t>(r.rx_data[2]) << 8) | r.rx_data[3];
}

// True if `want` (24 bits) appears in `raw` after a dummy phase of 0..8 clocks. Modules differ in
// whether the dummy is one clock or a whole byte, so every alignment is accepted.
static bool idMatches(uint32_t raw, uint32_t want, uint32_t mask) {
  for (int s = 0; s <= 8; s++)
    if (((raw >> (8 - s)) & mask) == (want & mask)) return true;
  return false;
}

// Which controller answered: ILI9341 "ID4" (0xD3) reads 00 93 41; ST7789 "RDDID" (0x04) reads 85 85 52.
static uint8_t gLastChipSeen = 0xFF;
static uint32_t gRawD3 = 0, gRaw04 = 0;
static uint32_t probeId() {
  gRawD3 = readRaw32(0xD3);
  gRaw04 = readRaw32(0x04);
  gLastChipSeen = 0xFF;
  if (idMatches(gRawD3, 0x009341, 0xFFFF)) { gLastChipSeen = TFT_CHIP_ILI9341; return 0x009341; }
  if (idMatches(gRaw04, 0x858552, 0xFFFFFF)) { gLastChipSeen = TFT_CHIP_ST7789; return 0x858552; }
  // Something answers on SDO, but not with the ILI9341 ID: not an ILI9341. The look-alike boards
  // carry ST7789-family controllers (seen 2026-11-13: D3 = 00000000, 04 = 40C0D9FF).
  if (gRawD3 != 0xFFFFFFFF && gRaw04 != 0xFFFFFFFF && gRaw04 != 0 && !idMatches(gRawD3, 0x009341, 0xFFFF)) {
    gLastChipSeen = TFT_CHIP_ST7789;
    return gRaw04 & 0xFFFFFF;
  }
  return gRawD3 & 0xFFFFFF;
}

static void hardReset() {
  if (PIN_TFT_RST < 0) return;
  pinMode(PIN_TFT_RST, OUTPUT);
  digitalWrite(PIN_TFT_RST, HIGH); delay(5);
  digitalWrite(PIN_TFT_RST, LOW);  delay(15);
  digitalWrite(PIN_TFT_RST, HIGH); delay(120);
}

// ST7789(V): a different controller sold on identical-looking red 2.4" boards. The ILI9341 sequence
// below must NOT be sent to it: its register 0xC0 (LCMCTRL) would XOR the landscape and scan bits.
static void initST7789() {
  sendCmd(0x01);                   // software reset
  delay(150);
  sendCmd(0x11);                   // sleep out
  delay(120);
  CMD(0x3A, 0x55);                 // 16 bits per pixel
  delay(10);
  // MADCTL: rot 1 = MX|MV (0x60), rot 3 = MY|MV (0xA0); bit 3 = BGR order.
  const uint8_t madctl = static_cast<uint8_t>((gCfg.rotation == 3 ? 0xA0 : 0x60) | (gCfg.bgr ? 0x08 : 0x00));
  cmdData(0x36, &madctl, 1);
  CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);   // porch
  CMD(0xB7, 0x35);                 // gate voltages
  CMD(0xBB, 0x19);                 // VCOM
  CMD(0xC0, 0x2C);                 // LCMCTRL: default, no XOR of MADCTL bits
  CMD(0xC2, 0x01);
  CMD(0xC3, 0x12);                 // VRH
  CMD(0xC4, 0x20);                 // VDV
  CMD(0xC6, 0x0F);                 // 60 Hz
  CMD(0xD0, 0xA4, 0xA1);           // power
  CMD(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23);
  CMD(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23);
  sendCmd(gCfg.invert ? 0x21 : 0x20);   // most ST7789 panels need inversion ON (the "chip st" default)
  sendCmd(0x13);                   // normal display mode
  delay(10);
  sendCmd(0x29);                   // display on
  delay(20);
}

static void initPanel() {
  if (gCfg.chip == TFT_CHIP_ST7789) { initST7789(); return; }
  sendCmd(0x01);   // software reset
  delay(130);
  CMD(0xEF, 0x03, 0x80, 0x02);
  CMD(0xCF, 0x00, 0xC1, 0x30);
  CMD(0xED, 0x64, 0x03, 0x12, 0x81);
  CMD(0xE8, 0x85, 0x00, 0x78);
  CMD(0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);
  CMD(0xF7, 0x20);
  CMD(0xEA, 0x00, 0x00);
  CMD(0xC0, 0x23);                 // power control 1
  CMD(0xC1, 0x10);                 // power control 2
  CMD(0xC5, 0x3E, 0x28);           // VCOM 1
  CMD(0xC7, 0x86);                 // VCOM 2
  const uint8_t madctl = (gCfg.rotation == 3 ? 0xE8 : 0x28) & (gCfg.bgr ? 0xFF : 0xF7);
  cmdData(0x36, &madctl, 1);       // memory access: landscape, colour order
  CMD(0x3A, 0x55);                 // 16 bits per pixel
  CMD(0xB1, 0x00, 0x18);           // frame rate 79 Hz
  CMD(0xB6, 0x08, 0x82, 0x27);     // display function
  CMD(0xF2, 0x00);                 // 3-gamma off
  CMD(0x26, 0x01);                 // gamma curve 1
  CMD(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
  CMD(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);
  sendCmd(gCfg.invert ? 0x21 : 0x20);
  sendCmd(0x11);                   // sleep out
  delay(130);
  sendCmd(0x29);                   // display on
  delay(20);
}

static void setFullWindow() {
  const uint8_t ca[4] = {0, 0, static_cast<uint8_t>((TFT_W - 1) >> 8), static_cast<uint8_t>((TFT_W - 1) & 0xFF)};
  const uint8_t pa[4] = {0, 0, static_cast<uint8_t>((TFT_H - 1) >> 8), static_cast<uint8_t>((TFT_H - 1) & 0xFF)};
  cmdData(0x2A, ca, 4);
  cmdData(0x2B, pa, 4);
  sendCmd(0x2C);                   // memory write: pixel data follows
}

// Raw panel command for live tuning ("tft cmd"). Waits for queued pixel strips first; the caller must
// hold the render task so nothing else uses the bus.
bool tftDriverWriteCmd(uint8_t cmd, const uint8_t* data, size_t n) {
  if (!gRunning || !gDev || n > 16) return false;
  if (!finishPixels()) return false;
  sendCmd(cmd);
  if (n) sendData(data, n);
  return true;
}

uint32_t tftDriverReadReg(uint8_t cmd) {
  if (!gRunning || !gDev) return 0;
  // Reads need a slow clock: swap the device for a 4 MHz one, read, swap back. The caller must make
  // sure nothing else is using the bus (tft_ui holds the render task).
  spi_bus_remove_device(gDev); gDev = nullptr;
  uint32_t v = 0;
  if (addDevice(4 * 1000 * 1000)) { v = readRaw32(cmd); spi_bus_remove_device(gDev); gDev = nullptr; }
  if (!addDevice(static_cast<int>(gCfg.hz))) { gRunning = false; gSpiErrors++; }
  return v;
}

// RAMRD is RGB666 regardless of the RGB565 write format (ST7789V datasheet 9.1.23).
// Clock changes happen only while the render task is held by tftUiVerify().
static bool verifyClock(uint32_t hz) {
  if(!finishPixels())return false;
  if (gDev) {
    const esp_err_t e=spi_bus_remove_device(gDev); noteErr(e);
    if (e!=ESP_OK) return false;
    gDev=nullptr;
  }
  if (addDevice(static_cast<int>(hz))) return true;
  gSpiErrors++; return false;
}
static bool verifyReadRow(int y) {
  const uint8_t ca[]={0,0,static_cast<uint8_t>((TFT_W-1)>>8),static_cast<uint8_t>(TFT_W-1)};
  const uint8_t pa[]={static_cast<uint8_t>(y>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8),static_cast<uint8_t>(y)};
  cmdData(0x2A,ca,sizeof(ca)); cmdData(0x2B,pa,sizeof(pa));
  esp_err_t e=spi_device_acquire_bus(gDev,portMAX_DELAY); noteErr(e);
  if (e!=ESP_OK) return false;
  spi_transaction_t cmd={}; cmd.length=8;
  cmd.flags=SPI_TRANS_USE_TXDATA|SPI_TRANS_CS_KEEP_ACTIVE; cmd.tx_data[0]=0x2E;
  e=spi_device_polling_transmit(gDev,&cmd); noteErr(e);
  if (e==ESP_OK) {
    spi_transaction_t rx={}; rx.length=(TFT_W*3+2)*8; rx.rxlength=rx.length;
    rx.rx_buffer=gBounce; rx.user=reinterpret_cast<void*>(1);
    e=spi_device_polling_transmit(gDev,&rx); noteErr(e);
  }
  spi_device_release_bus(gDev); return e==ESP_OK;
}
static void verifyWriteFrame(uint32_t seed,void (*service)()) {
  CMD(0x3A,0x55); tftFrameBegin();
  for (int y=0;y<TFT_H;y+=TFT_STRIP_ROWS) {
    for (int i=0;i<TFT_W*TFT_STRIP_ROWS;i++) gStrip[i]=tftVerifyPixel(static_cast<uint32_t>(y*TFT_W+i),seed);
    tftFrameRows(TFT_STRIP_ROWS);
    if (service) service();
  }
  tftFrameEnd();
}
bool tftDriverVerify(Print& out,void (*service)()) {
  if (!gRunning || !gDev || PIN_TFT_MISO<0) { out.println("[verify] needs a running panel and connected SDO"); return false; }
  const uint32_t savedHz=gCfg.hz, errorsBefore=gSpiErrors;
  const auto restore=[&]() {
    const bool ok=verifyClock(savedHz);
    if (ok) { CMD(0x3A,0x55); setFullWindow(); }
    else gRunning=false;
    out.printf(ok?"[verify] restored %lu MHz; settings not saved\n":"[verify] restore FAILED at %lu MHz; settings not saved\n",
        static_cast<unsigned long>(savedHz/1000000));
    return ok;
  };
  out.println("[verify] full 320x240 RAM test; two patterns/speed, reads at 4 MHz; not a visual/colour test");
  if (!verifyClock(10000000)) { restore(); return false; }
  verifyWriteFrame(1,service);
  if (!verifyClock(4000000)) { restore(); return false; }
  CMD(0x3A,0x66);
  unsigned dummy=0; bool swap=false;
  if (verifyReadRow(0)) {
    for (unsigned bits:{8U,1U}) for (bool rb:{false,true}) {
      bool equal=true;
      for (int x=0;x<TFT_W;x++) equal &= tftRead565(gBounce,x,bits,rb)==tftVerifyPixel(x,1);
      if (equal) { dummy=bits; swap=rb; }
    }
  }
  if (!dummy || gSpiErrors!=errorsBefore) {
    out.println("[verify] INCONCLUSIVE: no reliable RAM read-back baseline at 10 MHz. Check SDO/read support; no speed qualified.");
    restore(); return false;
  }
  out.printf("[verify] baseline OK: dummy_bits=%u read_rb_swap=%u\n",dummy,swap?1:0);
  uint32_t fastest=0, contiguous=0; bool cleanSoFar=true, complete=true;
  for (unsigned mhz:{10U,20U,26U,40U,80U}) {
    uint32_t mismatch=0; const uint32_t before=gSpiErrors;
    for (uint32_t seed:{1U,2U}) {
      if (!verifyClock(mhz*1000000UL)) { complete=false; break; }
      verifyWriteFrame(seed,service);
      if (!verifyClock(4000000)) { complete=false; break; }
      CMD(0x3A,0x66);
      for (int y=0;y<TFT_H;y++) {
        if (!verifyReadRow(y)) { mismatch+=TFT_W; }
        else for (int x=0;x<TFT_W;x++)
          mismatch+=tftRead565(gBounce,x,dummy,swap)!=tftVerifyPixel(static_cast<uint32_t>(y*TFT_W+x),seed);
        if (service && y%16==15) service();
      }
    }
    const bool clean=complete && !mismatch && gSpiErrors==before;
    if (clean) fastest=mhz;
    cleanSoFar &= clean; if (cleanSoFar) contiguous=mhz;
    out.printf("[verify] %u MHz: mismatches=%lu/153600 spi_errors=%lu %s\n",mhz,
        static_cast<unsigned long>(mismatch),static_cast<unsigned long>(gSpiErrors-before),clean?"PASS":"FAIL");
    if (!complete) break;
  }
  out.printf("[verify] fastest_clean=%lu MHz contiguous_clean=%lu MHz (this run only; eyes still needed)\n",
      static_cast<unsigned long>(fastest),static_cast<unsigned long>(contiguous));
  const bool restored=restore(); return restored && complete;
}
uint32_t tftDriverSpiErrors() { return gSpiErrors; }
int tftDriverSpiLastError() { return static_cast<int>(gSpiLastErr); }
void tftDriverRawIds(uint32_t& d3, uint32_t& r04) { d3 = gRawD3; r04 = gRaw04; }
bool tftDriverRunning() { return gRunning; }
TftProbe tftDriverProbe() { return gProbe; }
uint32_t tftDriverId() { return gId; }
uint16_t* tftStripBuffer() { return gRunning ? gStrip : nullptr; }
uint32_t tftFramesPushed() { return gFrames; }
uint32_t tftFrameLastMs() { return gFrameLastMs; }

void tftSetBacklight(uint8_t percent) {
  if (PIN_TFT_BL < 0) return;
  if (!gBacklightReady) {
    ledcAttach(PIN_TFT_BL, 25000, 8);   // 25 kHz: dimming stays invisible (eyes, cameras) and silent
    gBacklightReady = true;
  }
  if (percent > 100) percent = 100;
  ledcWrite(PIN_TFT_BL, static_cast<uint32_t>(255UL * percent / 100));
}

bool tftDriverBegin(bool force) {
  if (gRunning) return true;
  gProbe = TftProbe::NOT_TRIED;
  gId = 0;

  pinMode(PIN_TFT_DC, OUTPUT);
  digitalWrite(PIN_TFT_DC, HIGH);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_TFT_MOSI;
  bus.miso_io_num = PIN_TFT_MISO;
  bus.sclk_io_num = PIN_TFT_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = static_cast<int>(kChunkBytes);
  if (spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { gProbe = TftProbe::ERROR; return false; }
  gBusUp = true;

  // Slow device first: both controllers specify reads at under ~6.6 MHz.
  if (!addDevice(4 * 1000 * 1000)) { gProbe = TftProbe::ERROR; tftDriverEnd(); return false; }
  hardReset();
  gId = PIN_TFT_MISO >= 0 ? probeId() : 0;
  spi_bus_remove_device(gDev);
  gDev = nullptr;
  gProbe = gLastChipSeen != 0xFF ? TftProbe::FOUND : TftProbe::SILENT;
  if (gProbe == TftProbe::FOUND) gCfg.chip = gLastChipSeen;   // a conclusive ID beats the saved choice
  if (gProbe != TftProbe::FOUND && !force) { tftDriverEnd(); return false; }

  gBounce = static_cast<uint8_t*>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  gBounceOther=static_cast<uint8_t*>(heap_caps_malloc(kChunkBytes,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));
  gStrip = static_cast<uint16_t*>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!gBounce || !gBounceOther || !gStrip || !addDevice(static_cast<int>(gCfg.hz))) { gProbe = TftProbe::ERROR; tftDriverEnd(); return false; }
  memset(gStrip, 0, kChunkBytes);

  hardReset();
  initPanel();
  gRunning = true;
  return true;
}

void tftDriverEnd() {
  gRunning = false;
  if(!finishPixels())return; // never free memory that DMA might still own
  if (gDev) { spi_bus_remove_device(gDev); gDev = nullptr; }
  if (gBusUp) { spi_bus_free(kHost); gBusUp = false; }
  if (gStrip) { heap_caps_free(gStrip); gStrip = nullptr; }
  if (gBounce) { heap_caps_free(gBounce); gBounce = nullptr; }
  if(gBounceOther){heap_caps_free(gBounceOther);gBounceOther=nullptr;}
  if (gBacklightReady) { tftSetBacklight(0); }
}

void tftFrameBegin() {
  if (!gRunning) return;
  setFullWindow();
  gFrameStartMs = millis();
}

void tftFrameRows(int rows) {
  if (!gRunning || rows <= 0) return;
  if (rows > TFT_STRIP_ROWS) rows = TFT_STRIP_ROWS;
  const size_t px = static_cast<size_t>(rows) * TFT_W;
  const unsigned slot=gNextPixel;
  while(gPending[slot])if(!waitPixel())return;
  uint8_t* bounce=slot?gBounceOther:gBounce;
  for (size_t i = 0; i < px; i++) {          // the panel wants the high byte first
    const uint16_t v = gStrip[i];
    bounce[2 * i] = static_cast<uint8_t>(v >> 8);
    bounce[2 * i + 1] = static_cast<uint8_t>(v);
  }
  auto& tx=gPixelTx[slot];tx={};tx.length=px*16;tx.tx_buffer=bounce;tx.user=reinterpret_cast<void*>(1);
  const esp_err_t e=spi_device_queue_trans(gDev,&tx,portMAX_DELAY);noteErr(e);
  if(e==ESP_OK){gPending[slot]=true;gNextPixel=1-slot;}
}

void tftFrameEnd() {
  if(!finishPixels())return;
  gFrames++;
  gFrameLastMs = millis() - gFrameStartMs;
}
