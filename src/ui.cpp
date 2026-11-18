// ui.cpp - everything the human sees, hears or presses.
#include "ui.h"
#include "breadcrumbs.h"
#include <Wire.h>
#include <U8g2lib.h>
#include "config.h"
#include "pins.h"
#include "watchlist.h"
#include "detector.h"
#include "storage_estimate.h"
#include "tft_ui.h"

// ---- OLED ----------------------------------------------------------------------------
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C gDisp(U8G2_R0, U8X8_PIN_NONE);
static bool gDispOk = false;
static uint8_t gVolume=100, gBrightness=100;
static uint16_t gToneHz=0;

// Look for the OLED on the bus so a wiring mistake shows up in the serial log instead of
// as a mysteriously blank screen. Most modules are at 0x3C, some at 0x3D.
static uint8_t scanI2C() {
  uint8_t oled = 0;
  Serial.print("[i2c] devices:");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
      if (a == 0x3C || a == 0x3D) oled = a;
    }
  }
  Serial.println(oled ? "" : " (none - check OLED wiring)");
  return oled;
}

// ---- Buzzer --------------------------------------------------------------------------
// Passive piezo: the ESP32's LEDC PWM generates a square wave at the note frequency.
// Active buzzer: it makes its own tone, so we only switch it on and off.
#if !BUZZER_ACTIVE
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    static void toneInit() { ledcAttach(PIN_BUZZER, 2000, 10); }
    static void toneSet(uint16_t hz) {
      gToneHz=hz;
      ledcWriteTone(PIN_BUZZER,hz);
      ledcWrite(PIN_BUZZER,hz ? (511UL*gVolume/100) : 0);
    }
  #else
    static const uint8_t kBuzzerChannel = 0;
    static void toneInit() { ledcSetup(kBuzzerChannel, 2000, 10); ledcAttachPin(PIN_BUZZER, kBuzzerChannel); }
    static void toneSet(uint16_t hz) {
      gToneHz=hz;
      ledcWriteTone(kBuzzerChannel,hz);
      ledcWrite(kBuzzerChannel,hz ? (511UL*gVolume/100) : 0);
    }
  #endif
#else
  static void toneInit() { pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW); }
  static void toneSet(uint16_t hz) { gToneHz=hz; digitalWrite(PIN_BUZZER, hz && gVolume ? HIGH : LOW); }
#endif

struct Note { uint16_t hz; uint16_t ms; };  // hz 0 = rest; ms 0 = end of pattern
static const Note kBoot[]    = {{1500, 50}, {0, 30}, {2200, 50}, {0, 0}};
static const Note kMark[]    = {{2400, 70}, {0, 0}};
static const Note kNewSess[] = {{1800, 80}, {0, 40}, {2600, 100}, {0, 0}};
static const Note kError[]   = {{300, 400}, {0, 0}};
// Detection patterns, one per category so you can tell them apart without looking.
static const Note kCamera[]  = {{3000, 80}, {0, 60}, {3000, 80}, {0, 60}, {3000, 80}, {0, 0}};  // 3 short high
static const Note kBodycam[] = {{1500, 250}, {0, 120}, {1500, 250}, {0, 0}};                    // 2 long
static const Note kFleet[]   = {{900, 150}, {0, 50}, {1800, 200}, {0, 0}};                      // low-high
static const Note kOther[]   = {{2000, 350}, {0, 0}};                                           // 1 long

static Note gCustom[] = {{0, 0}, {0, 0}};  // filled by uiTone()

static const Note* gPattern = nullptr;
static uint8_t gNoteIdx = 0;
static uint32_t gNoteEndMs = 0;

void uiBeep(Beep b) {
  switch (b) {
    case Beep::BOOT:        gPattern = kBoot; break;
    case Beep::MARK:        gPattern = kMark; break;
    case Beep::NEW_SESSION: gPattern = kNewSess; break;
    case Beep::ERROR:       gPattern = kError; break;
  }
  gNoteIdx = 0;
  gNoteEndMs = 0;  // start immediately on the next uiService()
}

void uiBeepCategory(uint8_t cat) {
  switch (cat) {
    case WL_CAT_CAMERA:      gPattern = kCamera; break;
    case WL_CAT_BODYCAM:     gPattern = kBodycam; break;
    case WL_CAT_FLEET_MODEM: gPattern = kFleet; break;
    default:                 gPattern = kOther; break;
  }
  gNoteIdx = 0;
  gNoteEndMs = 0;
}

