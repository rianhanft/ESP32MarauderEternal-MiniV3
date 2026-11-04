#include "TDongleDisplay.h"

#ifdef HAS_T_DONGLE_DISPLAY

#include "WiFiScan.h"
#include "TDongleStats.h"
#include <SPI.h>
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

// The microSD card and the TFT share one SPI bus. The SD is bound to this
// instance (SD.begin(SD_CS, sharedSPI)); reclaim the bus through it -- not the
// global SPI object -- so freeing/re-initing the bus for a redraw doesn't leave
// the SD on a dead handle (which made every wardrive/SD write fail).
extern SPIClass sharedSPI;

namespace {
constexpr uint32_t kRefreshMs = 500;
constexpr uint8_t kRowHeight = 13;
constexpr uint8_t kBacklightPin = 0;
}

void TDongleDisplay::begin() {
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, HIGH);
  delay(500);
  // The SD card's sharedSPI.begin() already claimed this bus; point it back at
  // the TFT pins before init or the ST7735 never receives its setup commands.
  sharedSPI.end();
  sharedSPI.begin(T_DONGLE_SPI_SCLK_PIN, T_DONGLE_SPI_MISO_PIN,
            T_DONGLE_SPI_MOSI_PIN, -1);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("MARAUDER CLI", 2, 1);
  tft.drawFastHLine(0, 11, tft.width(), TFT_DARKGREY);
  digitalWrite(kBacklightPin, LOW);
  init_time = millis();
  reinit_count = 0;
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

  // Connecting the web remote triggers a partial reset (rst 0x15) that the
  // one-shot begin() can't always bring the panel back from -- the screen stays
  // grey while attached. Re-initialize the panel a few times over the first few
  // seconds of each boot (a connect causes a reboot, so this runs after it too)
  // so the display self-heals instead of staying blank.
  if (reinit_count < 1 &&
      static_cast<int32_t>(now - init_time) >=
          static_cast<int32_t>((reinit_count + 1) * 1200)) {
    reinit_count++;
    sharedSPI.end();
    sharedSPI.begin(T_DONGLE_SPI_SCLK_PIN, T_DONGLE_SPI_MISO_PIN,
              T_DONGLE_SPI_MOSI_PIN, -1);
    pinMode(kBacklightPin, OUTPUT);
    digitalWrite(kBacklightPin, LOW);
    tft.init();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("MARAUDER CLI", 2, 1);
    tft.drawFastHLine(0, 11, tft.width(), TFT_DARKGREY);
    last_ap_count = -1; last_station_count = -1; last_ble_count = -1;
    last_channel = -1; last_mode = -1;  // force every value to redraw below
  }

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

  // LED updates bit-bang the TFT's MOSI/MISO pins. Reclaim the shared bus only
  // when a redraw is required, then let the caller write the LED state last.
  sharedSPI.end();
  sharedSPI.begin(T_DONGLE_SPI_SCLK_PIN, T_DONGLE_SPI_MISO_PIN,
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
  sharedSPI.end();
  sharedSPI.begin(T_DONGLE_SPI_SCLK_PIN, T_DONGLE_SPI_MISO_PIN,
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
