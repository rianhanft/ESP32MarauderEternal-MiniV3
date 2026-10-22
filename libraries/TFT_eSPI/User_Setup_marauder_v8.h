// =============================================================================
// TFT_eSPI setup for the ESP32 Marauder V8 (JustCallMeKoKo), ESP32-C5.
// =============================================================================
// Marauder V8 = ESP32-C5 + 2.8" ILI9341 240x320 display + XPT2046 resistive touch.
// JCMK's own V8 setup is private; these pins were independently reverse-engineered
// from real hardware by two people who agree on the display bus:
//   * H4W9/ESP32_Library (voltmeter + brute-force scan, Apr 2026) — the same
//     contributor whose TFT_eSPI fork JCMK's official CI uses for C5 boards.
//   * skizzophrenic/SquachWatch-CYD (probed ILI9341 ID 0x9341 + XPT2046, Oct 2026).
// Confirmed pins: MOSI7 MISO2 SCLK6 CS23 DC24 RST=-1(EN) BL8(HIGH) SD_CS10.
// TOUCH_CS: H4W9 found GPIO3, skizzophrenic found GPIO9 — if touch does not
// respond, switch TOUCH_CS from 3 to 9 (display is unaffected either way).
// =============================================================================

// Section 1. Driver ----------------------------------------------------------
#define ILI9341_DRIVER

// V8 panel uses RGB colour order — without this, RED displays as BLUE.
#define TFT_RGB_ORDER TFT_BGR

// Section 2. Pins (ESP32-C5 FSPI bus, shared by TFT + SD + XPT2046 touch) -----
#define TFT_MISO  2
#define TFT_MOSI  7
#define TFT_SCLK  6

#define TFT_CS   23   // Display chip select
#define TFT_DC   24   // Data / Command
#define TFT_RST  -1   // Reset tied to EN via hardware — no GPIO

#define TFT_BL    8   // PWM backlight
#define TFT_BACKLIGHT_ON HIGH

#define TOUCH_CS  9   // XPT2046 resistive touch (HARDWARE-CONFIRMED on V8; H4W9 reported 3)

// Section 3. Fonts -----------------------------------------------------------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GFXFF
#define SMOOTH_FONT

// Section 4. SPI speed -------------------------------------------------------
#define SPI_FREQUENCY       20000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
