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
#include "ReconUi.h"

#include <cassert>
#include <cstring>
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

  // ----------------------------------------------------------------------
  // ReconUi.h pure dashboard/geometry helpers.
  // ----------------------------------------------------------------------

  // Layout picked from screen dimensions (Mini V3 128x128, V8 240x320, ...).
  assert(reconLayoutFor(128, 128) == ReconLayout::COMPACT_SQUARE);
  assert(reconLayoutFor(135, 160) == ReconLayout::COMPACT_SQUARE);  // boundary
  assert(reconLayoutFor(240, 128) == ReconLayout::COMPACT_LANDSCAPE);
  assert(reconLayoutFor(160, 240) == ReconLayout::NARROW_PORTRAIT);
  assert(reconLayoutFor(136, 160) == ReconLayout::NARROW_PORTRAIT);
  assert(reconLayoutFor(240, 320) == ReconLayout::LARGE);
  assert(reconRelationshipRows(ReconLayout::LARGE) == 3);
  assert(reconRelationshipRows(ReconLayout::NARROW_PORTRAIT) == 1);
  assert(reconRelationshipRows(ReconLayout::COMPACT_SQUARE) == 0);
  assert(reconRelationshipRows(ReconLayout::COMPACT_LANDSCAPE) == 0);

  // Channel paging: 14 channels -> 2 pages of 8, cycling every 4s; 8 -> 1 page.
  assert(reconChannelPage(0, 0) == 0);          // no channels
  assert(reconChannelPage(0, 14) == 0);
  assert(reconChannelPage(4000, 14) == 1);
  assert(reconChannelPage(8000, 14) == 0);      // wraps (2 pages)
  assert(reconChannelPage(12000, 14) == 1);
  assert(reconChannelPage(99999, 8) == 0);      // single page -> always 0
  assert(reconChannelsOnPage(0, 14) == 8);
  assert(reconChannelsOnPage(1, 14) == 6);      // partial last page
  assert(reconChannelsOnPage(2, 14) == 0);      // past the end
  assert(reconChannelsOnPage(0, 8) == 8);
  assert(reconChannelsOnPage(1, 8) == 0);

  // Churn-bar height: 0 stays 0, scales with value/peak, clamps to height,
  // and a 0 peak doesn't divide by zero.
  assert(reconChurnHeight(0, 10, 58) == 0);
  assert(reconChurnHeight(5, 0, 58) == 58);     // peak clamped to 1 -> full
  assert(reconChurnHeight(7, 10, 0) == 0);      // no height
  assert(reconChurnHeight(5, 10, 58) == 29);    // 1 + 5*57/10
  assert(reconChurnHeight(10, 10, 58) == 58);   // exactly full
  assert(reconChurnHeight(10, 5, 58) == 58);    // over-peak clamps to height
  assert(reconChurnHeight(1, 100, 58) == 1);    // floor is 1 when non-zero

  // RSSI -> signal bars / plot level / proximity label (boundary values).
  assert(reconSignalSegments(-127) == 0);
  assert(reconSignalSegments(-90) == 1);
  assert(reconSignalSegments(-78) == 2);
  assert(reconSignalSegments(-67) == 3);
  assert(reconSignalSegments(-55) == 4);
  assert(reconSignalSegments(-54) == 5);
  assert(reconRssiPlotLevel(-127) == 0);
  assert(reconRssiPlotLevel(-100) == 1);
  assert(reconRssiPlotLevel(-35) == 100);
  assert(reconRssiPlotLevel(0) == 100);
  assert(reconRssiPlotLevel(-60) == 61);        // 1 + 40*99/65
  assert(!strcmp(reconProximityLabel(-127), "NO SIGNAL"));
  assert(!strcmp(reconProximityLabel(-82), "FAR"));
  assert(!strcmp(reconProximityLabel(-81), "MID"));
  assert(!strcmp(reconProximityLabel(-65), "MID"));
  assert(!strcmp(reconProximityLabel(-64), "NEAR"));

  // Fox-hunt signal trend: needs >=6 dBm of movement, ignores dead (-127)
  // samples at the ends, and stays STEADY when there's nothing to compare.
  {
    const int8_t approaching[] = {-80, -75, -70};
    const int8_t departing[]   = {-70, -75, -80};
    const int8_t steady[]      = {-70, -68, -69};
    const int8_t padded[]      = {-127, -80, -70, -127};
    const int8_t all_dead[]    = {-127, -127};
    const int8_t one[]         = {-70};
    assert(reconSignalTrend(approaching, 3) == ReconSignalTrend::APPROACHING);
    assert(reconSignalTrend(departing, 3) == ReconSignalTrend::DEPARTING);
    assert(reconSignalTrend(steady, 3) == ReconSignalTrend::STEADY);
    assert(reconSignalTrend(padded, 4) == ReconSignalTrend::APPROACHING);
    assert(reconSignalTrend(all_dead, 2) == ReconSignalTrend::STEADY);
    assert(reconSignalTrend(one, 1) == ReconSignalTrend::STEADY);
    assert(reconSignalTrend(nullptr, 5) == ReconSignalTrend::STEADY);
  }

  // Stale-device expiry, including the 32-bit millis() rollover (now < last_seen).
  assert(!reconDeviceExpired(100000, 0));                 // 0 = never seen
  assert(!reconDeviceExpired(100000, 50000));             // 50s < 120s TTL
  assert(reconDeviceExpired(200000, 50000));              // 150s >= TTL
  assert(!reconDeviceExpired(100, 4294967200u));          // wrap: only ~196ms old
  assert(reconDeviceExpired(129904u, 4294967200u));       // wrap: ~130s old -> expired

  // Middle-ellipsis truncation into a fixed buffer.
  {
    char out[8];
    reconTruncate("abc", out, sizeof(out));
    assert(!strcmp(out, "abc"));                 // fits whole
    reconTruncate("abcdefghij", out, sizeof(out));
    assert(!strcmp(out, "ab..hij"));             // 7 visible: 2 + ".." + 3
    char tiny[4];
    reconTruncate("abcdef", tiny, sizeof(tiny));
    assert(!strcmp(tiny, "abc"));                // too small for ellipsis
    char small[6];
    reconTruncate("123456789", small, sizeof(small));
    assert(!strcmp(small, "1..89"));
    char one[1];
    reconTruncate("x", one, sizeof(one));        // size 1 -> no room, no crash
    assert(one[0] == '\0' || one[0] == 'x');     // defensive: either empty or set
  }

  std::cout << "Recon UI helpers: layout, channel paging, churn scaling, RSSI "
               "mapping, signal trend, TTL rollover and truncation passed\n";
  return 0;
}
