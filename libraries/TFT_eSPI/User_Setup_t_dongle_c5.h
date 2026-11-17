// TFT_eSPI configuration for the LilyGo T-Dongle C5 (ESP32-C5HR8).
// Select this file from TFT_eSPI's User_Setup_Select.h, or copy it over
// TFT_eSPI/User_Setup.h before building.
//
// Panel: 0.96" IPS, ST7735 controller, 80x160, 4-wire SPI. The display, the
// microSD slot, and the back APA102 RGB LED all share one SPI bus; the LED is
// bit-banged on the TFT's MOSI/SCLK lines (see TDongleDisplay.cpp / configs.h
// T_DONGLE_* pins), each peripheral gated by its own chip-select.

#define USER_SETUP_INFO "LilyGo T-Dongle C5 ESP32-C5"

#define ST7735_DRIVER
#define ST7735_GREENTAB160x80
#define TFT_RGB_ORDER TFT_BGR
#define TFT_WIDTH 80
#define TFT_HEIGHT 160
// 0.96" IPS ST7735 panels on this dongle render inverted without this. If
// colors come out as negatives on hardware, comment this line out.
#define TFT_INVERSION_ON
#define TFT_BACKLIGHT_ON LOW

// Shared SPI bus (matches configs.h T_DONGLE_* and TDongleDisplay SPI.begin).
#define TFT_MISO 7
#define TFT_MOSI 2
#define TFT_SCLK 6
#define TFT_CS   10
#define TFT_DC   3
#define TFT_RST  1
#define TFT_BL   0
#define TOUCH_CS -1

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY 20000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000
