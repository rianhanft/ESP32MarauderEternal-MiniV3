# ESP32 Marauder Eternal — v1.2.4

**Boards:** ESP32 Marauder **V8** and **Mini V3** (ESP32-C5)
Boot splash now reads **1.2.4**.

This release is almost entirely about **Evil Portal reliability on the
dual-band ESP32-C5** — in particular tracking down why the **Mini V3** portal
broadcast a weak, phone-invisible signal — plus the **Remote ID drone map**
tracking fix on the V8, the **MarauderBox** rename, and a batch of
**LilyGo T-Dongle C5** fixes.

---

## 📡 Evil Portal — the big one

### Mini V3: fixed the weak / "shows on laptop but not my phone" signal
The Mini's portal was beaconing at full TX power (confirmed 20 dBm) yet phones
couldn't see it at range while laptops could. That is the signature of a
**beacon data-rate problem, not a power problem**: with 802.11b disabled, the
beacon goes out at a fast OFDM rate with short reach, so a laptop's radio still
decodes it but a phone at the same distance can't.

- **Force a clean 2.4 GHz PHY (11b/g/n, no Long-Range, no 11ax) on the softAP**
  every time the portal starts, so beacons use the rugged long-range 1 Mbps
  11b rate every phone relies on to find the AP at distance.
- **Plain softAP bring-up** matching the proven Eternal 1.15.x/1.16.x portal
  (no per-activation band-mode pinning, which was re-asserting modem-sleep).
- **Power-save pinned off** so the AP never dozes between beacons.
- The portal status screen now shows the **real effective TX power and PHY**
  (`TX20dBm PHY:11bgn`) for at-a-glance confirmation.

### V8: Evil Portal hardening
- **Force the legacy 2.4 GHz PHY** and log the real AP channel.
- Before hosting, **tear the BT controller down, rebalance Wi-Fi/BT
  coexistence, defragment the internal heap, and drop any leftover promiscuous
  RX** — fixes the "hosting but the page just keeps loading" that could follow a
  BLE or Remote ID scan.

---

## 🛩️ Remote ID drone map (V8)

- **Range scale is now quantized to stable radar steps** instead of re-fitting
  every frame. Previously the lone drone stayed at a near-constant fraction of
  the ring, so the dot "barely moved no matter where you went"; now the dot's
  distance from the receiver tracks the real range and the scale only jumps at a
  threshold. A small range-ring label shows the current scale.
- *(The Mini V3 shows the Remote ID device list in its compact layout — the
  grid/map view is the larger-screen layout, unchanged on the Mini.)*

---

## 🐱 MarauderBox

- The in-house feature menu formerly called **"NyanBox"** is now **"MarauderBox"**
  throughout the menus and attack list.

---

## 🔌 LilyGo T-Dongle C5 (single-button board)

- **On-device menu** enabled, driven by **single-button BOOT navigation**
  (tap = next, hold = select/back).
- **Hold-to-stop crash fixed**, and a **16 MB dual-OTA partition** layout.
- **Recon dashboard** redrawn so it no longer runs off the 160×80 screen.
- **Menu title banner** no longer clipped under the status bar; the clipped
  **scan/attack HUD titles were removed**.
- **MarauderBox features run on the single BOOT button**, and **Settings** was
  added to the start menu.

---

## ✨ Boot splash

- Cat emblem **anti-aliased** (same art, smoother edges) and its **dashed face
  line widened** so it renders on the Mini V3.

---

## 📦 Binaries & flashing

Each board has an **application image** (flash at **`0x10000`**) plus its
**bootloader** (`0x2000`) and **partition table** (`0x8000`) for a full flash.
All are ESP32-C5 images.

| Board | App (0x10000) | Bootloader (0x2000) | Partitions (0x8000) |
|---|---|---|---|
| **V8** | `MarauderBox_V8_1.2.4_APP_0x10000.bin` | `MarauderBox_V8_1.2.4_BOOTLOADER_0x2000.bin` | `MarauderBox_V8_1.2.4_PARTITIONS_0x8000.bin` |
| **Mini V3** | `MarauderBox_MiniV3_1.2.4_APP_0x10000.bin` | `MarauderBox_MiniV3_1.2.4_BOOTLOADER_0x2000.bin` | `MarauderBox_MiniV3_1.2.4_PARTITIONS_0x8000.bin` |

If the board already has a bootloader + partition table (i.e. you have flashed
it before), flashing just the **app image at `0x10000`** is enough.

### Build provenance
- **V8** built from branch `release/1.2.4` (the V8 line + version bump; Remote
  ID map and Evil Portal unchanged from the V8 development line).
- **Mini V3** built from branch `mini-evilportal-rollback` (the exact tested
  Evil Portal build; commit `eb6e923`).
