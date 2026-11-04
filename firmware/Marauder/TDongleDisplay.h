#pragma once

#include "configs.h"

#ifdef HAS_T_DONGLE_DISPLAY

#include <TFT_eSPI.h>

class WiFiScan;

class TDongleDisplay {
 public:
  void begin();
  bool update(uint32_t now, const WiFiScan& scan);
  void selfTest();  // CLI "tfttest": display bring-up diagnostic over serial

 private:
  TFT_eSPI tft;
  uint32_t last_update = 0;
  int last_ap_count = -1;
  int last_station_count = -1;
  int last_ble_count = -1;
  int last_channel = -1;
  int last_mode = -1;
  uint32_t init_time = 0;    // millis() at begin(), for the self-heal re-inits
  uint8_t reinit_count = 0;  // panel re-inits done in the first seconds of boot

  void drawValue(uint8_t row, const char* label, int value, uint16_t color);
};

#endif
