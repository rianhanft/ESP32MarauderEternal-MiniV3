#pragma once

// Standalone phone control: the Marauder hosts its own Wi-Fi AP and serves a
// device-style control page. Your phone joins the AP and drives the CLI from a
// browser -- no laptop, no shared network.
//
// Single-radio caveat: a WiFi scan/attack takes the radio and drops the AP while
// it runs; pump() rebuilds the AP once the scan stops so the phone can reconnect.
namespace WebRemote {
  void start();            // bring up the AP + web server
  void stop();             // tear it all down
  void pump();             // call every loop(); runs queued web commands + upkeep
  bool active();
  const char* apSsid();
  const char* apPassword();
}