void uiTone(uint16_t hz, uint16_t ms) {
  gCustom[0] = {hz, ms};
  gPattern = gCustom;
  gNoteIdx = 0;
  gNoteEndMs = 0;
}

static void serviceBuzzer(uint32_t now) {
  if (!gPattern) return;
  if (gNoteEndMs != 0 && static_cast<int32_t>(now - gNoteEndMs) < 0) return;  // note still playing
  const Note& n = gPattern[gNoteIdx];
  if (n.ms == 0) { toneSet(0); gPattern = nullptr; return; }
  toneSet(n.hz);
  gNoteEndMs = now + n.ms;
  gNoteIdx++;
}

// ---- LED -----------------------------------------------------------------------------
static LedMode gLedMode = LedMode::OFF;
static uint32_t gFlashUntilMs = 0;

// Brightness: OFF hides the "alive" heartbeat (alerts/errors still show, dimmed),
// DIM is the default (easy on the eyes in a dark room/car), ON is full brightness.
static LedLevel gLedLevel = LedLevel::DIM;
static constexpr uint8_t kDimDuty = 12;      // of 255: heartbeat in DIM
static constexpr uint8_t kAlertDimDuty = 60; // alerts/errors when DIM or OFF

static void ledPwm(uint8_t pin, uint8_t duty) {
  analogWrite(pin, LED_COMMON_CATHODE ? duty : 255 - duty);
}

static void ledWriteLevel(bool r, bool g, bool b, uint8_t duty) {
  ledPwm(PIN_LED_R, r ? duty : 0);
  ledPwm(PIN_LED_G, g ? duty : 0);
  ledPwm(PIN_LED_B, b ? duty : 0);
}

// Alerts and errors: full in ON, dimmed otherwise (never hidden).
static void ledWrite(bool r, bool g, bool b) {
  ledWriteLevel(r, g, b, gLedLevel == LedLevel::ON ? 255 : kAlertDimDuty);
}

void uiSetLed(LedMode m) { gLedMode = m; }
void uiSetLedLevel(LedLevel l) { gLedLevel = l; }
LedLevel uiLedLevel() { return gLedLevel; }
void uiFlashBlue(uint16_t ms) { gFlashUntilMs = millis() + ms; }

static void serviceLed(uint32_t now) {
  if (static_cast<int32_t>(gFlashUntilMs - now) > 0) { ledWrite(false, false, true); return; }
  switch (gLedMode) {
    case LedMode::OFF:   ledWrite(false, false, false); break;
    case LedMode::OK: {  // green heartbeat, the "I'm alive" blink
      const bool beat = gLedLevel != LedLevel::OFF && (now % 2000) < 80;
      ledWriteLevel(false, beat, false, gLedLevel == LedLevel::ON ? 255 : kDimDuty);
      break;
    }
    case LedMode::WARN:  ledWrite(true, true, false); break;                // yellow steady
    case LedMode::ERROR: ledWrite(true, false, false); break;               // red steady
    case LedMode::DETECT_POSSIBLE: ledWrite(false, false, (now % 1000) < 500); break;   // blue slow blink
    case LedMode::DETECT_LIKELY:   ledWrite((now % 500) < 250, (now % 500) < 250, false); break;  // yellow blink
    case LedMode::DETECT_HIGH:     ledWrite((now % 250) < 125, false, false); break;    // red fast blink
  }
}

// ---- Button --------------------------------------------------------------------------
// Short press fires on release; long press fires the moment the hold time is reached
// (so you get feedback while still holding) and the later release is ignored.
static ButtonEvent serviceButton(uint32_t now) {
  static bool lastRaw = false, stable = false, longFired = false;
  static uint32_t lastChangeMs = 0, pressStartMs = 0;
  ButtonEvent ev = ButtonEvent::NONE;

  const bool raw = digitalRead(PIN_BUTTON) == LOW;  // wired to GND: pressed = LOW
  if (raw != lastRaw) { lastRaw = raw; lastChangeMs = now; }
  if (now - lastChangeMs >= BUTTON_DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable) { pressStartMs = now; longFired = false; }
    else if (!longFired) ev = ButtonEvent::SHORT_PRESS;
  }
  if (stable && !longFired && now - pressStartMs >= BUTTON_LONG_PRESS_MS) {
    longFired = true;
    ev = ButtonEvent::LONG_PRESS;
  }
  return ev;
}

