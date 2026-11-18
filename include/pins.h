// pins.h - every GPIO the firmware touches lives here. Nothing else hard-codes a pin.
//
// Pins deliberately NOT used on the ESP32-S3 DevKitC-1 N16R8:
//   0, 3, 45, 46   boot strapping pins (a wrong level at reset changes how the chip boots)
//   26-32          internal SPI flash
//   33-37          octal PSRAM on the N16R8 (35, 36, 37 are the ones that bite on the header)
//   19, 20         native USB D-/D+
//   43, 44         UART0 TX/RX (the "UART" USB port and the serial monitor)
//   48 (or 38)     the board's own RGB LED (only ever switched off, see below)
#pragma once

// ---- OLED, SSD1306 128x64, I2C ----
// 8/9 are the Arduino-core default I2C pins for the S3 DevKitC-1.
// NOTE: on the header GPIO 3 and 46 sit BETWEEN 8 and 9 - SDA and SCL are not neighbours.
#define PIN_OLED_SDA 8
#define PIN_OLED_SCL 9

// ---- Buzzer ----
// 0 = passive piezo (we generate the tone), 1 = active buzzer (just on/off).
#define BUZZER_ACTIVE 0
#define PIN_BUZZER 4

// ---- RGB LED ----
// 1 = common-cathode (long leg to GND, GPIO drives HIGH to light). 0 = common-anode.
#define LED_COMMON_CATHODE 1
#define PIN_LED_R 7   // unit A has the LED turned around, so red/blue are 7/5 instead of 5/7
#define PIN_LED_G 6
#define PIN_LED_B 5

// ---- Board's own WS2812 RGB LED ----
// Not used; switched off at boot so it doesn't keep the demo sketch's colour (or glow on
// a dark dashboard). 48 on DevKitC-1 v1.0 and most clones, 38 on v1.1.
#define PIN_ONBOARD_RGB 48

// ---- GPS module (ATGM336H / NEO-6M style, NMEA over UART1) ----
// GPS TX -> PIN_GPS_RX is the only wire the firmware needs. PIN_GPS_TX is wired to the module's
// RX for future configuration (update rate) and is not driven today. 17/18 are plain free GPIOs.
#define PIN_GPS_RX 18
#define PIN_GPS_TX 17

// ---- Colour screen: 2.4" ILI9341, 240x320, 4-wire SPI (added in firmware 0.5.0) ----
// PROPOSED pins, picked to avoid everything above and the flash/PSRAM/strapping/USB pins. They are
// not confirmed against real hardware: change them here if your module or wiring differs.
// 11/12/13 are the S3's default SPI2 (FSPI) pins. SCK/MOSI/MISO are shared with the microSD slot
// that most of these boards carry on the back; the card gets its own chip-select (reserved below).
#define PIN_TFT_SCK  12
#define PIN_TFT_MOSI 11
#define PIN_TFT_MISO 13   // module "SDO". Only used to recognise the panel at boot; optional
#define PIN_TFT_CS   14
#define PIN_TFT_DC   16
#define PIN_TFT_RST  15   // -1 if the module's RESET is tied to the board reset / 3V3
#define PIN_TFT_BL   2    // backlight (module "LED"), driven with PWM; -1 if it is wired to 3V3
#define PIN_SD_CS    21   // reserved for the microSD card; nothing drives it yet

// ---- Volume/menu knob (KY-040 rotary encoder) and mute button (added in firmware 0.5.0) ----
// PROPOSED pins, free on the DevKitC-1 N16R8 header (39-42 are the JTAG pins, unused here).
// KY-040: CLK, DT, SW, "+" to 3V3, GND to GND (the module has its own pull-ups; the chip's are on too).
// Mute button: GPIO -> button -> GND, internal pull-up. Nothing connected = no events, no harm.
#define PIN_KNOB_CLK 39
#define PIN_KNOB_DT  40
#define PIN_KNOB_SW  41
#define PIN_MUTE_BTN 42

// ---- MARK / new-session button: GPIO -> button -> GND, internal pull-up ----
#define PIN_BUTTON 10
