// =============================================================================
// TFT_eSPI setup for the ESP32 Marauder V8 (JustCallMeKoKo), ESP32-C5.
// =============================================================================
// CONFIRMED hardware (official source + JCMK wiki "Marauder v8" page):
//   * Panel driver : ILI9341   (NOT ST7796 — that is the Pancake/Waveshare class)
//   * Resolution   : 240 x 320 (2.8" display), portrait, SCREEN_ORIENTATION 0
//   * Touch        : XPT2046 resistive over SPI (TFT_eSPI built-in setTouch);
//                    firmware uses the HAS_ILI9341 resistive path, NOT FT6336.
//   * Backlight    : plain GPIO driven by ESP32 LEDC PWM (no I/O expander).
//   * I2C (aux/batt): SCL=GPIO4, SDA=GPIO5  -> TFT/touch pins must AVOID 4 & 5.
//
// !!! PINS ARE NOT YET KNOWN !!!
// The exact V8 GPIO pin map (MOSI/MISO/SCLK/CS/DC/RST/BL + TOUCH_CS) lives ONLY
// in JCMK's PRIVATE fork justcallmekoko/TFT_eSPI_JCMK and is not published in
// any public source (verified: main repo, the public TFT_eSPI fork branches,
// H4W9/TFT_eSPI ESP32-C5, the wiki, and web). The values below are PLACEHOLDERS
// and WILL produce a black screen until replaced with the real V8 wiring from
// the private User_Setup or the V8 schematic. Do not flash a build made with
// these placeholder pins.
// =============================================================================

// Section 1. Driver ----------------------------------------------------------
#define ILI9341_DRIVER

// Section 2. Geometry --------------------------------------------------------
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

// Section 3. Pins  ----  <<< REPLACE ALL OF THESE WITH THE REAL V8 VALUES >>> --
// Placeholders only. Must not collide with I2C (GPIO4/GPIO5) on the V8.
#define TFT_MISO  -1   // PLACEHOLDER — real V8 value needed
#define TFT_MOSI  -1   // PLACEHOLDER — real V8 value needed
#define TFT_SCLK  -1   // PLACEHOLDER — real V8 value needed
#define TFT_CS    -1   // PLACEHOLDER — real V8 value needed
#define TFT_DC    -1   // PLACEHOLDER — real V8 value needed
#define TFT_RST   -1   // PLACEHOLDER — real V8 value needed (TFT_eSPI pulses this)
#define TFT_BL    -1   // PLACEHOLDER — real V8 backlight GPIO (LEDC PWM) needed

// Resistive touch (XPT2046) shares the SPI bus; needs its own chip-select.
#define TOUCH_CS  -1   // PLACEHOLDER — real V8 T_CS value needed

// Section 4. Fonts -----------------------------------------------------------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// Section 5. SPI speed -------------------------------------------------------
#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
