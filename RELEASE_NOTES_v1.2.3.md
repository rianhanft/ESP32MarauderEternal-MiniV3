# ESP32 Marauder Eternal — v1.2.3

**Boards:** ESP32 Marauder **V8** and **Mini V3** (ESP32-C5)
Boot splash now reads **1.2.3**.

This release reworks the **targeted WiFi attack flow**, puts a **live status
screen on every attack**, and teaches the **Drone Spoof** to broadcast your
Marauder's **own GPS position** — plus a recon count fix and new automated tests.

---

## 🎯 New targeted-attack flow — "Target Mode"

Station-targeted attacks (Deauth Targeted, Bad Msg Targeted, Assoc Sleep
Targeted) now walk a clean, official-style path:

**Choose Targets → Review / Continue → Target Mode → Review Attack → Start**

- **Choose Targets** — tap APs to drop them in/out of the profile (green = in),
  with **Select by SSID** to grab a whole ESS in one tap.
- **Target Mode** — decide how the selected AP(s) are hit:
  - **All Stations** — target every client the box has seen on those APs.
  - **Pick Stations** — a per-client picker across the selected APs, with
    *Select AP Stations* / *Clear AP Stations* shortcuts and *Add Another AP*.
  - **Remove from Profile** — drop the AP(s) and go back to Choose Targets.
- **Review Attack** — summary + **Start Attack**.

(The old "Broadcast" row was removed from Target Mode: targeted deauth only hits
selected clients, so whole-AP broadcast lives in **Deauth Flood** where it
belongs.)

---

## 📡 Flood vs. Targeted, made consistent

Every family now has a clean split:

| Flood (pick SSIDs → Review → Start) | Targeted (Target Mode → All / Pick) |
|---|---|
| Deauth Flood | Deauth Targeted |
| **Bad Msg** | Bad Msg Targeted |
| **Assoc Sleep** | Assoc Sleep Targeted |

**Bad Msg** and **Assoc Sleep** are now **flood** attacks — just pick the
APs/SSIDs you want and hit Start; they hit every known client of each selected
AP, exactly like Deauth Flood. The per-client variants keep the Target Mode flow.

---

## 📊 Live attack HUD on more attacks

The unified live status screen (attack name, blinking TX, target context,
packets / packets-per-sec / elapsed) now shows up where it used to be missing:

- **Deauth Targeted** now draws the HUD like Deauth Flood.
- **Channel Switch (CSA)** and **Quiet Time** — previously ran with a frozen
  screen — now show the live HUD (**CHANNEL SWITCH** / **QUIET TIME**), cycling
  the selected AP(s) with SSID / BSSID / channel / RSSI.

---

## 🛩️ Drone Spoof now broadcasts your live GPS

The Remote ID **Drone Spoof** used to replay a captured drone's frozen
coordinates. Now, with a GPS fix, it rewrites the broadcast position to **your
Marauder's own live location** every cycle:

- A second Marauder running **Drone Remote ID** plots the spoofed drone at
  **your real position**, so its on-map distance reflects how far apart the two
  units are — and it **moves as you move**.
- The spoof now reports a **tight accuracy (<3 m)** and a **real altitude**
  instead of "unknown," so detectors plot a precise point instead of a fuzzy one.
- **Drone Spoof (SD)** now runs with **no auto-stop** — it broadcasts until you
  press Center. (The captured-replay Drone Spoof keeps its 60-second auto-stop.)
- The status screen shows **`GPS: live <lat>,<lon>`** when it's tracking.

*Authorized testing between devices you own.*

---

## 🐛 Fixes

- **Recon AP over-count** — Recon was reporting ~400 APs in a room with ~100
  (while Scan AP, on the same list, maxed around 120). Its positional cursor
  re-counted the whole list every time stale APs were pruned; the headline
  AP / station / BLE figures now track the real, de-duplicated device counts.

---

## 🧪 Under the hood

- New **host-side tests** (no hardware needed) for the pure logic that
  regressions hide in: the recon dashboard helpers (layout, channel paging,
  churn scaling, RSSI mapping, signal trend, TTL rollover, truncation) and the
  `Remote.py` web-bridge parsers. Wired into CI so they run on every push.

---

## Flashing

Flash the matching `.bin` with the Marauder web installer (app image, offset
`0x10000`) or esptool.

- `marauder_eternal_v1.2.3_v8.bin`
- `marauder_eternal_v1.2.3_mini_v3.bin`
