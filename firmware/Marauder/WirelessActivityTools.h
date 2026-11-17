#pragma once

namespace WirelessActivityTools {
// fromSerial = launched over the serial CLI (web remote). When true the tool
// streams a once-per-second status line to Serial and exits on any serial input
// (so the web "Stop"/stopscan button ends it); on-device launches pass false and
// behave exactly as before (button-only exit, no serial output).
void runScanner(bool fromSerial = false);
void runJamDetector(bool fromSerial = false);
}
