# ESP32 Marauder Eternal — v1.2.2

**Boards:** ESP32 Marauder **V8** and **Mini V3** (ESP32-C5)
Boot splash now reads **1.2.2**.

This release is headlined by a brand-new **Fox Hunt UI** and a fix for the
long-standing **Scan AP channel-hopping stall**.

---

## 🎯 New Fox Hunt UI — a radial signal gauge

Fox Hunt (pick a target, then physically walk it down by signal strength) used to
be a flat number + a bar. It's now a proper **radial instrument**, shared by the
WiFi (`Signal Strength`) and BLE fox hunts:

- A **270° arc** that fills with signal strength and is color-coded red → green,
  drawn with the display's anti-aliased arc engine.
- The **live RSSI big in the center** of the dial (+ `dBm`), color-matched.
- A **white peak-hold notch** on the arc marking your **closest approach** — beat
  it and the fill crosses the notch — plus a **`BEST -xx`** readout.
- A bold **WARMER ▲ / COLDER ▼ / HOLD** cue so you know you're walking the right
  direction without staring at the number.
- A **SEARCHING** state with a rotating scan sweep when the target goes quiet
  (while the box hops channels looking for it again), instead of a frozen reading.

Everything resets cleanly when you pick a new target, and it's drawn correctly on
both the V8 (240×320) and the Mini V3 (128×128).

---

## 📡 Fixed: Scan AP getting stuck on a channel

In busy, AP-dense environments the WiFi scan would **stall on one channel**
(classically a crowded 5 GHz channel like 132) and stop sweeping — it looked
frozen even though RAM was fine.

**Root cause:** on the single-core C5, the Wi-Fi receive callback ran heavy work
on *every* received frame — matching each beacon against the whole AP list by
**copying** the entry, and re-querying the heap on every packet. On a flooded
channel that starved the app task that advances the channel hop, so the sweep
never moved on.

**Fix:** the hot receive path now compares and updates APs **by reference**
(touching only the 6 BSSID bytes, copying nothing). The callback keeps up with the
beacon storm, the channel-hop timer gets CPU again, and the sweep runs smoothly
even in a crowded RF room.

---

## Also in this release (since v1.2.0)

- **Unified live attack HUD** — one clean, color-coded status screen across the
  WiFi attacks (deauth, beacon spam, bad msg, assoc sleep, etc.): attack name,
  blinking TX indicator, target context (SSID / BSSID / channel / RSSI), and live
  packets / packets-per-sec / elapsed.
- **Evil Portal dashboard** — hosted SSID, armed HTML page, connected client
  count, and an **accumulating list of every captured `user : pass`** (instead of
  flashing one and losing it). Full history still saved to the SD log.
- **T-Dongle C5 SD fix** — disabled the APA102 status LED, which on that board is
  wired to the SD card's MOSI/SCK pins and was corrupting SD/wardrive access.

---

## Flashing

Flash the matching `.bin` with the Marauder web installer (app image, offset
`0x10000`) or esptool.

- `marauder_eternal_v1.2.2_v8.bin`
- `marauder_eternal_v1.2.2_mini_v3.bin`
