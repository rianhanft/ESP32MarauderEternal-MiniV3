/*
 * Bounded captured Open Drone ID broadcaster for authorized lab testing.
 *
 * Transport/message handling is adapted from nyanBOX (MIT) and
 * opendroneid-core-c (Apache-2.0). This version only replays a drone explicitly
 * selected from the most recent passive Remote ID scan (60 s auto-stop) or
 * built from an SD file (runs until the user stops it), and uses a moderate
 * update interval.
 * SPDX-License-Identifier: MIT AND Apache-2.0
 */

#include "DroneRemoteIDSpoofer.h"

#include "configs.h"

#if defined(HAS_BT) && defined(HAS_NIMBLE_2) && defined(HAS_SCREEN) && ( \
      (defined(HAS_BUTTONS) && (C_BTN >= 0)) || defined(HAS_TOUCH) )

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches c_btn;

#ifdef HAS_GPS
  #include "GpsInterface.h"
  extern GpsInterface gps_obj;
#endif

namespace DroneRemoteIDSpoofer {
namespace {

constexpr uint8_t DEFAULT_WIFI_CHANNEL = 6;
constexpr uint16_t MESSAGE_INTERVAL_MS = 250;
constexpr uint32_t MAX_RUNTIME_MS = 60000;

// ODID message-type indices into CapturedDrone::messages[] (matching the
// decoder's message types). The Location message carries the drone's position;
// the System message carries the operator position.
constexpr uint8_t ODID_MSG_LOCATION = 1;
constexpr uint8_t ODID_MSG_SYSTEM = 4;

enum GpsSpoofState : uint8_t {
  GPS_UNSUPPORTED,   // firmware built without GPS
  GPS_NO_FIX,        // GPS present but no fix -> replay the captured coordinates
  GPS_NO_LOC_MSG,    // have a fix, but the captured drone had no Location message
  GPS_TRACKING       // overwriting the broadcast position with our live fix
};

DroneRemoteID::CapturedDrone selectedTarget{};
bool targetSelected = false;

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

void putLe16(uint8_t* output, uint16_t value) {
  output[0] = value & 0xFF;
  output[1] = value >> 8;
}

void putI32Le(uint8_t* output, int32_t value) {
  const uint32_t raw = static_cast<uint32_t>(value);
  output[0] = raw & 0xFF;
  output[1] = (raw >> 8) & 0xFF;
  output[2] = (raw >> 16) & 0xFF;
  output[3] = (raw >> 24) & 0xFF;
}

// Rewrite the replayed drone's broadcast position to this Marauder's own live
// GPS fix, so a second Marauder running Remote ID plots the spoofed drone at
// THIS unit's real location -- the drone's on-map distance then reflects how
// far apart the two units are and updates as this unit moves. Overwrites the
// Location message lat/lon (bytes 5-8 / 9-12) and the System message operator
// lat/lon (bytes 2-5 / 6-9) in place. With no fix it leaves the captured
// coordinates untouched. Authorized testing between devices you own.
GpsSpoofState applyLiveGps() {
#ifdef HAS_GPS
  if (!gps_obj.getFixStatus())
    return GPS_NO_FIX;
  // GPS ints are microdegrees (1e-6 deg); ODID encodes 1e-7 deg, so scale x10.
  const int32_t latE7 =
      static_cast<int32_t>(static_cast<int64_t>(gps_obj.getLatInt()) * 10);
  const int32_t lonE7 =
      static_cast<int32_t>(static_cast<int64_t>(gps_obj.getLonInt()) * 10);
  if (selectedTarget.messageMask & (1 << ODID_MSG_SYSTEM)) {
    putI32Le(selectedTarget.messages[ODID_MSG_SYSTEM] + 2, latE7);
    putI32Le(selectedTarget.messages[ODID_MSG_SYSTEM] + 6, lonE7);
  }
  if (!(selectedTarget.messageMask & (1 << ODID_MSG_LOCATION)))
    return GPS_NO_LOC_MSG;
  putI32Le(selectedTarget.messages[ODID_MSG_LOCATION] + 5, latE7);
  putI32Le(selectedTarget.messages[ODID_MSG_LOCATION] + 9, lonE7);
  return GPS_TRACKING;
#else
  return GPS_UNSUPPORTED;
#endif
}

bool parseAddress(const char* address, uint8_t output[6]) {
  if (!address)
    return false;
  unsigned values[6]{};
  if (sscanf(address, "%02x:%02x:%02x:%02x:%02x:%02x",
             &values[0], &values[1], &values[2], &values[3], &values[4],
             &values[5]) != 6)
    return false;
  for (uint8_t index = 0; index < 6; index++)
    output[index] = values[index];
  return true;
}

uint8_t capturedMessageTypes(uint8_t output[DroneRemoteID::CAPTURED_MESSAGE_TYPES]) {
  uint8_t count = 0;
  for (uint8_t type = 0; type < DroneRemoteID::CAPTURED_MESSAGE_TYPES; type++) {
    if (selectedTarget.messageMask & (1 << type))
      output[count++] = type;
  }
  return count;
}

bool updateBleAdvertisement(
    NimBLEAdvertising* advertising,
    const uint8_t message[DroneRemoteID::CAPTURED_MESSAGE_SIZE],
    uint8_t counter) {
  uint8_t raw[31]{};
  raw[0] = 30;
  raw[1] = 0x16;                      // Service Data, 16-bit UUID.
  raw[2] = 0xFA;
  raw[3] = 0xFF;                      // Open Drone ID UUID 0xFFFA.
  raw[4] = 0x0D;
  raw[5] = counter;
  memcpy(raw + 6, message, DroneRemoteID::CAPTURED_MESSAGE_SIZE);

  if (advertising->isAdvertising())
    advertising->stop();
  advertising->reset();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);
  advertising->enableScanResponse(false);
  advertising->setAdvertisingInterval(320);      // 200 ms.
  NimBLEAdvertisementData data;
  if (!data.addData(raw, sizeof(raw)) ||
      !advertising->setAdvertisementData(data))
    return false;
  return advertising->start();
}

bool sendWifiBeacon(
    const uint8_t source[6], uint8_t channel,
    const uint8_t message[DroneRemoteID::CAPTURED_MESSAGE_SIZE],
    uint8_t counter, uint16_t sequence) {
  uint8_t frame[83]{};
  frame[0] = 0x80;                    // Beacon.
  memset(frame + 4, 0xFF, 6);
  memcpy(frame + 10, source, 6);
  memcpy(frame + 16, source, 6);
  putLe16(frame + 22, (sequence & 0x0FFF) << 4);
  const uint64_t timestamp = esp_timer_get_time();
  memcpy(frame + 24, &timestamp, sizeof(timestamp));
  putLe16(frame + 32, 100);
  frame[34] = 0x01;
  frame[35] = 0x04;
  frame[36] = 0x00;                   // Empty SSID.
  frame[37] = 0x00;
  const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8B, 0x96,
                           0x0C, 0x18, 0x30, 0x48};
  memcpy(frame + 38, rates, sizeof(rates));
  frame[48] = 0x03;
  frame[49] = 0x01;
  frame[50] = channel;
  frame[51] = 0xDD;                   // Vendor-specific Remote ID IE.
  frame[52] = 30;
  frame[53] = 0xFA;
  frame[54] = 0x0B;
  frame[55] = 0xBC;
  frame[56] = 0x0D;
  frame[57] = counter;
  memcpy(frame + 58, message, DroneRemoteID::CAPTURED_MESSAGE_SIZE);
  return esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false) == ESP_OK;
}

