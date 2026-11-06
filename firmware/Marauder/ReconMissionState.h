#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ReconSource : uint8_t { AP_LIST = 0, STATION_LIST = 1, BLE_LIST = 2 };

struct ReconRange {
  size_t begin;
  size_t end;
};

inline uint8_t reconRssiLevel(int8_t rssi) {
  if (rssi <= -127) return 0;
  if (rssi <= -100) return 1;
  if (rssi >= -30) return 8;
  return static_cast<uint8_t>(1 + ((rssi + 100) * 7) / 70);
}

class ReconMissionState {
 public:
  void reset() {
    for (size_t& cursor : cursors) cursor = 0;
  }

  ReconRange consume(ReconSource source, size_t size) {
    size_t& cursor = cursors[static_cast<uint8_t>(source)];
    // Only entries appended since the last pass are new. The device lists also
    // shrink while recon runs (stale APs/stations/BLE get pruned), and when a
    // list shrinks we must NOT rewind the cursor to 0 -- doing so re-counts
    // every surviving entry as "new" on the next pass, which ballooned the
    // recon AP/station/BLE tallies to several times the real device count.
    // A shrink simply yields an empty range; the headline counts are taken
    // from the live list sizes in ReconMission::observeLists().
    const ReconRange range = {cursor < size ? cursor : size, size};
    cursor = size;
    return range;
  }

 private:
  size_t cursors[3] = {0, 0, 0};
};