// ---- Public --------------------------------------------------------------------------
void uiBegin() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledWrite(false, false, false);
  rgbLedWrite(PIN_ONBOARD_RGB, 0, 0, 0);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  toneInit();

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);
  const uint8_t addr = scanI2C();
  if (addr) {
    gDisp.setI2CAddress(addr << 1);  // U8g2 wants the 8-bit form
    gDisp.begin();
    gDispOk = true;
  }
}

bool uiDisplayFound() { return gDispOk; }
void uiSetVolume(uint8_t percent) { gVolume=percent>100?100:percent; toneSet(gToneHz); }
uint8_t uiVolume() { return gVolume; }
void uiSetBrightness(uint8_t percent) {
  gBrightness=percent>100?100:percent;
  if (gDispOk) gDisp.setContrast(static_cast<uint8_t>(255UL*gBrightness/100));
  tftUiSetBrightness(gBrightness);  // the colour screen's backlight follows the same setting
}
uint8_t uiBrightness() { return gBrightness; }

static void smallText(int x,int y,const char* text) {
  gDisp.setFont(u8g2_font_5x8_tr); gDisp.drawStr(x,y,text);
}
static void footer(bool muted,bool storageIssue,uint32_t estimatedMinutes) {
  char text[24]; snprintf(text,sizeof(text),muted?"MUTED":"VOL %u",gVolume);
  smallText(0,63,text);
  char estimate[24]; storageEstimateText(estimate,sizeof(estimate),estimatedMinutes);
  const char* right=storageIssue?"CHECK LOG":estimate;
  smallText(128-static_cast<int>(strlen(right))*5,63,right);
}
static char gGpsText[10] = "";
void uiSetGpsStatus(const char* text) { strlcpy(gGpsText,text?text:"",sizeof(gGpsText)); }
// 1500 ft and 0.3 mi style distances for a US driver.
static void formatDistance(uint16_t m,char* out,size_t n) {
  const float ft=m*3.281f;
  if (ft<1000.0f) snprintf(out,n,"%d ft",static_cast<int>((ft+5)/10)*10);
  else snprintf(out,n,"%.1f mi",m/1609.34f);
}
void uiShowCamera(const char* maker,uint16_t distM,bool close,bool facing,bool muted) {
  BreadcrumbScope stage(Crumb::UiDraw); if (!gDispOk) return;
  gDisp.clearBuffer(); gDisp.setDrawColor(1);
  smallText(0,8,close?"MAP: CAMERA CLOSE":"MAP: CAMERA AHEAD"); gDisp.drawHLine(0,12,128);
  gDisp.setFont(u8g2_font_10x20_tr); gDisp.drawStr(0,36,maker?maker:"ALPR");
  char dist[12]; formatDistance(distM,dist,sizeof(dist));
  gDisp.drawStr(128-gDisp.getStrWidth(dist),36,dist);
  smallText(0,47,facing?"LENS TOWARD YOU":"LENS FACING AWAY");
  // The distance bar fills as you close in (0 at 900 m, full at 0).
  const int fill=distM>=900?0:(900-distM)*126/900;
  gDisp.drawFrame(0,50,128,5); gDisp.drawBox(1,51,fill,3);
  smallText(0,63,"MAPPED, NOT A RADIO HIT");
  if (muted) smallText(103,63,"MUTE");
  gDisp.sendBuffer();
}
void uiShowHome(const char* mode,bool ble,bool muted,bool fsOk,bool writeError,bool pressure,
                uint32_t freeBytes,uint32_t estimatedMinutes) {
  BreadcrumbScope stage(Crumb::UiDraw); if (!gDispOk) return;
  gDisp.clearBuffer(); gDisp.setDrawColor(1);
  smallText(0,8,"RX");
  if (gGpsText[0]) smallText(18,8,gGpsText);
  char top[24]; snprintf(top,sizeof(top),"%s  BLE %s",strcmp(mode,"car")==0?"CAR":"RES",ble?"ON":"OFF");
  smallText(128-static_cast<int>(strlen(top))*5,8,top);
  const char* title=!fsOk?"NO STORAGE":writeError?"WRITE ERROR":pressure?"LOW STORAGE":"LISTENING";
  gDisp.setFont(u8g2_font_10x20_tr);
  gDisp.drawStr((128-gDisp.getStrWidth(title))/2,34,title);
  smallText(0,48,!fsOk?"LOG OFF":writeError?"LOG ERROR":pressure?"LOG LOW":"LOG OK");
  char line[24]; snprintf(line,sizeof(line),"%.1fMB FREE",freeBytes/1048576.0f);
  smallText(128-static_cast<int>(strlen(line))*5,48,line);
  gDisp.drawHLine(0,53,128); footer(muted,!fsOk || writeError || pressure,estimatedMinutes); gDisp.sendBuffer();
}
void uiShowAlert(const DetectView& v,bool muted,bool storageIssue) {
  BreadcrumbScope stage(Crumb::UiDraw); if (!gDispOk) return;
  gDisp.clearBuffer(); gDisp.setDrawColor(1);
  // The source is visible on every alert. No percentages or threat-free claims.
  smallText(0,8,"RF: EQUIPMENT INFERRED"); gDisp.drawHLine(0,12,128);
  gDisp.setFont(u8g2_font_10x20_tr); gDisp.drawStr(0,36,"RF");
  smallText(0,47,v.ble?"BLE":"WIFI"); gDisp.drawVLine(28,18,30);
  static const char* titles[]={"CAMERA","ACOUSTIC","BODY CAM","ROUTER","GEAR","TACTICAL","TRACKER"};
  if (v.cat==WL_CAT_FLEET_MODEM) smallText(35,23,"FLEET");
  gDisp.setFont(u8g2_font_10x20_tr);
  gDisp.drawStr(35,v.cat==WL_CAT_FLEET_MODEM?43:38,v.cat<WL_CAT_COUNT?titles[v.cat]:"RF LEAD");
  char vendor[18]; snprintf(vendor,sizeof(vendor),"%.17s",v.vendor?v.vendor:"Unknown"); smallText(0,55,vendor);
  // Fixed RSSI thresholds, not confidence or distance. Full dBm remains in CSV.
  const int thresholds[]={-90,-80,-70,-60,-50};
  for (int i=0;i<5;i++) {
    const int h=v.rssi>=thresholds[i]?3+i*2:1;
    gDisp.drawBox(95+i*7,55-h,4,h);
  }
  smallText(0,63,storageIssue?"CHECK LOG":v.tier>=WL_LIKELY?"UNVERIFIED":"POSSIBLE / UNVERIFIED");
  if (muted && !storageIssue && v.tier>=WL_LIKELY) smallText(103,63,"MUTE");
  gDisp.sendBuffer();
}

