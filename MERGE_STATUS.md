# Marauder v8 (official UI) + Eternal features — merge status

Goal: recreate the firmware on the **official ESP32 Marauder v1.19.2** codebase
(official menu tree / UI), running on the **ESP32‑C5 Marauder v8** board, and
port in the features that are unique to **Marauder Eternal**.

This document is the single source of truth for what is done and what remains,
so the work can continue across sessions without re‑deriving context.

## TL;DR — the key finding

Official Marauder **v1.19.2 already contains almost everything Eternal is known
for**. Eternal forked from official 1.18.0; official has since advanced to
1.19.2 and absorbed the headline attacks/sniffers. Already upstream, on the
official UI, already building for the C5 v8 board:

- FindMy attacks (live scan, sound, target menu) — `BT_ATTACK_FINDMY_LIVE`
- Flock + flock‑camera detection — `BT_SCAN_FLOCK`, `isFlockCamera`
- AirTag spoofing — `BT_SPOOF_AIRTAG`, `spoofat`
- Drone Remote ID decoding — `RemoteIdDecoder`, `RemoteIdModel`
- Fox Hunt / Recon / target sorting — `FoxHuntTarget`, `ReconMission`, `TargetListSort`
- All standard WiFi/BLE sniffers, deauth, beacon, evil portal, etc.

So the merge is mostly: **base off official v1.19.2** and port the genuinely
Eternal‑only modules into the official menu tree.

## Repository layout after the merge

| Path | Meaning |
|---|---|
| `firmware/Marauder/` | **New merged firmware.** Official v1.19.2 source + ported Eternal modules. The `.ino` is `Marauder.ino` (must match folder name for arduino‑cli). |
| `firmware/MarauderEternal/` | Original Eternal firmware, kept as the porting reference. Remove once the merge is complete. |
| `libraries/` | Pinned libraries. C5‑capable `TFT_eSPI` (bundled by Eternal), plus `Adafruit_NeoPixel` and `Adafruit_MAX1704X` added for the v8/Pancake build. |
| `scripts/build.sh` | Parameterized build: `scripts/build.sh <PANCAKE\|V8\|MINI_V3\|WS_C5_35>`. |

## Board targets