void drawStatus(uint32_t started, uint32_t blePackets, uint32_t wifiPackets,
                uint8_t wifiChannel, uint8_t messageCount, bool bleOkay,
                bool wifiOkay, bool exactBleAddress, bool exactWifiAddress,
                uint8_t gpsState, bool autoStop) {
  const uint32_t elapsed = millis() - started;
  const uint32_t remaining = elapsed >= MAX_RUNTIME_MS ? 0 :
      (MAX_RUNTIME_MS - elapsed + 999) / 1000;
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, 16, TFT_RED);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_RED);
  display_obj.tft.drawString("AUTHORIZED DRONE SPOOF", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.setViewport(2, 19, TFT_WIDTH - 4, 14);
  display_obj.tft.setCursor(0, 2);
  display_obj.tft.print(selectedTarget.id);
  display_obj.tft.resetViewport();
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String("Captured types: ") + messageCount,
                             4, 36, 1);
  display_obj.tft.drawString(String("BLE: ") + (bleOkay ? "on " : "failed ") +
      blePackets + (exactBleAddress ? " exact" : " local"), 4, 50, 1);
  display_obj.tft.drawString(String("WiFi: ") + (wifiOkay ? "ch" : "failed ") +
      (wifiOkay ? String(wifiChannel) : String("")) + " " + wifiPackets +
      (exactWifiAddress ? " exact" : " local"), 4, 64, 1);
  // Live-GPS spoof-position status: green when the broadcast position is being
  // driven by our own fix, else a note that captured coordinates are replayed.
  #ifdef HAS_GPS
    uint16_t gpsColor = TFT_DARKGREY;
    String gpsText = "GPS: no fix (captured loc)";
    if (gpsState == GPS_TRACKING) {
      gpsColor = TFT_GREEN;
      gpsText = String("GPS: live ") + gps_obj.getLat() + "," + gps_obj.getLon();
    } else if (gpsState == GPS_NO_LOC_MSG) {
      gpsColor = TFT_ORANGE;
      gpsText = "GPS fix: target has no loc msg";
    }
    display_obj.tft.setTextColor(gpsColor, TFT_BLACK);
    display_obj.tft.setViewport(4, 78, TFT_WIDTH - 8, 13);
    display_obj.tft.setCursor(0, 2);
    display_obj.tft.print(gpsText);
    display_obj.tft.resetViewport();
    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  #else
    (void)gpsState;
  #endif
  display_obj.tft.setViewport(4, 92, TFT_WIDTH - 8, 13);
  display_obj.tft.setCursor(0, 2);
  display_obj.tft.print(selectedTarget.address);
  display_obj.tft.resetViewport();
  display_obj.tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  if (autoStop)
    display_obj.tft.drawString(String("Auto stop: ") + remaining + "s", 4, 106, 1);
  else
    display_obj.tft.drawString("Running (no auto-stop)", 4, 106, 1);
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Center: stop now"));
}

