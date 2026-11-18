# Wiring - ESP32-S3 DevKitC-1 N16R8 capture logger

All pin numbers are GPIO numbers (the numbers printed on the board), defined in `include/pins.h`.

## Pin table

| Part | Part pin | ESP32-S3 pin | Notes |
|---|---|---|---|
| OLED (SSD1306 0.96", I2C) | VCC | **3V3** | 3.3 V only |
| | GND | **GND** | |
| | SDA | **GPIO 8** | |
| | SCL | **GPIO 9** | |
| Buzzer (piezo) | + | **GPIO 4** | active buzzer: set `BUZZER_ACTIVE 1` in pins.h |
| | - | **GND** | |
| *or* amp (PAM8302A / CJMCU-8302) + speaker | In+ (A+) | **GPIO 4** | instead of the buzzer, no code change |
| | In- (A-) | **GND** | |
| | Vin (2-5VDC) | **3V3** | the 5V header pin is input-only on this board (see below) |
| | Ground | **GND** | |
| | SD (shutdown) | nothing | tied to GND it mutes the amp |
| RGB LED (common-cathode) | R, via 220 ohm | **GPIO 7** | resistor on every colour leg |
| | G, via 220 ohm | **GPIO 6** | |
| | B, via 220 ohm | **GPIO 5** | if red/blue come out swapped, swap the two numbers in pins.h |
| | longest leg (common) | **GND** | no resistor needed on this leg |
| Pushbutton | one side | **GPIO 10** | internal pull-up, no resistor |
| | other side | **GND** | |

The LED is optional for Phase 1 (it shows status colours). The OLED, buzzer and button are the useful ones.

## Where the pins are on the board

Hold the board with the USB ports at the bottom. The **left header** (J1), top to bottom:

```
3V3  3V3  RST  4  5  6  7  15  16  17  18  8  3  46  9  10  11  12  13  14  5V  G
```

Everything we use is on this left header: `4 5 6 7` are consecutive near the top, `8` then `9` and `10` further down.

**Trap:** `3` and `46` sit between `8` and `9`. SDA (8) and SCL (9) are NOT next to each other. Never connect anything to 3, 46 (strapping pins), or to 0 and 45 on the right header.

Take 3V3 from either 3V3 pin at the top of the left header. GND is the `G` at the bottom of the left header, or any `G` on the right header. Run one wire from GND to a breadboard GND rail and share it.

Silkscreen labels can differ slightly between clones, so check the printed numbers on your board before powering up.

## Part-by-part

**OLED.** The 4 pins are labelled on the back or the board edge. Two layouts exist: `GND VCC SCL SDA` (most common) and `VCC GND SCL SDA`. Read your label; swapping VCC and GND can kill the display. Use male-to-female jumpers from the OLED pins to the breadboard/board.

**Amp + speaker (unit A).** The firmware's square-wave tone goes into In+. Turn the amp's
volume trimmer down before first power-up and raise it slowly; a 3.3 V square wave is loud.

On these dual-USB-C clone boards the header pin labelled **5Vin** is input-only: a diode keeps
USB power off it, so it measures ~0 V and cannot power the amp. Unit A runs the amp from 3V3
at low volume. That shares the board's regulator with the radio, so if the unit resets on a
beep (`info` shows `reset: BROWNOUT`), turn the volume down. For full volume, give the amp
real 5 V: bridge the board's IN-OUT solder pads (should connect the 5V pin to USB power,
unconfirmed on this exact board; then never feed 5V in while USB is plugged in) or use a
separate 5 V supply with a shared GND.

## Power

Measure 3V3 to GND once on every new supply. It should be 3.2-3.3 V.

