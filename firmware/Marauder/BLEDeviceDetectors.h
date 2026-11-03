#pragma once

#include <Arduino.h>
#include "IBeacon.h"
class TFT_eSprite;

namespace BLEDeviceDetectors {

enum class DetectorType : uint8_t {
  Meshtastic,
  MeshCore,
  SmartTag,
  Tile,
  Axon,
  IBeacon,
  NyanBox,
};

// fromSerial = launched over the serial CLI (web remote): exit on any serial
// input (web "Stop"/stopscan). Detections already stream to Serial as
// "[BLE ...]" lines, so the browser console shows hits live either way.
void run(DetectorType detector, uint32_t durationMs = 0, bool fromSerial = false);
bool observeIBeaconAdvertisement(const uint8_t* data, size_t length, const char* address, int8_t rssi, uint32_t now);
unsigned iBeaconCount();
bool snapshotIBeacon(unsigned index, marauder::IBeaconObservation& out);
void printIBeacons();
void runIBeaconChecks();
bool renderIBeaconPreview(TFT_eSprite& frame);

}  // namespace BLEDeviceDetectors
