#pragma once

#include <stddef.h>
#include <stdint.h>

namespace marauder {

struct IBeaconPayload {
  uint8_t uuid[16] = {};
  uint16_t major = 0;
  uint16_t minor = 0;
  int8_t measured_power = 0;
};

inline bool sameIBeacon(const IBeaconPayload& lhs, const IBeaconPayload& rhs) {
  if (lhs.major != rhs.major || lhs.minor != rhs.minor) return false;
  for (size_t index = 0; index < sizeof(lhs.uuid); index++) {
    if (lhs.uuid[index] != rhs.uuid[index]) return false;
  }
  return true;
}

inline bool parseIBeacon(const uint8_t* payload, size_t length, IBeaconPayload& beacon) {
  if (!payload || length < 25) return false;
  for (size_t offset = 0; offset + 25 <= length; offset++) {
    if (payload[offset] != 0x4c || payload[offset + 1] != 0x00 ||
        payload[offset + 2] != 0x02 || payload[offset + 3] != 0x15)
      continue;
    for (size_t index = 0; index < sizeof(beacon.uuid); index++)
      beacon.uuid[index] = payload[offset + 4 + index];
    beacon.major = (static_cast<uint16_t>(payload[offset + 20]) << 8) |
                   payload[offset + 21];
    beacon.minor = (static_cast<uint16_t>(payload[offset + 22]) << 8) |
                   payload[offset + 23];
    beacon.measured_power = static_cast<int8_t>(payload[offset + 24]);
    return true;
  }
  return false;
}

// --- Appended from Marauder Eternal for ported BLEDeviceDetectors sniffers ---
struct IBeaconObservation {
  IBeaconPayload beacon;
  char address[18] = {};
  int8_t rssi = 0;
  uint32_t first_seen = 0, last_seen = 0, advertisements = 0;
};

inline int retainIBeacon(IBeaconObservation* records, unsigned& count, unsigned capacity,
                         const IBeaconPayload& beacon, const char* address,
                         int8_t rssi, uint32_t now) {
  if (!records || !capacity || count > capacity || !address || strlen(address) != 17) return -1;
  unsigned slot = count;
  for (unsigned i = 0; i < count; ++i)
    if (sameIBeacon(records[i].beacon, beacon)) { slot = i; break; }
  if (slot == count) {
    if (count >= capacity) return -1; // Keep retained identities stable until the next scan.
    ++count;
    records[slot] = IBeaconObservation(); records[slot].first_seen = now;
  }
  auto& record = records[slot];
  record.beacon = beacon; strcpy(record.address, address); record.rssi = rssi;
  record.last_seen = now;
  if (record.advertisements != UINT32_MAX) ++record.advertisements;
  return static_cast<int>(slot);
}

inline bool formatIBeaconUuid(const IBeaconPayload& beacon, char* out, size_t size) {
  if (!out || size < 37) return false;
  const auto* u = beacon.uuid;
  snprintf(out, size, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
      u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
  return true;
}
// --- end Eternal append ---

}  // namespace marauder
