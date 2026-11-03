/*
 * ESP32-C5 Wi-Fi/BLE activity scanner and baseline anomaly detector.
 *
 * The UI concept is inspired by nyanBOX's Scanner and Jam Detector. nyanBOX
 * measures raw 2.4 GHz energy with external nRF24 radios; this adaptation can
 * only count frames and advertisements decoded by the ESP32-C5. It therefore
 * labels possible saturation/interference as an anomaly, never proof of a
 * jammer.
 * SPDX-License-Identifier: MIT
 */

#include "WirelessActivityTools.h"

#include "configs.h"

// Enabled on the Mini V3 (physical D-pad) or any touch board (Marauder V8),
// where u_btn..c_btn come from EternalTouchButtons.cpp as touch zones.
#if defined(HAS_BT) && defined(HAS_NIMBLE_2) && defined(HAS_SCREEN) && ( \
      (defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
       (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)) \
      || defined(HAS_TOUCH) )

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace WirelessActivityTools {
namespace {

constexpr uint8_t CHANNEL_COUNT = 11;
constexpr uint16_t CHANNEL_DWELL_MS = 500;
constexpr uint8_t BASELINE_SWEEPS = 4;

// Anomaly thresholds. A live dwell must clear BOTH a multiple of the channel
// baseline AND an absolute margin before it counts as a frame hit, so ordinary
// beacon/probe traffic on a channel that was quiet during calibration can no
// longer trip the alarm on its own. A hit then has to repeat on STRIKE_ON
// separate visits to that channel before the alarm latches; an overwhelming
// surge latches on the first visit.
constexpr uint16_t ACTIVITY_MULTIPLIER = 3;
constexpr uint16_t ACTIVITY_FLOOR = 60;
constexpr uint16_t SURGE_MULTIPLIER = 6;
constexpr uint16_t SURGE_FLOOR = 300;
constexpr int16_t NOISE_DELTA_DB = 10;
constexpr uint16_t NOISE_MIN_FRAMES = 10;
constexpr uint32_t BLE_MULTIPLIER = 3;
constexpr uint32_t BLE_FLOOR = 25;
constexpr uint8_t STRIKE_ON = 2;

struct ChannelSample {
  uint16_t frames;
  int8_t averageRssi;
  int8_t noiseFloor;
};

volatile uint32_t dwellFrames = 0;
volatile int32_t dwellRssiSum = 0;
volatile int32_t dwellNoiseSum = 0;
volatile uint32_t dwellNoiseSamples = 0;
volatile uint32_t bleAdvertisements = 0;
volatile uint8_t activeChannel = 1;
portMUX_TYPE activityMux = portMUX_INITIALIZER_UNLOCKED;

bool buttonDown(Switches& button) {
  return button.isDown();
}

void releaseButton(Switches& button) {
  while (buttonDown(button)) {
    button.justPressed();
    delay(5);
  }
  button.justPressed();
}

void releaseControls() {
  releaseButton(u_btn);
  releaseButton(d_btn);
  releaseButton(l_btn);
  releaseButton(r_btn);
  releaseButton(c_btn);
}

void wifiCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  if (!buffer || type == WIFI_PKT_MISC)
    return;
  const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
  portENTER_CRITICAL_ISR(&activityMux);
  dwellFrames++;
  dwellRssiSum += packet->rx_ctrl.rssi;
  const int8_t noise = packet->rx_ctrl.noise_floor;
  if (noise <= -20 && noise >= -127) {
    dwellNoiseSum += noise;
    dwellNoiseSamples++;
  }
  portEXIT_CRITICAL_ISR(&activityMux);
}

class ActivityCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice*) override {
    portENTER_CRITICAL(&activityMux);
    bleAdvertisements++;
    portEXIT_CRITICAL(&activityMux);
  }
};

ActivityCallbacks activityCallbacks;