- Unit A read **2.93 V** on a motherboard USB port (below the chip's 3.0 V minimum) and
  **3.21 V** on a phone charger with the same cable. Use a charger or power bank for logging
  sessions; the PC port is fine for a quick flash or log dump.
- The USB power goes through a diode and a 1117 regulator, which together need ~4.5 V in.
  Long or thin cables, front-panel ports and unpowered hubs can sag below that.

**Buzzer.** A bare piezo disc has two wires with no meaningful polarity. A buzzer with a `+` mark: `+` goes to GPIO 4. If it is an *active* buzzer (makes a tone when given plain 3.3 V) set `BUZZER_ACTIVE 1`. A bare piezo can be a bit quiet; that is normal.

**RGB LED.** Four legs: the longest is the common cathode. Typical order is `R, common, G, B`. Put the common leg on GND, and a 220 ohm resistor between each colour leg and its GPIO. If it turns out to be common-anode (colours light only when the common leg goes to 3V3 and the GPIO is LOW), set `LED_COMMON_CATHODE 0` and move the common leg to 3V3.

**Button.** Tactile buttons have 4 legs wired in pairs. Wire across the diagonal (opposite corners) so you always cross the switch. One side to GPIO 10, the other to GND.

## Bring-up order (do it in steps)

1. Board alone on USB: flash, open the monitor, confirm the boot check says `Flash 16MB OK`, `PSRAM 8MB OK`.
2. Add the OLED. The boot log prints `[i2c] devices: 0x3C`. If it says `none`, recheck SDA/SCL/VCC/GND.
3. Add the button. Type `status`, then press it: `[btn] MARK in s000x` appears in the monitor.
4. Add the buzzer (MARK press should beep) and the LED.

## Before the boards arrive (what you can do today)

- Read the OLED's back label and write down its pin order.
- Seat the OLED header, buzzer, button and LED on the breadboard so the only wires left to add are the 8 jumpers to the ESP32 (plus 3V3/GND). Put the three 220 ohm resistors on the LED colour legs.
- Link the breadboard GND rail to the common leg of the LED, the button's far side, and the buzzer `-`.
- Check the button with the multimeter in continuity mode: open normally, beeps/closed when pressed.
- Identify the RGB LED's long leg and confirm which colour is which later with the 3V3 + resistor trick (3V3 -> 220 ohm -> colour leg, common leg to GND).

Do not connect anything to the ESP32 until it is unplugged from USB.

## GPS module (ATGM336H, added in firmware 0.4.0)

| GPS module pin | Connect to | Note |
|---|---|---|
| VCC | 3V3 | The board's 3.3 V pin keeps the module's TX at a safe 3.3 V logic level. |
| GND | GND | |
| TX | GPIO 18 | The only data wire the firmware needs (it reads, never sends). |
| RX | GPIO 17 | Optional. Wired for later (update rate); the firmware does not drive it yet. |
| PPS | leave open | Pulse-per-second output, unused. |

Check the labels on your module's header; some boards print TX/RX from the module's point of view,
which is what the table above assumes (module TX -> ESP32 RX).

The firmware finds the baud rate itself (tries 9600, 38400, 115200, 4800, 19200, 57600), so you
do not need to know it. `gps` in the serial console shows what it found. Put the antenna with a
clear view of the sky (a windowsill is usually enough for a first fix; a cold start can take a
few minutes). The module's little coin cell keeps satellite data for fast restarts.
Keep the GPS antenna away from the ESP32's own antenna and the display wiring.

Bench test without satellites: `gps sim 42.0 -75.0 0 30` pretends you are driving north at 30 mph
from that point; load the camera map first (`python tools/push_cameras.py COM6`) and use a
coordinate near a mapped camera (`cam near` lists the closest ones once a position exists).

## Colour screen, knob and mute button (firmware 0.5.0)

All pins below are **proposed** (chosen to avoid every pin used above and the flash, PSRAM, strapping and USB
pins). They are not confirmed against real hardware: if your parts or wiring differ, change `include/pins.h`.
Nothing here is needed for the unit to work; with nothing connected the firmware simply reports
`[tft] not running` and the knob/button pins read "not pressed".

### 2.4" ILI9341 colour screen (SPI)

| Screen module pin | ESP32-S3 pin | Notes |
|---|---|---|
| VCC | **3V3** | 3.3 V. Check the module: some have a 5 V regulator input, but 3V3 works on the common red board |
| GND | **GND** | |
| CS | **GPIO 14** | |
| RESET | **GPIO 15** | or leave to the board reset and set `PIN_TFT_RST -1` |
| DC (RS) | **GPIO 16** | |
| SDI (MOSI) | **GPIO 11** | |
| SCK | **GPIO 12** | |
| SDO (MISO) | **GPIO 13** | optional: only used at boot to recognise the panel. If it is not wired, run `tft on` once |
| LED (backlight) | **GPIO 2** | PWM brightness. If the module's LED pin is wired to 3V3 you may also just connect it there and set `PIN_TFT_BL -1` |
| T_CLK, T_CS, T_DIN, T_DO, T_IRQ | nothing | touch panel: not used |
| SD_CS | **GPIO 21** (reserved) | full-size SD slot on the back (owner-confirmed 2026-11-15; this file used to say microSD): the firmware does not use it |
| SD_MOSI / SD_MISO / SD_SCK | share GPIO 11 / 13 / 12 | only if a card is ever used |

`tft` shows status. `tft demo camera 30` puts sample data on the screen for 30 s (marked DEMO) without GPS or
a radio hit. `python tools/tft_shot.py` takes a screenshot of what the unit is drawing, even with no screen wired
(run `tft on` first). Orientation or colours wrong (firmware 0.5.1+, no reflash, saved): `tft rot 3` (upside down),
`tft bgr 0` (red and blue swapped), `tft inv 1` (looks like a negative), `tft mhz 26` (noisy or torn picture),
`tft panel reset` (back to the `include/config.h` defaults). `tft` shows the current values.

**Controller:** the red "2.4 TFT SPI 240X320 V1.3" board bought 2026-11-13 is NOT an ILI9341: it reads ID `81 81 B3`
(ST7789 family) and answers `D3` with zeros. Firmware 0.5.2+ detects this at boot and uses an ST7789 start-up sequence;
`tft chip st` / `tft chip ili` forces either one. Symptom of the wrong one: a squashed, repeated picture in portrait that
covers only the top three quarters of the glass. `tft read <hex>` reads a panel register back (needs SDO wired).

### Knob (KY-040) and mute button

| Part | Part pin | ESP32-S3 pin |
|---|---|---|
| KY-040 | CLK | **GPIO 39** |
| | DT | **GPIO 40** |
| | SW (push) | **GPIO 41** |
| | + | **3V3** |
| | GND | **GND** |
| Mute button | one side | **GPIO 42** |
| | other side | **GND** |

Turn = volume (bar on screen). Press = settings menu. Mute button = mute on/off. In the menu: turn to move,
press to edit, turn to change, press to confirm, hold to cancel/leave; it closes by itself after 20 s and a
camera warning closes it at once. If the knob feels backwards, set `KNOB_REVERSE 1`.
Test without the parts: `knob cw 3`, `knob ccw`, `knob press`, `knob long`, `knob mute`; `knob` shows pin levels.
