#include "TDongleDisplay.h"

#ifdef HAS_T_DONGLE_DISPLAY

#include "WiFiScan.h"
#include "TDongleStats.h"
#include <SPI.h>
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "CatLogo.h"

// The TFT, microSD and APA102 LED share one SPI bus (SCLK 6 / MOSI 2 / MISO 7).
// TFT_eSPI drives the panel through the GLOBAL `SPI` object; the SD card is
// bound to a *copy* of it (sharedSPI) in the main sketch. Matching the official
// Marauder firmware, the display reclaims the bus for a redraw by re-initing the
// GLOBAL `SPI` object only -- never sharedSPI. Tearing down sharedSPI (the
// handle SD is actively bound to) is what made every wardrive/PCAP write fail
// with "Failed to open file"; re-initing the global SPI the TFT uses leaves the
// SD on a live handle.

namespace {
constexpr uint32_t kRefreshMs = 500;
constexpr uint8_t kRowHeight = 13;
constexpr uint8_t kBacklightPin = 0;

// MarauderBox cat-in-a-box emblem, drawn straight from the shared 1-bit CatLogo
// bitmap scaled to fit (nearest-neighbour -- plenty for this small start screen).
inline bool tdCatBit(int16_t x, int16_t y) {
  const int16_t rowbytes = (CAT_LOGO_WIDTH + 7) / 8;
  const uint8_t packed = pgm_read_byte(CAT_LOGO_BITMAP + y * rowbytes + (x >> 3));
  return (packed >> (7 - (x & 7))) & 0x01;
}
void tdDrawCat(TFT_eSPI& tft, int16_t dx, int16_t dy, int16_t dw, int16_t dh,
               uint16_t color) {
  if (dw <= 0 || dh <= 0) return;
  for (int16_t y = 0; y < dh; ++y) {
    const int16_t sy = static_cast<int16_t>(
        (static_cast<int32_t>(y) * CAT_LOGO_HEIGHT) / dh);
    int16_t run = -1;
    for (int16_t x = 0; x < dw; ++x) {
      const int16_t sx = static_cast<int16_t>(
          (static_cast<int32_t>(x) * CAT_LOGO_WIDTH) / dw);
      if (tdCatBit(sx, sy)) {
        if (run < 0) run = x;
      } else if (run >= 0) {
        tft.drawFastHLine(dx + run, dy + y, x - run, color);
        run = -1;
      }
    }
    if (run >= 0) tft.drawFastHLine(dx + run, dy + y, dw - run, color);
  }
}
}

void TDongleDisplay::begin() {
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, HIGH);
  delay(500);
  // tft.init() brings up the global SPI bus (spi.begin(SCLK, MISO, MOSI)) on its
  // own, so there is no manual bus juggling here -- just like the official code.
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  // MarauderBox cat-in-a-box start screen: emblem framed on the left, name on
  // the right. Guard the panel height (the dongle's TFT can mis-report it) and
  // keep clear top/bottom margins so the emblem is never clipped.
  int16_t PH = tft.height();
  if (PH < 60 || PH > 100) PH = 80;          // 160x80 landscape panel
  const int16_t box_h = PH - 16;             // 8px margin top + bottom
  const int16_t box_y = (PH - box_h) / 2;
  const int16_t box_x = 6;
  const int16_t cat_h = box_h - 12;
  const int16_t cat_w = static_cast<int16_t>(
      (static_cast<int32_t>(cat_h) * CAT_LOGO_WIDTH) / CAT_LOGO_HEIGHT);
  const int16_t box_w = cat_w + 12;
  tdDrawCat(tft, box_x + (box_w - cat_w) / 2, box_y + (box_h - cat_h) / 2,
            cat_w, cat_h, TFT_WHITE);
  tft.drawRoundRect(box_x, box_y, box_w, box_h, 5, TFT_WHITE);
  tft.drawRoundRect(box_x + 2, box_y + 2, box_w - 4, box_h - 4, 4, TFT_WHITE);

  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  const int16_t tx = box_x + box_w + 10;
  tft.drawString("MarauderBox", tx, box_y + 10, 1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("1.2.4", tx, box_y + 26, 1);

  digitalWrite(kBacklightPin, LOW);
}