ChannelSample finishDwell() {
  ChannelSample sample{};
  portENTER_CRITICAL(&activityMux);
  const uint32_t frames = dwellFrames;
  sample.frames = min<uint32_t>(frames, UINT16_MAX);
  sample.averageRssi = frames ? dwellRssiSum / static_cast<int32_t>(frames) : -127;
  sample.noiseFloor = dwellNoiseSamples ?
      dwellNoiseSum / static_cast<int32_t>(dwellNoiseSamples) : -127;
  dwellFrames = 0;
  dwellRssiSum = 0;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  portEXIT_CRITICAL(&activityMux);
  return sample;
}

uint32_t copyBleCount() {
  portENTER_CRITICAL(&activityMux);
  const uint32_t count = bleAdvertisements;
  portEXIT_CRITICAL(&activityMux);
  return count;
}

void resetCounters() {
  portENTER_CRITICAL(&activityMux);
  dwellFrames = 0;
  dwellRssiSum = 0;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  bleAdvertisements = 0;
  portEXIT_CRITICAL(&activityMux);
}

void drawHeader(const char* title, uint16_t color) {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, 16, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(color, TFT_NAVY);
  display_obj.tft.drawString(title, TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawScanner(const ChannelSample samples[CHANNEL_COUNT], uint32_t bleRate) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("C5 WIFI/BLE SCANNER", TFT_CYAN);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("BLE:%lu/s  Ch:%u", static_cast<unsigned long>(bleRate),
                         activeChannel);

  uint16_t maximum = 1;
  for (uint8_t index = 0; index < CHANNEL_COUNT; index++)
    maximum = max(maximum, samples[index].frames);
  for (uint8_t index = 0; index < CHANNEL_COUNT; index++) {
    const int16_t x = 3 + index * 11;
    const int16_t height = min<int16_t>(67,
        static_cast<int32_t>(samples[index].frames) * 67 / maximum);
    const uint16_t color = (index + 1 == activeChannel) ? TFT_YELLOW : TFT_CYAN;
    display_obj.tft.drawRect(x, 32, 8, 69, TFT_DARKGREY);
    if (height > 0)
      display_obj.tft.fillRect(x + 1, 100 - height, 6, height, color);
    display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display_obj.tft.setCursor(x, 103);
    display_obj.tft.print(index + 1);
  }
  const ChannelSample& current = samples[(activeChannel + CHANNEL_COUNT - 2) % CHANNEL_COUNT];
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, 114);
  if (current.noiseFloor > -127)
    display_obj.tft.printf("N:%ddBm  Center:exit", current.noiseFloor);
  else
    display_obj.tft.print(F("Decoded frames  C:exit"));
}

void drawBaseline(uint8_t sweeps, uint32_t elapsed) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("JAM DETECTOR", TFT_ORANGE);
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.drawString("Measuring baseline", 4, 25, 1);
  const uint8_t progress = min<uint32_t>(100,
      (elapsed * 100) / (CHANNEL_COUNT * CHANNEL_DWELL_MS * BASELINE_SWEEPS));
  display_obj.tft.drawRect(4, 47, TFT_WIDTH - 8, 13, TFT_DARKGREY);
  display_obj.tft.fillRect(6, 49, (TFT_WIDTH - 12) * progress / 100, 9,
                           TFT_CYAN);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String(progress) + "%  sweep " +
      String(sweeps + 1) + "/" + String(BASELINE_SWEEPS), 4, 68, 1);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 84);
  display_obj.tft.print(F("Keep the local radio environment steady."));
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: exit"));
}

