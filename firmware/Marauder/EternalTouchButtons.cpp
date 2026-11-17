// Touch-to-button bridge for Marauder Eternal features on touch-only boards.
//
// Ported Eternal modules (Camera detector, Device Scout, Jam detector, BLE
// device sniffers, ...) were written for the Mini V3's physical D-pad and refer
// to the globals u_btn/d_btn/l_btn/r_btn/c_btn (defined in the sketch only when
// HAS_BUTTONS). On a touch-only board such as the Marauder V8 those globals do
// not exist, so here we define them as touch-zone Switches: a virtual D-pad
// mapped onto screen regions. Zones are per-mille of the live screen so they
// follow the active rotation. Tune the fractions below against real hardware.
//
// Layout (portrait):
//   +-----------------------------+
//   |            UP               |  y 0 .. 22%
//   +------+---------------+------+
//   | LEFT |    CENTER     | RIGHT|  y 22% .. 78%
//   +------+---------------+------+
//   |           DOWN              |  y 78% .. 100%
//   +-----------------------------+
//
#include "configs.h"

#if defined(HAS_TOUCH) && !defined(HAS_BUTTONS)

#include "Switches.h"

// x0, y0, x1, y1 in per-mille (0..1000) of screen width/height.
Switches u_btn(0,    0,   1000, 220);   // scroll up      (top band)
Switches d_btn(0,    780, 1000, 1000);  // scroll down    (bottom band)
Switches l_btn(0,    220, 250,  780);   // back / left    (left band)
Switches r_btn(750,  220, 1000, 780);   // detail / right (right band)
Switches c_btn(250,  220, 750,  780);   // select / exit  (center)
Switches b_btn(0,    0,   150,  120);   // aux (B) button (top-left corner)

#endif  // HAS_TOUCH && !HAS_BUTTONS