void TDongleDisplay::drawValue(uint8_t row, const char* label, int value, uint16_t color) {
  const int y = 14 + (row * kRowHeight);
  tft.fillRect(0, y, tft.width(), kRowHeight, TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(label, 2, y + 2);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawRightString(String(value), tft.width() - 2, y + 2, 1);
}

bool TDongleDisplay::update(uint32_t now, const WiFiScan& scan) {
  if (now - last_update < kRefreshMs) return false;
  last_update = now;
  bool drew = false;

  const int ap_count = static_cast<int>(scan.retainedAccessPointCount());
  const int station_count = static_cast<int>(scan.retainedStationCount());
  const int ble_count = static_cast<int>(scan.retainedBleDeviceCount());
  const bool ap_changed = ap_count != last_ap_count;
  const bool station_changed = station_count != last_station_count;
  const bool ble_changed = ble_count != last_ble_count;
  const bool channel_changed = scan.set_channel != last_channel;
  const bool mode_changed = scan.currentScanMode != last_mode;

  if (!(ap_changed || station_changed || ble_changed || channel_changed || mode_changed)) {
    return false;
  }

  // LED updates bit-bang the TFT's MOSI/MISO pins. Reclaim the shared bus (the
  // GLOBAL SPI object TFT_eSPI uses, NOT the SD's sharedSPI) only when a redraw
  // is required, then let the caller write the LED state last.
  SPI.end();
  SPI.begin(T_DONGLE_SPI_SCLK_PIN, T_DONGLE_SPI_MISO_PIN,
            T_DONGLE_SPI_MOSI_PIN, -1);

  if (ap_changed) {
    drawValue(0, "WiFi AP", ap_count, TFT_GREEN);
    drew = true;
  }
  if (station_changed) {
    drawValue(1, "Stations", station_count, TFT_CYAN);
    drew = true;
  }
  if (ble_changed) {
    drawValue(2, "BLE", ble_count, TFT_MAGENTA);
    drew = true;
  }
  if (channel_changed) {
    drawValue(3, "Channel", scan.set_channel, TFT_YELLOW);
    drew = true;
  }

  if (mode_changed) {
    const int y = 14 + (4 * kRowHeight);
    tft.fillRect(0, y, tft.width(), kRowHeight, TFT_BLACK);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("Mode", 2, y + 2);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawRightString(TDongleStats::modeLabel(scan.currentScanMode), tft.width() - 2, y + 2, 1);
    drew = true;
  }

  last_ap_count = ap_count;
  last_station_count = station_count;
  last_ble_count = ble_count;
  last_channel = scan.set_channel;
  last_mode = scan.currentScanMode;
  return drew;
}

void TDongleDisplay::selfTest() {
  Serial.println(F("[TFT] self-test start"));
  // Reclaim the global SPI bus for the TFT (same object TFT_eSPI uses).
  SPI.end();
  SPI.begin(T_DONGLE_SPI_SCLK_PIN, T_DONGLE_SPI_MISO_PIN,
            T_DONGLE_SPI_MOSI_PIN, -1);
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, LOW);  // on, if backlight is active-low
  tft.init();
  tft.setRotation(3);
  Serial.printf("[TFT] panel reports %dx%d, backlight pin %d = LOW\n",
                (int)tft.width(), (int)tft.height(), (int)kBacklightPin);

  const uint16_t cols[4] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE };
  const char* names[4] = { "RED", "GREEN", "BLUE", "WHITE" };
  for (int i = 0; i < 4; i++) {
    Serial.print(F("[TFT] fill ")); Serial.print(names[i]);
    Serial.println(F("  (what color is on the screen?)"));
    tft.fillScreen(cols[i]);
    delay(1600);
  }

  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, tft.width(), tft.height(), TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("TFT TEST", 4, 4, 1);
  tft.drawString("abc 12345", 4, 20, 1);
  Serial.println(F("[TFT] white border + text -- centered? shifted? cut off?"));
  delay(2500);

  Serial.println(F("[TFT] invert ON"));
  tft.invertDisplay(true);
  delay(2000);
  Serial.println(F("[TFT] invert OFF"));
  tft.invertDisplay(false);
  delay(2000);

  Serial.println(F("[TFT] pin toggled HIGH -- if that's the backlight, screen goes dark now"));
  digitalWrite(kBacklightPin, HIGH);
  delay(2000);
  digitalWrite(kBacklightPin, LOW);
  Serial.println(F("[TFT] self-test done"));
}

void tDongleSetUsbAutoReset(bool enabled) {
  // USB_SERIAL_JTAG_CHIP_RST_REG bit USB_UART_CHIP_RST_DIS (bit 2): set it and
  // the chip ignores the host's DTR/RTS reset (so connecting the web remote no
  // longer reboots the dongle and blanks the panel); clear it and the host can
  // reset the chip, which the flasher needs to enter download mode.
  volatile uint32_t* reg = reinterpret_cast<volatile uint32_t*>(
      USB_SERIAL_JTAG_CHIP_RST_REG);
  volatile uint32_t* upd = reinterpret_cast<volatile uint32_t*>(
      USB_SERIAL_JTAG_CONFIG_UPDATE_REG);
  if (enabled)
    *reg = 0;  // allow the USB-triggered reset (needed to flash)
  else
    *reg = USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS;  // ignore it (protect screen)
  // The USB-Serial-JTAG latches config-register changes only on this strobe;
  // without it the bit above silently doesn't take effect.
  *upd = USB_SERIAL_JTAG_CONFIG_UPDATE;
  Serial.printf("[USB] chip_rst=0x%08lx rst_dis=%s\n", (unsigned long)*reg,
                (*reg & USB_SERIAL_JTAG_USB_UART_CHIP_RST_DIS) ? "SET" : "clear");
}

#endif