void drawDetector(const ChannelSample samples[CHANNEL_COUNT],
                  const uint16_t baselineFrames[CHANNEL_COUNT],
                  const int8_t baselineNoise[CHANNEL_COUNT],
                  uint32_t bleRate, uint32_t baselineBleRate,
                  bool activityAlert, bool noiseAlert, bool bleAlert) {
  uint8_t worstChannel = 1;
  uint16_t worstScore = 0;
  int16_t noiseDelta = 0;
  for (uint8_t index = 0; index < CHANNEL_COUNT; index++) {
    const uint16_t base = max<uint16_t>(1, baselineFrames[index]);
    const uint16_t ratio = min<uint32_t>(999,
        static_cast<uint32_t>(samples[index].frames) * 100 / base);
    int16_t delta = 0;
    if (samples[index].noiseFloor > -127 && baselineNoise[index] > -127)
      delta = samples[index].noiseFloor - baselineNoise[index];
    const uint16_t score = ratio + max<int16_t>(0, delta) * 20;
    if (score >= worstScore) {
      worstScore = score;
      worstChannel = index + 1;
      noiseDelta = delta;
    }
  }
  const uint8_t index = worstChannel - 1;
  const bool alert = activityAlert || noiseAlert || bleAlert;

  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("JAM DETECTOR", alert ? TFT_RED : TFT_GREEN);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextColor(alert ? TFT_RED : TFT_GREEN, TFT_BLACK);
  display_obj.tft.setTextSize(2);
  display_obj.tft.drawString(alert ? "ANOMALY" : "NORMAL", TFT_WIDTH / 2, 22, 1);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String("WiFi ch ") + worstChannel + ": " +
      samples[index].frames + " / " + baselineFrames[index], 4, 48, 1);
  display_obj.tft.drawString(String("Noise delta: ") + noiseDelta + " dB",
                             4, 62, 1);
  display_obj.tft.drawString(String("BLE ads/s: ") + bleRate + " / " +
      baselineBleRate, 4, 76, 1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
  const char* reason = noiseAlert ? "Decoded-frame noise rise" :
      (activityAlert ? "WiFi activity saturation" :
       (bleAlert ? "BLE activity saturation" : "No baseline deviation"));
  display_obj.tft.drawString(reason, 4, 91, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("R:recalibrate C:exit"));
}

void showError(const char* message) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader("WIRELESS ACTIVITY", TFT_RED);
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 28);
  display_obj.tft.print(message);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(4, TFT_HEIGHT - 12);
  display_obj.tft.print(F("Center: exit"));
}