void drawError(const char* message) {
  display_obj.tft.fillScreen(TFT_BLACK);
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 20);
  display_obj.tft.print(message);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(4, TFT_HEIGHT - 12);
  display_obj.tft.print(F("Center: return"));
}

}  // namespace

bool selectTarget(const DroneRemoteID::CapturedDrone& target) {
  if (target.messageMask == 0)
    return false;
  selectedTarget = target;
  targetSelected = true;
  return true;
}

void run(bool autoStop) {
  releaseButton(c_btn);
  if (!targetSelected || selectedTarget.messageMask == 0) {
    drawError("No captured target selected. Run Drone Remote ID first.");
    while (!c_btn.justPressed())
      delay(5);
    releaseButton(c_btn);
    return;
  }
  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);

  uint8_t messageTypes[DroneRemoteID::CAPTURED_MESSAGE_TYPES]{};
  const uint8_t messageCount = capturedMessageTypes(messageTypes);
  if (messageCount == 0) {
    drawError("The selected entry has no replayable Remote ID messages.");
    while (!c_btn.justPressed())
      delay(5);
    releaseButton(c_btn);
    return;
  }

  uint8_t capturedAddress[6]{};
  const bool parsedAddress = parseAddress(selectedTarget.address, capturedAddress);
  const bool exactWifiAddress = parsedAddress &&
      (selectedTarget.methods & DroneRemoteID::CAPTURE_METHOD_WIFI);
  uint8_t wifiSource[6]{};
  uint8_t bleAddress[6]{};
  if (exactWifiAddress) {
    memcpy(wifiSource, capturedAddress, sizeof(wifiSource));
    wifiSource[0] &= 0xFE;             // 802.11 source must be unicast.
  }
  else {
    for (uint8_t index = 0; index < 6; index++)
      wifiSource[index] = esp_random();
    wifiSource[0] = (wifiSource[0] & 0xFC) | 0x02;
  }

  if (parsedAddress)
    memcpy(bleAddress, capturedAddress, sizeof(bleAddress));
  else
    for (uint8_t index = 0; index < 6; index++)
      bleAddress[index] = esp_random();

  const NimBLEAddress capturedRandom(bleAddress, BLE_ADDR_RANDOM);
  const bool exactBleAddress = parsedAddress &&
      (selectedTarget.methods & DroneRemoteID::CAPTURE_METHOD_BLE) &&
      capturedRandom.isStatic();
  if (!exactBleAddress) {
    for (uint8_t index = 0; index < 6; index++)
      bleAddress[index] = esp_random();
    bleAddress[0] = (bleAddress[0] & 0x3F) | 0xC0;
  }

  const uint8_t wifiChannel = selectedTarget.channel >= 1 &&
      selectedTarget.channel <= 11 ? selectedTarget.channel : DEFAULT_WIFI_CHANNEL;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  bool wifiOkay = esp_wifi_set_channel(wifiChannel,
                                       WIFI_SECOND_CHAN_NONE) == ESP_OK;

  NimBLEDevice::init("Marauder-RID-Replay");
  const NimBLEAddress randomAddress(bleAddress, BLE_ADDR_RANDOM);
  bool bleOkay = randomAddress.isStatic() &&
      NimBLEDevice::setOwnAddr(randomAddress) &&
      NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  applyLiveGps();   // prime the broadcast position from our live fix, if any
  const uint8_t* firstMessage = selectedTarget.messages[messageTypes[0]];
  if (bleOkay)
    bleOkay = updateBleAdvertisement(advertising, firstMessage, 0);

  if (!bleOkay && !wifiOkay) {
    drawError("BLE and Wi-Fi captured-data broadcasts could not start.");
    while (!c_btn.justPressed())
      delay(5);
  }
  else {
    const uint32_t started = millis();
    uint32_t nextMessage = started;
    uint32_t nextDraw = 0;
    uint32_t blePackets = bleOkay ? 1 : 0;
    uint32_t wifiPackets = 0;
    uint8_t messageIndex = 0;
    uint8_t counter = 0;
    uint16_t sequence = 0;
    GpsSpoofState gpsState = applyLiveGps();
    Serial.printf("[Drone Spoof] replaying captured RID %s (%u message types); %s\n",
                  selectedTarget.id, messageCount,
                  autoStop ? "maximum 60 seconds" : "no auto-stop (Center to stop)");
    while (!autoStop || millis() - started < MAX_RUNTIME_MS) {
      #ifdef HAS_GPS
        gps_obj.main();   // keep our fix fresh during this blocking loop
      #endif
      if (c_btn.justPressed())
        break;
      const uint32_t now = millis();
      if (static_cast<int32_t>(now - nextMessage) >= 0) {
        // Re-point the replayed drone (and operator) at our current fix before
        // broadcasting, so the position tracks us as we move.
        gpsState = applyLiveGps();
        const uint8_t* message = selectedTarget.messages[messageTypes[messageIndex]];
        if (bleOkay) {
          if (updateBleAdvertisement(advertising, message, counter))
            blePackets++;
          else
            bleOkay = false;
        }
        if (wifiOkay) {
          if (sendWifiBeacon(wifiSource, wifiChannel, message, counter,
                             sequence++))
            wifiPackets++;
          else
            wifiOkay = false;
        }
        messageIndex = (messageIndex + 1) % messageCount;
        counter++;
        nextMessage = now + MESSAGE_INTERVAL_MS;
      }
      if (static_cast<int32_t>(now - nextDraw) >= 0) {
        drawStatus(started, blePackets, wifiPackets, wifiChannel, messageCount,
                   bleOkay, wifiOkay, exactBleAddress, exactWifiAddress, gpsState,
                   autoStop);
        nextDraw = now + 500;
      }
      delay(5);
    }
    Serial.printf("[Drone Spoof] stopped; BLE updates=%lu Wi-Fi frames=%lu\n",
                  static_cast<unsigned long>(blePackets),
                  static_cast<unsigned long>(wifiPackets));
  }

  if (advertising->isAdvertising())
    advertising->stop();
  NimBLEDevice::stopAdvertising();
  delay(60);  // let the BLE host settle so nimble_port_stop() can complete

  // Shared-radio coexistence teardown. The spoofer drives BOTH radios (BLE
  // advertising + raw WiFi beacon TX), so it must hand the radio back clean or
  // the next Bluetooth scan is starved and detects nothing. Release the WiFi
  // side first so the BT controller is torn down with the radio already idle,
  // then fully stop NimBLE. NimBLEDevice::deinit() only clears NimBLE's state
  // and releases the BT controller if its host-stop succeeds; retry until it
  // reports de-initialized so the next NimBLEDevice::init() actually re-inits.
  esp_wifi_set_promiscuous(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // The beacon TX path used raw esp_wifi (esp_wifi_80211_tx / set_channel), and
  // WiFi.mode(WIFI_OFF) does not reliably stop that on the C5, so the WiFi
  // controller can keep the shared radio and starve the next Bluetooth scan.
  // Force the controller fully down; both calls are harmless no-ops if WiFi is
  // already stopped/deinitialized, and the next WiFi feature re-inits from cfg.
  esp_wifi_stop();
  esp_wifi_deinit();
  delay(50);
  NimBLEDevice::deinit(true);
  for (uint8_t attempt = 0; attempt < 3 && NimBLEDevice::isInitialized(); attempt++) {
    delay(60);
    NimBLEDevice::deinit(true);
  }
  // Last-resort controller release if the host stop never completed, so WiFi
  // (and a re-init of BLE) can take the radio back.
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
    esp_bt_controller_disable();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
    esp_bt_controller_deinit();
  delay(20);
  releaseButton(c_btn);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);

  // The spoofer drives WiFi and Bluetooth coexistence together, which leaves the
  // C5's shared RF in a state that a software re-init does not recover: the next
  // Bluetooth scan comes up clean (NimBLE ok, controller enabled, RAM free) yet
  // receives nothing. A reboot restores the radio to its known-good power-on
  // state, and the spoof is a terminal action, so a brief reset here is the
  // reliable way to keep Bluetooth (and WiFi) working afterward.
  #ifdef HAS_SCREEN
    display_obj.tft.fillScreen(TFT_BLACK);
    display_obj.tft.setTextDatum(MC_DATUM);
    display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
    display_obj.tft.drawString("Spoof complete", TFT_WIDTH / 2, (TFT_HEIGHT / 2) - 10, 2);
    display_obj.tft.drawString("Resetting radio...", TFT_WIDTH / 2, (TFT_HEIGHT / 2) + 10, 2);
    display_obj.tft.setTextDatum(TL_DATUM);
  #endif
  delay(1500);
  ESP.restart();
}

}  // namespace DroneRemoteIDSpoofer

#else

namespace DroneRemoteIDSpoofer {
bool selectTarget(const DroneRemoteID::CapturedDrone&) { return false; }
void run(bool) {}
}

#endif
