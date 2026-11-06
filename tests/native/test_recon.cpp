// Regression test for the Recon dashboard AP/station/BLE over-count.
//
// Recon walks the shared scan lists with a positional cursor (ReconMissionState)
// and treats entries appended since the last pass as "new". Those same lists are
// also pruned of stale devices while a mission runs, so they shrink as well as
// grow. The original consume() rewound the cursor to 0 on any shrink, which made
// the next pass re-count every surviving entry as new -- a stable ~120-AP room
// reported 400+. These checks pin the cursor to tail-only appends across prune
// and clear, and confirm the per-source cursors stay independent.

#include "ReconMissionState.h"

#include <cassert>
#include <iostream>

int main() {
  // reconRssiLevel buckets (pure helper that ships in the same header).
  assert(reconRssiLevel(-127) == 0);
  assert(reconRssiLevel(-100) == 1);
  assert(reconRssiLevel(-30) == 8);
  assert(reconRssiLevel(0) == 8);
  assert(reconRssiLevel(-65) >= 1 && reconRssiLevel(-65) <= 8);

  ReconMissionState state;

  // Drive the AP cursor exactly as ReconMission does: prime at mission start
  // (fresh scan, empty list), then consume() the live list size each pass. A
  // cumulative tally of the returned ranges must grow ONLY by genuinely new
  // tail appends.
  state.reset();
  state.consume(ReconSource::AP_LIST, 0);  // prime

  size_t logged = 0;
  auto observe = [&](size_t size) {
    const ReconRange r = state.consume(ReconSource::AP_LIST, size);
    assert(r.begin <= r.end);  // never malformed, even on a shrink
    assert(r.end == size);
    logged += r.end - r.begin;
  };

  // Grow to 120 unique APs across many passes.
  for (size_t n = 1; n <= 120; ++n) observe(n);
  assert(logged == 120);

  // Prune drops 5 stale APs. Pre-fix this rewound the cursor and re-logged all
  // 115 survivors; now it must add nothing.
  observe(115);
  assert(logged == 120);

  // 3 APs rejoin -> only the 3 fresh tail entries are new.
  observe(118);
  assert(logged == 123);

  // Churn hard around a stable ~120: each cycle prunes 10 then regains 10.
  for (int cycle = 0; cycle < 50; ++cycle) {
    observe(110);  // prune: empty range
    observe(120);  // regain: exactly 10 new
  }
  assert(logged == 123 + 50 * 10);  // 623, strictly tail growth

  // A full user clear then regrow starts clean (no negative/garbage range).
  observe(0);
  const size_t before_clear = logged;
  observe(7);
  assert(logged == before_clear + 7);

  // Per-source cursors are independent.
  state.reset();
  const ReconRange ap = state.consume(ReconSource::AP_LIST, 10);
  const ReconRange sta = state.consume(ReconSource::STATION_LIST, 4);
  const ReconRange ble = state.consume(ReconSource::BLE_LIST, 2);
  assert(ap.begin == 0 && ap.end == 10);
  assert(sta.begin == 0 && sta.end == 4);
  assert(ble.begin == 0 && ble.end == 2);
  // Second pass on an unchanged AP list yields an empty range.
  const ReconRange ap_again = state.consume(ReconSource::AP_LIST, 10);
  assert(ap_again.begin == 10 && ap_again.end == 10);

  std::cout << "Recon cursor counts tail-only appends; survives prune/clear "
               "without re-counting\n";
  return 0;
}
