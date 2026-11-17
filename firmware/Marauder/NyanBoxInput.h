// Single-button (BOOT) interaction + rotation-aware screen metrics for the
// ported "NyanBox" Marauder Eternal features (Device Scout, Jam Detector,
// Camera Detector, Drone Remote ID, BLE detectors, ...).
//
// Those features were written for boards with a full U/D/L/R/C button cluster
// (Mini V3) or touch zones (V8). On a single-button board like the LilyGo
// T-Dongle C5 (only the BOOT button, SINGLE_BUTTON_NAV) they were compiled out
// entirely and did nothing when selected. This header gives them one shared
// interaction model so they run on the dongle too:
//
//   * Tap  (press shorter than the hold threshold) = "next" / advance.
//   * Hold (press reaches the hold threshold)       = "select" / exit.
//
// which mirrors the single-button menu navigation the rest of the dongle build
// already uses (tap to move, hold to act).
//
// It also exposes rotation-aware screen dimensions. The feature draw code was
// written against the TFT_WIDTH / TFT_HEIGHT macros, which are the *unrotated*
// panel size. On the dongle the panel is 80x160 but runs rotated to 160x80, so
// those macros are wrong there. nbScreenW()/nbScreenH() return the live,
// rotation-aware size (tft.width()/height()), which equals the macros on every
// other board.
#pragma once

#include "configs.h"

#ifdef HAS_SCREEN
  #include "Display.h"
  extern Display display_obj;
  // Live, rotation-correct drawable size. Use these instead of TFT_WIDTH /
  // TFT_HEIGHT in feature draw code so it fills the screen on the rotated dongle
  // panel while staying identical on every other board.
  static inline int16_t nbScreenW() { return display_obj.tft.width(); }
  static inline int16_t nbScreenH() { return display_obj.tft.height(); }
#endif

#ifdef SINGLE_BUTTON_NAV
  #include "Switches.h"

  namespace NyanBoxInput {
    enum Event : uint8_t { NONE = 0, TAP = 1, HOLD = 2 };

    // Per-loop state for one button. Declare one of these before the loop and
    // feed it to poll() each iteration.
    struct SingleButton {
      bool pressing = false;   // a press span is in progress
      bool holdFired = false;  // the hold event already fired for this span
    };

    // Non-blocking single-button poll. Call once per loop iteration.
    //   HOLD fires once, the instant the press reaches the hold threshold, so
    //        the user does not have to release to trigger it (matches the menu).
    //   TAP  fires on release, only when the span never reached hold.
    inline Event poll(Switches& btn, SingleButton& st) {
      btn.justPressed();              // advances the hold timer so isHeld() updates
      if (btn.isPressedNow()) {
        if (!st.pressing) { st.pressing = true; st.holdFired = false; }
        if (btn.isHeld() && !st.holdFired) { st.holdFired = true; return HOLD; }
      } else if (st.pressing) {
        st.pressing = false;
        const bool wasHold = st.holdFired;
        st.holdFired = false;
        if (!wasHold) return TAP;
      }
      return NONE;
    }
  }  // namespace NyanBoxInput
#endif  // SINGLE_BUTTON_NAV
