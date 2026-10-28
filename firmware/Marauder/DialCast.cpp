/*
 * Chromecast / DIAL "Cast" tools. Joins a WiFi network and discovers cast /
 * smart devices via SSDP; can launch a YouTube video on DIAL (Chromecast /
 * Google TV / Android TV) and Roku (ECP) targets. Feature selection inspired by
 * Ghost ESP's cast feature. Authorized testing only.
 */

#include "DialCast.h"

#include "configs.h"

// Needs a screen plus a 5-way control surface (Mini V3 D-pad, or V8 touch zones
// from EternalTouchButtons.cpp). WiFi-only -- no Bluetooth dependency.
#if defined(HAS_SCREEN) && ( \
      (defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
       (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)) \
      || defined(HAS_TOUCH) )

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#ifdef HAS_SD
  #include <SD.h>
#endif

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace DialCast {
namespace {

constexpr uint8_t MAX_TARGETS = 20;
constexpr uint16_t SSDP_LISTEN_MS = 2500;
constexpr char DEFAULT_VIDEO[] = "dQw4w9WgXcQ";  // Rick Astley - Never Gonna Give You Up

struct Target {
  char name[33];
  char ip[16];
  char location[96];  // UPnP device description URL (SSDP LOCATION)
  char appUrl[96];    // DIAL Application-URL (REST base for app launch)
  bool isDial;
  bool isRoku;
};

// File-scope (not stack): a Target[20] is ~5 KB; keeping it off run()'s stack
// avoids the overflow that bit Device Scout. Zeroed at the start of each run.
Target targets[MAX_TARGETS];
uint8_t targetCount = 0;

// --- buttons ------------------------------------------------------------------
void releaseButton(Switches& b) { while (b.isDown()) { b.justPressed(); delay(5); } b.justPressed(); }
void releaseControls() { releaseButton(u_btn); releaseButton(d_btn); releaseButton(l_btn);
                         releaseButton(r_btn); releaseButton(c_btn); }

// --- tiny display helpers -----------------------------------------------------
void header(const char* title) {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, 16, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_NAVY);
  display_obj.tft.drawString(title, TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void message(const char* title, const char* line1, const char* line2 = nullptr,
             uint16_t color = TFT_WHITE) {
  display_obj.tft.fillScreen(TFT_BLACK);
  header(title);
  display_obj.tft.setTextColor(color, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 26);
  display_obj.tft.print(line1);
  if (line2) { display_obj.tft.setCursor(4, 42); display_obj.tft.print(line2); }
  display_obj.tft.setTextWrap(false);
}

// --- string helpers -----------------------------------------------------------
String between(const String& s, const char* a, const char* b) {
  int i = s.indexOf(a); if (i < 0) return ""; i += strlen(a);
  int j = s.indexOf(b, i); if (j < 0) return "";
  String v = s.substring(i, j); v.trim(); return v;
}

// Case-insensitive HTTP header lookup. `lc` is a lowercased copy of `resp` (same
// length, so indices line up); the value is sliced from the original `resp`.
String headerValue(const String& resp, const String& lc, const char* name) {
  String key(name); key.toLowerCase();
  int i = lc.indexOf(key); if (i < 0) return "";
  i += key.length();
  int end = resp.indexOf('\r', i);
  if (end < 0) end = resp.indexOf('\n', i);
  if (end < 0) end = resp.length();
  String v = resp.substring(i, end); v.trim(); return v;
}

String ipFromUrl(const String& url) {
  int s = url.indexOf("://"); if (s < 0) return ""; s += 3;
  int e = s; while (e < (int)url.length() && url[e] != ':' && url[e] != '/') e++;
  return url.substring(s, e);
}

int findByIp(const char* ip) {
  for (uint8_t i = 0; i < targetCount; i++)
    if (strcmp(targets[i].ip, ip) == 0) return i;
  return -1;
}

// --- config -------------------------------------------------------------------
bool loadConfig(String& ssid, String& pass, String& video) {
  video = DEFAULT_VIDEO;
#ifdef HAS_SD
  File f = SD.open("/cast.txt", FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (!line.length() || line.startsWith("#") || line.startsWith(";")) continue;
    int eq = line.indexOf('='); if (eq < 0) continue;
    String k = line.substring(0, eq); k.trim(); k.toLowerCase();
    String v = line.substring(eq + 1); v.trim();
    if (k == "ssid") ssid = v;
    else if (k == "password" || k == "pass") pass = v;
    else if (k == "video" || k == "video_id" || k == "v") { if (v.length()) video = v; }
  }
  f.close();
  return ssid.length() > 0;
#else
  return false;
#endif
}

// --- WiFi ---------------------------------------------------------------------
bool connectWifi(const String& ssid, const String& pass) {
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t start = millis();
  uint8_t dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    char line[40];
    snprintf(line, sizeof(line), "Joining %-16.16s", ssid.c_str());
    message("CAST", line, "");
    display_obj.tft.setCursor(4, 42);
    display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
    for (uint8_t i = 0; i < (dots % 6) + 1; i++) display_obj.tft.print(".");
    dots++;
    if (c_btn.justPressed() || l_btn.justPressed()) break;  // allow bail-out
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiCleanup() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
}

// --- SSDP discovery -----------------------------------------------------------
// Send an M-SEARCH for one search target and fold responses into targets[].
void ssdpSearch(const char* st, bool markDial, bool markRoku) {
  WiFiUDP udp;
  udp.begin(0);
  IPAddress mcast(239, 255, 255, 250);
  String msearch = String("M-SEARCH * HTTP/1.1\r\n") +
      "HOST: 239.255.255.250:1900\r\n" +
      "MAN: \"ssdp:discover\"\r\n" +
      "MX: 2\r\n" +
      "ST: " + st + "\r\n\r\n";
  for (int i = 0; i < 3; i++) {  // UDP is lossy -- send a few
    udp.beginPacket(mcast, 1900);
    udp.write(reinterpret_cast<const uint8_t*>(msearch.c_str()), msearch.length());
    udp.endPacket();
    delay(80);
  }

  char buf[720];
  const uint32_t start = millis();
  while (millis() - start < SSDP_LISTEN_MS && targetCount < MAX_TARGETS) {
    int len = udp.parsePacket();
    if (len > 0) {
      int n = udp.read(buf, sizeof(buf) - 1);
      if (n < 0) n = 0;
      buf[n] = '\0';
      String resp(buf);
      String lc = resp; lc.toLowerCase();
      String loc = headerValue(resp, lc, "location:");
      String ip = loc.length() ? ipFromUrl(loc) : udp.remoteIP().toString();
      if (!ip.length()) continue;
      int idx = findByIp(ip.c_str());
      if (idx < 0 && targetCount < MAX_TARGETS) {
        idx = targetCount++;
        Target& t = targets[idx];
        memset(&t, 0, sizeof(t));
        strlcpy(t.ip, ip.c_str(), sizeof(t.ip));
        strlcpy(t.location, loc.c_str(), sizeof(t.location));
        strlcpy(t.name, ip.c_str(), sizeof(t.name));  // placeholder until XML fetched
      }
      if (idx >= 0) {
        if (markDial) targets[idx].isDial = true;
        String server = headerValue(resp, lc, "server:");
        if (markRoku || server.indexOf("Roku") >= 0 || lc.indexOf("roku") >= 0)
          targets[idx].isRoku = true;
      }
    }
    delay(5);
  }
  udp.stop();
}

// Fetch a device's UPnP description to resolve its friendly name and, for DIAL,
// the Application-URL used to launch apps.
void fetchInfo(Target& t) {
  if (!t.location[0]) return;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  if (!http.begin(t.location)) return;
  const char* hdrs[] = {"Application-URL"};
  http.collectHeaders(hdrs, 1);
  int code = http.GET();
  if (code == 200) {
    String appUrl = http.header("Application-URL");
    if (appUrl.length()) { strlcpy(t.appUrl, appUrl.c_str(), sizeof(t.appUrl)); t.isDial = true; }
    String body = http.getString();
    String name = between(body, "<friendlyName>", "</friendlyName>");
    if (!name.length()) name = between(body, "<roku:device-name>", "</roku:device-name>");
    if (name.length()) strlcpy(t.name, name.c_str(), sizeof(t.name));
    if (body.indexOf("Roku") >= 0) t.isRoku = true;
  }
  http.end();
}

// Run a full discovery sweep: SSDP for DIAL + Roku + everything, then resolve
// names. `dialOnly` keeps just castable targets (for the rickroll flow).
void discover(bool dialOnly) {
  targetCount = 0;
  memset(targets, 0, sizeof(targets));
  ssdpSearch("urn:dial-multiscreen-org:service:dial:1", true, false);
  ssdpSearch("roku:ecp", false, true);
  if (!dialOnly)
    ssdpSearch("ssdp:all", false, false);
  for (uint8_t i = 0; i < targetCount; i++) {
    if (c_btn.justPressed()) break;
    fetchInfo(targets[i]);
  }
  if (dialOnly) {
    // Compact the list to only DIAL/Roku (castable) targets.
    uint8_t w = 0;
    for (uint8_t i = 0; i < targetCount; i++)
      if (targets[i].isDial || targets[i].isRoku) targets[w++] = targets[i];
    targetCount = w;
  }
}

// --- cast ---------------------------------------------------------------------
bool castYouTube(Target& t, const String& video) {
  bool ok = false;
  if (t.isDial && t.appUrl[0]) {
    String url(t.appUrl);
    if (!url.endsWith("/")) url += "/";
    url += "YouTube";
    HTTPClient http;
    http.setConnectTimeout(3000); http.setTimeout(4000);
    if (http.begin(url)) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      int code = http.POST(String("v=") + video);
      if (code == 200 || code == 201) ok = true;
      http.end();
    }
  }
  if (!ok && t.isRoku) {  // Roku ECP: launch the YouTube channel (837) with a video
    HTTPClient http;
    http.setConnectTimeout(3000); http.setTimeout(4000);
    String url = String("http://") + t.ip + ":8060/launch/837?contentID=" + video;
    if (http.begin(url)) {
      int code = http.POST("");
      if (code == 200 || code == 204) ok = true;
      http.end();
    }
  }
  return ok;
}

// --- shared list UI -----------------------------------------------------------
void drawList(const char* title, uint8_t selected, bool castMode) {
  const int rowH = 14, startY = 29;
  const int visible = (TFT_HEIGHT - startY - 12) / rowH;
  display_obj.tft.fillScreen(TFT_BLACK);
  header(title);
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("%u device%s on %s", targetCount, targetCount == 1 ? "" : "s",
                         WiFi.localIP().toString().c_str());
  uint8_t start = selected >= visible ? selected - visible + 1 : 0;
  for (int row = 0; row < visible; row++) {
    const uint8_t idx = start + row;
    if (idx >= targetCount) break;
    const int16_t y = startY + row * rowH;
    const bool hot = idx == selected;
    const uint16_t bg = hot ? TFT_CYAN : TFT_BLACK;
    const uint16_t fg = hot ? TFT_BLACK : (targets[idx].isDial || targets[idx].isRoku ? TFT_GREEN : TFT_CYAN);
    display_obj.tft.fillRect(0, y, TFT_WIDTH, rowH - 1, bg);
    display_obj.tft.setTextColor(fg, bg);
    display_obj.tft.setViewport(2, y, TFT_WIDTH - 4, rowH - 1);
    display_obj.tft.setCursor(0, 2);
    const char tag = targets[idx].isRoku ? 'R' : (targets[idx].isDial ? 'C' : '-');
    display_obj.tft.printf("%c %s", tag, targets[idx].name);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(castMode ? F("R:cast  L:exit") : F("C:exit"));
}

}  // namespace

// Discovery: list what is castable / discoverable on the joined network.
void runDiscovery() {
  releaseControls();
  String ssid, pass, video;
  if (!loadConfig(ssid, pass, video)) {
    message("CAST", "No /cast.txt on SD.", "Add ssid=/password=", TFT_RED);
    while (!c_btn.justPressed() && !l_btn.justPressed()) delay(10);
    return;
  }
  if (!connectWifi(ssid, pass)) {
    message("CAST", "WiFi connect failed.", "Check /cast.txt creds", TFT_RED);
    wifiCleanup();
    while (!c_btn.justPressed() && !l_btn.justPressed()) delay(10);
    return;
  }
  message("CAST", "Discovering devices...", "");
  discover(false);

  uint8_t selected = 0;
  bool redraw = true;
  while (true) {
    if (c_btn.justPressed() || l_btn.justPressed()) break;
    if (u_btn.justPressed() && selected > 0) { selected--; redraw = true; }
    if (d_btn.justPressed() && selected + 1 < targetCount) { selected++; redraw = true; }
    if (redraw) { drawList("CAST SCOUT", selected, false); redraw = false; }
    delay(8);
  }
  wifiCleanup();
  releaseControls();
}

// Rickroll: discover DIAL/Roku targets, launch a YouTube video on them.
void runRickroll() {
  releaseControls();
  String ssid, pass, video;
  if (!loadConfig(ssid, pass, video)) {
    message("CAST RICKROLL", "No /cast.txt on SD.", "Add ssid/password/video", TFT_RED);
    while (!c_btn.justPressed() && !l_btn.justPressed()) delay(10);
    return;
  }
  if (!connectWifi(ssid, pass)) {
    message("CAST RICKROLL", "WiFi connect failed.", "Check /cast.txt creds", TFT_RED);
    wifiCleanup();
    while (!c_btn.justPressed() && !l_btn.justPressed()) delay(10);
    return;
  }
  message("CAST RICKROLL", "Finding cast targets...", "");
  discover(true);

  if (targetCount == 0) {
    message("CAST RICKROLL", "No DIAL/Roku targets", "found on this network", TFT_ORANGE);
    delay(50);
    while (!c_btn.justPressed() && !l_btn.justPressed()) delay(10);
    wifiCleanup();
    releaseControls();
    return;
  }

  uint8_t selected = 0;
  bool redraw = true;
  while (true) {
    if (l_btn.justPressed()) break;
    if (u_btn.justPressed() && selected > 0) { selected--; redraw = true; }
    if (d_btn.justPressed() && selected + 1 < targetCount) { selected++; redraw = true; }
    if ((r_btn.justPressed() || c_btn.justPressed()) && selected < targetCount) {
      Target& t = targets[selected];
      message("CAST RICKROLL", "Casting to:", t.name, TFT_YELLOW);
      bool ok = castYouTube(t, video);
      message("CAST RICKROLL", ok ? "Launched YouTube on:" : "Failed on:", t.name,
              ok ? TFT_GREEN : TFT_RED);
      delay(1200);
      redraw = true;
    }
    if (redraw) { drawList("CAST RICKROLL", selected, true); redraw = false; }
    delay(8);
  }
  wifiCleanup();
  releaseControls();
}

}  // namespace DialCast

#else  // board without a usable control surface

namespace DialCast {
void runDiscovery() {}
void runRickroll() {}
}

#endif