// Layout for a 0.96" screen: one big line (10x20 font, 12 chars) and two bold lines
// (7x13B, 18 chars). An inverted title is the "look at me" state for alerts. barPct >= 0
// draws a signal-strength bar on the right half of line 3.
void uiShow3(const char* big, const char* line2, const char* line3, int barPct, bool invertTitle) {
  BreadcrumbScope stage(Crumb::UiDraw);
  if (!gDispOk) return;
  gDisp.clearBuffer();
  gDisp.setFont(u8g2_font_10x20_tr);
  if (invertTitle) {
    gDisp.drawBox(0, 0, 128, 20);
    gDisp.setDrawColor(0);
  }
  gDisp.drawStr(2, 16, big ? big : "");
  gDisp.setDrawColor(1);

  gDisp.setFont(u8g2_font_7x13B_tr);
  gDisp.drawStr(0, 38, line2 ? line2 : "");
  gDisp.drawStr(0, 60, line3 ? line3 : "");
  if (barPct >= 0) {
    const int w = constrain(barPct, 0, 100) * 58 / 100;
    gDisp.drawFrame(68, 49, 60, 13);
    gDisp.drawBox(69, 50, w, 11);
  }
  gDisp.sendBuffer();
}

ButtonEvent uiService() {
  BreadcrumbScope stage(Crumb::UiService);
  const uint32_t now = millis();
  serviceBuzzer(now);
  serviceLed(now);
  return serviceButton(now);
}