void runTool(bool detector, bool fromSerial) {
  releaseControls();
  resetCounters();
  if (fromSerial)  // drop the CLI's trailing newline so it can't self-exit
    while (Serial.available()) Serial.read();

  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);

  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_err_t wifiError = esp_wifi_set_promiscuous_filter(&filter);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous_rx_cb(wifiCallback);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous(true);
  activeChannel = 1;

  NimBLEDevice::init("Marauder-Activity");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&activityCallbacks, true);
  scan->setActiveScan(false);
  scan->setInterval(50);
  scan->setWindow(35);
  scan->setMaxResults(0);
  const bool bleScanning = scan->start(0, false, true);
  const bool usable = wifiError == ESP_OK || bleScanning;
  if (!usable)
    showError("Wi-Fi and BLE receivers could not start.");

  ChannelSample samples[CHANNEL_COUNT]{};
  uint32_t baselineFrameSums[CHANNEL_COUNT]{};
  int32_t baselineNoiseSums[CHANNEL_COUNT]{};
  uint8_t baselineNoiseSamples[CHANNEL_COUNT]{};
  uint16_t baselineFrames[CHANNEL_COUNT]{};
  int8_t baselineNoise[CHANNEL_COUNT];
  memset(baselineNoise, -127, sizeof(baselineNoise));
  uint8_t baselineSweeps = 0;
  bool baselineReady = !detector;
  uint32_t baselineStarted = millis();
  uint32_t baselineBleStart = copyBleCount();
  uint32_t baselineBleRate = 0;
  uint32_t lastBleCount = 0;
  uint32_t bleRate = 0;
  uint32_t lastBleAt = millis();
  uint32_t nextHop = millis() + CHANNEL_DWELL_MS;
  uint32_t nextDraw = 0;
  uint8_t frameStrikes[CHANNEL_COUNT]{};
  uint8_t noiseStrikes[CHANNEL_COUNT]{};
  uint8_t bleStrikes = 0;
  uint16_t graceDwells = 0;

  uint32_t nextSerial = 0;
  while (true) {
    if (c_btn.justPressed())
      break;
    if (fromSerial && Serial.available()) {  // web "Stop"/stopscan ends the tool
      while (Serial.available()) Serial.read();
      break;
    }
    if (detector && baselineReady && r_btn.justPressed()) {
      memset(samples, 0, sizeof(samples));
      memset(baselineFrameSums, 0, sizeof(baselineFrameSums));
      memset(baselineNoiseSums, 0, sizeof(baselineNoiseSums));
      memset(baselineNoiseSamples, 0, sizeof(baselineNoiseSamples));
      memset(baselineFrames, 0, sizeof(baselineFrames));
      memset(baselineNoise, -127, sizeof(baselineNoise));
      baselineSweeps = 0;
      baselineReady = false;
      baselineBleRate = 0;
      baselineStarted = millis();
      baselineBleStart = copyBleCount();
      lastBleCount = baselineBleStart;
      memset(frameStrikes, 0, sizeof(frameStrikes));
      memset(noiseStrikes, 0, sizeof(noiseStrikes));
      bleStrikes = 0;
      graceDwells = 0;
    }

    const uint32_t now = millis();
    if (wifiError == ESP_OK && static_cast<int32_t>(now - nextHop) >= 0) {
      const uint8_t completed = activeChannel - 1;
      samples[completed] = finishDwell();
      if (detector && !baselineReady) {
        baselineFrameSums[completed] += samples[completed].frames;
        if (samples[completed].noiseFloor > -127) {
          baselineNoiseSums[completed] += samples[completed].noiseFloor;
          baselineNoiseSamples[completed]++;
        }
      }
      activeChannel = (activeChannel % CHANNEL_COUNT) + 1;
      if (activeChannel == 1 && detector && !baselineReady) {
        baselineSweeps++;
        if (baselineSweeps >= BASELINE_SWEEPS) {
          for (uint8_t channel = 0; channel < CHANNEL_COUNT; channel++) {
            baselineFrames[channel] = baselineFrameSums[channel] / BASELINE_SWEEPS;
            if (baselineNoiseSamples[channel])
              baselineNoise[channel] = baselineNoiseSums[channel] /
                                       baselineNoiseSamples[channel];
          }
          const uint32_t elapsedSeconds = max<uint32_t>(1,
              (now - baselineStarted) / 1000);
          baselineBleRate = (copyBleCount() - baselineBleStart) / elapsedSeconds;
          baselineReady = true;
          graceDwells = CHANNEL_COUNT;
          memset(frameStrikes, 0, sizeof(frameStrikes));
          memset(noiseStrikes, 0, sizeof(noiseStrikes));
          bleStrikes = 0;
        }
      }
      else if (detector && baselineReady) {
        const ChannelSample& live = samples[completed];
        if (graceDwells > 0) {
          // First live sweep after calibration: pull up any channel whose short
          // baseline undercounted its real beacon load, and hold the alarm off
          // so a fresh, realistic baseline is in place before detection arms.
          if (live.frames > baselineFrames[completed])
            baselineFrames[completed] = live.frames;
          if (live.noiseFloor > -127 && live.noiseFloor > baselineNoise[completed])
            baselineNoise[completed] = live.noiseFloor;
          frameStrikes[completed] = 0;
          noiseStrikes[completed] = 0;
          graceDwells--;
        } else {
          const uint32_t base = baselineFrames[completed];
          const bool frameHit = live.frames >
              base * ACTIVITY_MULTIPLIER + ACTIVITY_FLOOR;
          const bool frameSurge = live.frames >
              base * SURGE_MULTIPLIER + SURGE_FLOOR;
          if (frameSurge)
            frameStrikes[completed] = STRIKE_ON;
          else if (frameHit)
            frameStrikes[completed] =
                min<uint8_t>(frameStrikes[completed] + 1, STRIKE_ON + 1);
          else
            frameStrikes[completed] = 0;

          const bool noiseHit = live.noiseFloor > -127 &&
              baselineNoise[completed] > -127 &&
              live.frames >= NOISE_MIN_FRAMES &&
              live.noiseFloor - baselineNoise[completed] >= NOISE_DELTA_DB;
          noiseStrikes[completed] = noiseHit ?
              min<uint8_t>(noiseStrikes[completed] + 1, STRIKE_ON + 1) : 0;

          // Let a calm channel drift so the baseline tracks the room over time,
          // but never fold a hit into it (that would train the detector to treat
          // an ongoing jam as the new normal).
          if (!frameHit && !noiseHit) {
            baselineFrames[completed] =
                (baselineFrames[completed] * 15 + live.frames) / 16;
            if (live.noiseFloor > -127)
              baselineNoise[completed] =
                  (baselineNoise[completed] * 15 + live.noiseFloor) / 16;
          }
        }
      }
      esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
      nextHop = now + CHANNEL_DWELL_MS;
    }

    if (now - lastBleAt >= 1000) {
      const uint32_t current = copyBleCount();
      bleRate = (current - lastBleCount) * 1000 / max<uint32_t>(1, now - lastBleAt);
      lastBleCount = current;
      lastBleAt = now;
      if (detector && baselineReady && graceDwells == 0) {
        const bool bleHit = bleRate > baselineBleRate * BLE_MULTIPLIER + BLE_FLOOR;
        bleStrikes = bleHit ? min<uint8_t>(bleStrikes + 1, STRIKE_ON + 1) : 0;
      } else {
        bleStrikes = 0;
      }
    }
    if (usable && static_cast<int32_t>(now - nextDraw) >= 0) {
      nextDraw = now + 500;
      if (detector && !baselineReady)
        drawBaseline(baselineSweeps, now - baselineStarted);
      else if (detector) {
        bool activityAlert = false;
        bool noiseAlert = false;
        for (uint8_t ch = 0; ch < CHANNEL_COUNT; ch++) {
          if (frameStrikes[ch] >= STRIKE_ON) activityAlert = true;
          if (noiseStrikes[ch] >= STRIKE_ON) noiseAlert = true;
        }
        const bool bleAlert = bleStrikes >= STRIKE_ON;
        drawDetector(samples, baselineFrames, baselineNoise, bleRate,
                     baselineBleRate, activityAlert, noiseAlert, bleAlert);
      }
      else
        drawScanner(samples, bleRate);
    }
    // Serial telemetry for the web remote: one status line per second so the
    // browser console shows what the on-screen tool is doing.
    if (fromSerial && static_cast<int32_t>(now - nextSerial) >= 0) {
      nextSerial = now + 1000;
      if (detector && !baselineReady) {
        Serial.printf("[JAM] calibrating... sweep %u/%u\n",
                      (unsigned)(baselineSweeps + 1), (unsigned)BASELINE_SWEEPS);
      } else if (detector) {
        bool act = false, noi = false;
        int ch = -1;
        for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
          if (frameStrikes[i] >= STRIKE_ON) { act = true; if (ch < 0) ch = i + 1; }
          if (noiseStrikes[i] >= STRIKE_ON) { noi = true; if (ch < 0) ch = i + 1; }
        }
        const bool ble = bleStrikes >= STRIKE_ON;
        if (!(act || noi || ble))
          Serial.printf("[JAM] NORMAL  ble %lu/%lu/s\n",
                        (unsigned long)bleRate, (unsigned long)baselineBleRate);
        else
          Serial.printf("[JAM] ANOMALY ch%d  act=%d noise=%d ble=%d  ble %lu/%lu/s\n",
                        ch < 0 ? 0 : ch, act, noi, ble,
                        (unsigned long)bleRate, (unsigned long)baselineBleRate);
      } else {
        uint8_t busy = 0; uint16_t best = 0;
        for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
          if (samples[i].frames >= best) { best = samples[i].frames; busy = i + 1; }
        Serial.printf("[SCAN] ch%u  ble %lu/s  busiest ch%u %uf/500ms\n",
                      (unsigned)activeChannel, (unsigned long)bleRate,
                      (unsigned)busy, (unsigned)best);
      }
    }
    delay(5);
  }

  if (scan->isScanning())
    scan->stop();
  scan->setScanCallbacks(nullptr);
  scan->clearResults();
  NimBLEDevice::deinit(true);
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace

void runScanner(bool fromSerial) { runTool(false, fromSerial); }
void runJamDetector(bool fromSerial) { runTool(true, fromSerial); }

}  // namespace WirelessActivityTools

#else

namespace WirelessActivityTools {
void runScanner(bool) {}
void runJamDetector(bool) {}
}

#endif