"Marauder v8" is a real, first‑class board in official v1.19.2 — ESP32‑C5,
ST7796‑class 320×480 display, its own pin map (`MARAUDER_V8`). **It is
touch‑only** (`HAS_TOUCH`; `HAS_BUTTONS` is *not* defined). Pancake ("Pancake
Marauder V8") is the sibling board that has physical buttons.

| Target | Define | Input | FQBN | Status |
|---|---|---|---|---|
| Pancake ("Pancake Marauder V8") | `MARAUDER_PANCAKE` | buttons + touch | esp32c5 default_8MB PSRAM | **Verified building** (public, pin‑accurate) |
| Marauder v8 | `MARAUDER_V8` | **touch‑only** | esp32c5 default_8MB PSRAM | **Builds**; display pins best‑effort (see limitation) |
| Mini v3 | `MARAUDER_MINI_V3` | buttons | esp32c5 custom PSRAM | Original Eternal target |

### Important — the button‑driven ports and v8
The Eternal **games** and **WirelessDeviceScout** are button‑driven (they read
`u/d/l/r/c_btn` and are guarded by `HAS_BUTTONS`). On **Pancake** and **Mini v3**
they are compiled in and usable. On **v8 (touch‑only)** the `HAS_BUTTONS` guard
compiles the menu wiring out, so these entries do not appear and the game code is
dead‑stripped from the v8 image (confirmed: v8 image unchanged vs baseline; the
game `.o` files still compile cleanly, proving API compatibility). To use the
games/scout **on v8**, add a touch control scheme (map touch zones to the
`GameInput` button edges) — tracked as follow‑up. Everything else (the official
UI + all official attacks/sniffers) runs on v8 via touch today.

### Known limitation — v8 display pins
Official's *exact* v8 TFT_eSPI User_Setup (display driver + SPI GPIO) is published
only in justcallmekoko's **private** `TFT_eSPI_JCMK` fork; it is not in the public
repo (only STL mechanicals, no schematic). `libraries/TFT_eSPI/User_Setup_marauder_v8.h`
is therefore derived from the public **Pancake** setup (same ST7796 320×480 panel
class). It compiles and links; if the panel is blank/garbled on real v8 hardware,
correct the `TFT_*` pins there from the v8 schematic. The **Pancake** target is
fully pin‑accurate today.

## Build toolchain (verified in this environment)

- `arduino-cli` + **ESP32 Arduino core 3.3.4** (installs the C5 toolchain).
- Libraries: Eternal's bundled C5 `TFT_eSPI` (v2.5.44) + NimBLE 2.3.8 +
  ArduinoJson + AsyncTCP + ESPAsyncWebServer + ESP32Ping + MicroNMEA +
  EspSoftwareSerial + Adafruit_NeoPixel + Adafruit_MAX1704X + Adafruit_BusIO.
- Linker flag: `-Wl,-zmuldefs` (required for the C5 core). Do **not** carry over
  Eternal's `--wrap=ieee80211_raw_frame_sanity_check` flags — that `__wrap_`
  symbol lives in Eternal's `WiFiScan.cpp`, not official's, and dangles the link.

### Verified builds (this environment, core 3.3.4)
| Build | Result |
|---|---|
| Official v1.19.2, V8, no ports | 2,278,880 B (68%), RAM 27% |
| Merged, **V8** (Scout+games compiled, stripped — touch board) | 2,279,134 B (68%), RAM 27% |
| Merged, **Pancake** (Scout+games linked in — button board) | 2,286,782 B (68%), RAM 28% |

~1 MB program + ~70% RAM free remain for the rest of the ports.

## Feature delta — Eternal‑only modules to port

| Cluster | Files | Menu target | Status |
|---|---|---|---|
| **WirelessDeviceScout** (WiFi sniffer) | `WirelessDeviceScout.{cpp,h}` | WiFi > Sniffers > "Device Scout" | **Ported + wired**; compiles. Button boards only (self‑guards `HAS_BUTTONS`). |
| **Games** (button‑driven) | `GameInput.h`, `GameFrameBuffer.h`, 10×`*Game.{cpp,h}` | Device > Games | **Ported + wired** (guarded by `HAS_BUTTONS`). Active on Pancake/Mini‑v3; needs touch controls for v8. |
| **USB Screen Mirror** | `ScreenMirror.*`, `MirrorInput.*`, `MirrorCanvas.*` | Device | Not started |
| **Drone suite (incl. Remote ID spoofer)** | `DroneRemoteID.*`, `DroneTracking.*`, `DroneExport.*`, `DroneRemoteIDSpoofer.*`, `StoragePaths.h` | WiFi/BT (next to official Remote ID) | Not started — reconcile with official `RemoteIdDecoder`/`RemoteIdModel` |
| **Eternal WiFi‑OTA** | `WiFiOTA.*`, `OtaImageWriter.*`, `OtaTransport.*`, `OtaPolicy.h`, `DeviceClock.*`, `UpdateKeyboard.*` | Device > Update | Not started — official has its own `MarauderOTA`; evaluate overlap before porting |

## Porting pattern (proven)

Both repos share the same global‑object architecture (`display_obj`,
`wifi_scan_obj`, `menu_function_obj`, `sd_obj`, `gps_obj`, `settings_obj`,
`buffer_obj`) and the same `Switches` button globals (`u/d/l/r/c/b_btn`). So a
module port is:

1. Copy the module's `.cpp/.h` into `firmware/Marauder/`.
2. Reconcile small API diffs. Known one: Eternal's `Switches::isDown()` →
   added as an inline alias for official's `isPressedNow()` in `Switches.h`.
3. Wire into the official **on‑demand** menu system (not static `addNodes` in
   `RunSetup`): menus declare a `MenuBuildKind` and are populated in the big
   `switch` around `MenuFunctions.cpp` (the `case MenuBuildKind::…` block). Add
   an `addNodes(menu, "Label", COLOR, ICON, [this](){ Module::run(); … })` leaf
   in the right case. A new submenu also needs: a `Menu x;` field in
   `MenuFunctions.h`, a `MenuBuildKind::X` enum value, init in `RunSetup`
   (`name`/`buildKind`/`parentMenu`), and its own `case`.
4. Build with `scripts/build.sh` and fix.

Games draw purely through `display_obj.tft.*` (TFT_eSPI) and run as a **blocking**
`::run()` that returns on exit — highly portable.

## Next steps

1. Confirm the Scout + Games build (this session).
2. Port the ScreenMirror cluster (3 files, Device menu leaf).
3. Port the Drone suite; decide whether to keep Eternal's richer Remote ID
   implementation alongside or instead of official's decoder.
4. Evaluate Eternal WiFi‑OTA vs official MarauderOTA; port only the gap.
5. Flash to real v8 hardware and verify display pins + touch.
