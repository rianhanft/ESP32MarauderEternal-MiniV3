# ESP32 Marauder Eternal — v1.2.0

**Boards:** ESP32 Marauder **V8** and **Mini V3** (ESP32-C5)
**Base:** official ESP32 Marauder **v1.19.2**

This release rolls up everything added since v1.1.4 into a single versioned build.
The boot splash now reads **1.2.0**.

---

## Highlights

### 🔍 Live device-detail "preview" screens
Detections now open a dedicated, multi-page **detail/preview screen** instead of
just a list row — matching the cleaner device-detail UI from the newest upstream
Marauder:

- **Drone / Remote ID** — paged detail card (operator ID, UAV ID, location/altitude,
  speed, RSSI, first/last seen) with a live "preview" view.
- **iBeacon** — detail card (UUID, major/minor, address, RSSI, advertisement count).

### ✨ UX polish
- Guided **Choose Targets → Review → Start** attack flow (no more blind commands).
- Redrawn boot splash: white cat-in-a-box emblem + frame, version centered.
- Cleaner stats/detail screens and menu layout aligned with upstream conventions.

---

## WiFi — guided attack workflow
- **WiFi → Attacks** now walks you through *Choose Targets → Review → Start*.
- **Mixed per-AP station profiles** — assign Deauth / Bad Msg / Assoc Sleep
  independently per access point in one run.
- **Select by SSID** — select every AP broadcasting one SSID at once
  (without the old multi-second freeze).
- **EAPOL / PMKID target picker** + Force-PMKID deauth fix; fixed the black
  EAPOL/PMKID stats screen on targeted (list) scans.
- **Deauth Flood** sets the channel once per AP (not per frame) so the broadcast
  sweep stays fast.
- **Evil Portal HTML picker** built into the workflow, plus prompter captive-page
  popup and more reliable softAP start after BLE features.

## Bluetooth / BLE & drones
- Full **Drone cluster**: Remote ID scan + export, **Drone Spoof**, and
  **Drone Spoof (SD)** (spoof from an SD file), with clean radio teardown so
  Bluetooth works again afterward.
- **6 BLE sniffers / detectors**, **iBeacon detector**, **Device Scout**
  (crash-hardened via a PSRAM snapshot buffer), **Camera** detect/deauther.
- **RF Scanner** and a **Jam Detector** with the constant-false-positive bug fixed.

## Stability & memory
- Reworked **WiFi ↔ BT radio handoff** so scans/Evil Portal/wardrive survive
  switching radios.
- **Scan-AP-after-BLE** fixed: BLE device list moved to **PSRAM**, BLE radio
  released before AP scans, leftover AP lists freed before NimBLE init.
- **Heap/PSRAM policy**: Eternal feature buffers and menus moved to PSRAM to free
  internal SRAM (fixes "scans find nothing" and scan-after-BLE regressions).
- Fresh scan lists each run (fixes slow/stuck/dead scans); channel-sweep reset so
  dense 5 GHz channels don't stall.

## Companion tool — Marauder Remote (`tools/marauder_remote.py`)
Control the Marauder headless from a phone or laptop over USB/serial:
- Device-style menu mirroring the full CLI surface; SSID/AP pickers.
- Evil Portal flow with AP + **HTML file** pickers.
- Terminal-style, color-coded serial console with a live activity banner.
- Optional **public link** via Cloudflare tunnel; runs NyanBox / device-screen
  tools over the web.

## New hardware targets
- **ESP32 Mini V3 (C5)** — full merged feature set (a primary target of this release).
- **LilyGo T-Dongle C5** *(experimental)* — buildable/flashable target with
  USB-CDC-on-boot, a fix for the USB-connect grey screen, and a fix for
  SD/wardrive writes. Its binary is available on request.

---

## Flashing
Flash the matching `.bin` for your board with the Marauder web installer
(app image, offset `0x10000`) or esptool. **V8** and **Mini V3** binaries are
attached to this release.

- `marauder_eternal_v1.2.0_v8.bin`
- `marauder_eternal_v1.2.0_mini_v3.bin`
