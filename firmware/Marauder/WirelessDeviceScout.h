#pragma once

namespace WirelessDeviceScout {
// fromSerial = launched over the serial CLI (web remote): stream the ranked
// device list to Serial once every ~2s and exit on any serial input (web Stop).
// On-device launches pass false and behave exactly as before.
void run(bool fromSerial = false);
}
