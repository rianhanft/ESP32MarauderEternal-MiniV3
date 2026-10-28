#pragma once

// Chromecast / DIAL "Cast" tools (inspired by Ghost ESP's cast feature).
//
// Joins a WiFi network (credentials from /cast.txt on the SD card), discovers
// cast / smart devices on the LAN via SSDP, and can launch a YouTube video on
// DIAL-capable targets (Chromecast / Google TV / Android TV) and Roku (ECP).
//
// For authorized testing on networks and devices you own or are permitted to use.
namespace DialCast {
  void runDiscovery();  // list SSDP / UPnP / cast devices on the joined network
  void runRickroll();   // discover DIAL/Roku targets and launch a YouTube video
}
