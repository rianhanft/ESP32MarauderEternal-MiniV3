#pragma once

#include "DroneRemoteID.h"

namespace DroneRemoteIDSpoofer {
bool selectTarget(const DroneRemoteID::CapturedDrone& target);
// autoStop true (default): stop after 60 s (the captured-replay path). false:
// run until the user presses Center (the SD-file spoof path).
void run(bool autoStop = true);
}
