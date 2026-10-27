/*
 * Combined Wi-Fi/BLE device discovery and persistence ranking for ESP32-C5.
 * Feature selection is inspired by nyanBOX Device Scout (MIT). This version
 * uses fixed memory, passive receive, and a 128x128 joystick UI.
 * SPDX-License-Identifier: MIT
 */

#include "WirelessDeviceScout.h"

#include "configs.h"

// Enabled on the Mini V3 (physical D-pad) or any touch board (Marauder V8),
// where u_btn..c_btn come from EternalTouchButtons.cpp as touch zones.
#if defined(HAS_BT) && defined(HAS_NIMBLE_2) && defined(HAS_SCREEN) && ( \
      (defined(HAS_BUTTONS) && (U_BTN >= 0) && (D_BTN >= 0) && \
       (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)) \
      || defined(HAS_TOUCH) )

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include "Display.h"
#include "Switches.h"

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace WirelessDeviceScout {
namespace {

constexpr uint8_t MAX_RESULTS = 48;
constexpr uint8_t VISIBLE_ROWS = 6;
constexpr uint16_t CHANNEL_DWELL_MS = 350;
constexpr uint16_t PERSISTENCE_WINDOW_MS = 5000;
constexpr int16_t ROW_START_Y = 29;
constexpr int16_t ROW_HEIGHT = 14;

enum DeviceKind : uint8_t { WIFI_DEVICE, BLE_DEVICE };

struct Device {
  char address[18];
  char name[33];
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint32_t lastWindow;
  uint16_t observations;
  uint16_t windowsSeen;
  int8_t rssi;
  uint8_t channel;
  DeviceKind kind;
};

// Allocated in PSRAM on first use (see ensureBuffers()) instead of internal
// .bss, so this feature does not permanently hold the scarce internal SRAM that
// esp_wifi_init() needs after a Bluetooth session.
Device* results = nullptr;
// Shared scratch for a sorted snapshot. Lives in PSRAM, NOT on the stack: a
// Device[MAX_RESULTS] is ~3.6 KB, and putting one in run()'s frame alongside the
// 3.6 KB that drawList()/drawDetail() each use overflowed the loop task's 8 KB
// stack and crashed Device Scout. All snapshot consumers are sequential (never
// nested), so one shared buffer is safe.
Device* snapshotBuf = nullptr;
uint8_t resultCount = 0;
volatile uint32_t resultRevision = 0;
volatile uint32_t persistenceWindow = 1;
volatile uint8_t scanChannel = 1;
portMUX_TYPE resultMux = portMUX_INITIALIZER_UNLOCKED;

// Allocate a store in PSRAM on first use (idempotent). Must be called before any
// access and outside any portENTER_CRITICAL section (it allocates).
Device* allocDeviceBuffer() {
  void* mem = heap_caps_malloc(sizeof(Device) * MAX_RESULTS, MALLOC_CAP_SPIRAM);
  if (!mem) mem = heap_caps_malloc(sizeof(Device) * MAX_RESULTS, MALLOC_CAP_8BIT);
  if (mem) memset(mem, 0, sizeof(Device) * MAX_RESULTS);
  return static_cast<Device*>(mem);
}

bool ensureBuffers() {
  if (!results) results = allocDeviceBuffer();
  if (!snapshotBuf) snapshotBuf = allocDeviceBuffer();
  return results && snapshotBuf;
}

bool buttonDown(Switches& button) {
  return button.isDown();
}

void releaseButton(Switches& button) {
  while (buttonDown(button)) {
    button.justPressed();
    delay(5);
  }
  button.justPressed();
}

void releaseControls() {
  releaseButton(u_btn);
  releaseButton(d_btn);
  releaseButton(l_btn);
  releaseButton(r_btn);
  releaseButton(c_btn);
}

void formatAddress(const uint8_t* address, char* output, size_t outputSize) {
  snprintf(output, outputSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           address[0], address[1], address[2], address[3], address[4],
           address[5]);
}

void updateDevice(DeviceKind kind, const uint8_t* address, const char* name,
                  int8_t rssi, uint8_t channel) {
  if (!address || (kind == WIFI_DEVICE && (address[0] & 0x01)))
    return;
  if (!results) return;  // run() allocates the store before scanning starts
  char formatted[18]{};
  formatAddress(address, formatted, sizeof(formatted));
  const uint32_t now = millis();

  portENTER_CRITICAL(&resultMux);
  uint8_t index = resultCount;
  for (uint8_t current = 0; current < resultCount; current++) {
    if (results[current].kind == kind &&
        strncmp(results[current].address, formatted,
                sizeof(results[current].address)) == 0) {
      index = current;
      break;
    }
  }
  if (index == resultCount && resultCount < MAX_RESULTS) {
    resultCount++;
    memset(&results[index], 0, sizeof(results[index]));
    strlcpy(results[index].address, formatted, sizeof(results[index].address));
    results[index].kind = kind;
    results[index].firstSeen = now;
  }
  if (index < MAX_RESULTS) {
    Device& device = results[index];
    if (name && name[0])
      strlcpy(device.name, name, sizeof(device.name));
    if (!device.name[0])
      strlcpy(device.name, kind == BLE_DEVICE ? "BLE device" : "WiFi device",
              sizeof(device.name));
    device.rssi = rssi;
    device.channel = channel;
    device.lastSeen = now;
    if (device.observations < UINT16_MAX)
      device.observations++;
    if (device.lastWindow != persistenceWindow) {
      device.lastWindow = persistenceWindow;
      if (device.windowsSeen < UINT16_MAX)
        device.windowsSeen++;
    }
    resultRevision++;
  }
  portEXIT_CRITICAL(&resultMux);
}

bool readSsid(const uint8_t* payload, uint16_t length, uint16_t offset,
              char* output, size_t outputSize) {
  while (offset + 2 <= length) {
    const uint8_t id = payload[offset];
    const uint8_t size = payload[offset + 1];
    offset += 2;
    if (offset + size > length)
      return false;
    if (id == 0) {
      const size_t copied = min<size_t>(size, outputSize - 1);
      for (size_t index = 0; index < copied; index++) {
        const uint8_t value = payload[offset + index];
        output[index] = (value >= 32 && value <= 126) ? value : '?';
      }
      output[copied] = '\0';
      return copied > 0;
    }
    offset += size;
  }
  return false;
}

void wifiCallback(void* buffer, wifi_promiscuous_pkt_type_t type) {
  if (!buffer || type == WIFI_PKT_MISC)
    return;
  const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
  const uint8_t* payload = packet->payload;
  const uint16_t length = packet->rx_ctrl.sig_len;
  if (length < 24)
    return;
  const uint8_t frameType = (payload[0] >> 2) & 0x03;
  const uint8_t subtype = (payload[0] >> 4) & 0x0F;
  char name[33]{};
  if (frameType == 0 && (subtype == 8 || subtype == 5) && length >= 38) {
    if (!readSsid(payload, length, 36, name, sizeof(name)))
      strlcpy(name, "WiFi AP", sizeof(name));
  }
  else if (frameType == 0 && subtype == 4) {
    if (!readSsid(payload, length, 24, name, sizeof(name)))
      strlcpy(name, "WiFi probe", sizeof(name));
  }
  updateDevice(WIFI_DEVICE, payload + 10, name, packet->rx_ctrl.rssi,
               packet->rx_ctrl.channel);
}

class ScoutCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertised) override {
    const NimBLEAddress address = advertised->getAddress();
    uint8_t bytes[6]{};
    memcpy(bytes, address.getBase()->val, sizeof(bytes));
    // NimBLE stores addresses least-significant byte first.
    uint8_t ordered[6] = {bytes[5], bytes[4], bytes[3], bytes[2], bytes[1], bytes[0]};
    const std::string advertisedName = advertised->getName();
    updateDevice(BLE_DEVICE, ordered, advertisedName.c_str(),
                 advertised->getRSSI(), 0);
  }
};

ScoutCallbacks scoutCallbacks;

bool ranksBefore(const Device& left, const Device& right) {
  if (left.windowsSeen != right.windowsSeen)
    return left.windowsSeen > right.windowsSeen;
  if (left.lastSeen != right.lastSeen)
    return left.lastSeen > right.lastSeen;
  return left.rssi > right.rssi;
}

uint8_t copyResults(Device* output) {
  if (!results || !output) return 0;
  portENTER_CRITICAL(&resultMux);
  const uint8_t count = resultCount;
  memcpy(output, results, sizeof(Device) * count);
  portEXIT_CRITICAL(&resultMux);
  for (uint8_t index = 1; index < count; index++) {
    Device value = output[index];
    int previous = index - 1;
    while (previous >= 0 && ranksBefore(value, output[previous])) {
      output[previous + 1] = output[previous];
      previous--;
    }
    output[previous + 1] = value;
  }
  return count;
}

void resetResults() {
  if (!ensureBuffers()) return;
  portENTER_CRITICAL(&resultMux);
  memset(results, 0, sizeof(Device) * MAX_RESULTS);
  resultCount = 0;
  resultRevision++;
  persistenceWindow = 1;
  portEXIT_CRITICAL(&resultMux);
}

void drawHeader() {
  display_obj.tft.fillRect(0, 0, TFT_WIDTH, 16, TFT_NAVY);
  display_obj.tft.setTextDatum(TC_DATUM);
  display_obj.tft.setTextSize(1);
  display_obj.tft.setTextColor(TFT_CYAN, TFT_NAVY);
  display_obj.tft.drawString("DEVICE SCOUT", TFT_WIDTH / 2, 4, 1);
  display_obj.tft.setTextDatum(TL_DATUM);
}

void drawList(uint8_t selected) {
  if (!snapshotBuf) return;
  Device* snapshot = snapshotBuf;   // shared PSRAM scratch, not the stack
  const uint8_t count = copyResults(snapshot);
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.setCursor(2, 18);
  display_obj.tft.printf("Found:%u Ch:%u R:detail", count, scanChannel);
  uint8_t start = selected >= VISIBLE_ROWS ? selected - VISIBLE_ROWS + 1 : 0;
  for (uint8_t row = 0; row < VISIBLE_ROWS; row++) {
    const uint8_t index = start + row;
    if (index >= count)
      break;
    const int16_t y = ROW_START_Y + row * ROW_HEIGHT;
    const bool highlighted = index == selected;
    const uint16_t background = highlighted ? TFT_CYAN : TFT_BLACK;
    const uint16_t foreground = highlighted ? TFT_BLACK : TFT_CYAN;
    display_obj.tft.fillRect(0, y, TFT_WIDTH, ROW_HEIGHT - 1, background);
    display_obj.tft.setTextColor(foreground, background);
    display_obj.tft.setViewport(2, y, TFT_WIDTH - 4, ROW_HEIGHT - 1);
    display_obj.tft.setCursor(0, 2);
    display_obj.tft.printf("%c %d %s", snapshot[index].kind == BLE_DEVICE ? 'B' : 'W',
                           snapshot[index].rssi, snapshot[index].name);
    display_obj.tft.resetViewport();
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(F("Persistence-ranked  C:exit"));
}

// Locate the locked device by ADDRESS (not list position): the list is
// re-ranked live, so a fixed row index would keep pointing at whatever device
// the persistence sort shuffled into it. Returns -1 if the locked device is not
// currently in the snapshot.
int findLocked(const Device* snapshot, uint8_t count, const char* lockAddr,
               DeviceKind lockKind) {
  if (!lockAddr || !lockAddr[0]) return -1;
  for (uint8_t i = 0; i < count; i++) {
    if (snapshot[i].kind == lockKind &&
        strncmp(snapshot[i].address, lockAddr, sizeof(snapshot[i].address)) == 0)
      return i;
  }
  return -1;
}

void drawDetail(const char* lockAddr, DeviceKind lockKind, bool locate) {
  if (!snapshotBuf) return;
  Device* snapshot = snapshotBuf;   // shared PSRAM scratch, not the stack
  const uint8_t count = copyResults(snapshot);
  const int sel = findLocked(snapshot, count, lockAddr, lockKind);
  if (sel < 0) {
    drawList(0);
    return;
  }
  const Device& item = snapshot[sel];
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  display_obj.tft.setViewport(2, 19, TFT_WIDTH - 4, 14);
  display_obj.tft.setCursor(0, 2);
  display_obj.tft.print(item.name);
  display_obj.tft.resetViewport();
  display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
  display_obj.tft.drawString(item.address, 2, 35, 1);
  if (!locate) {
    display_obj.tft.drawString(String(item.kind == BLE_DEVICE ? "BLE" : "WiFi") +
        (item.channel ? String(" channel ") + item.channel : ""), 2, 50, 1);
    display_obj.tft.drawString(String(item.rssi) + " dBm  seen " +
        item.windowsSeen + " windows", 2, 65, 1);
    display_obj.tft.drawString(String(item.observations) + " packets/ads", 2, 80, 1);
    display_obj.tft.drawString(String("Age: ") +
        ((millis() - item.firstSeen) / 1000) + "s", 2, 95, 1);
  }
  else {
    display_obj.tft.setTextDatum(TC_DATUM);
    display_obj.tft.setTextSize(2);
    display_obj.tft.setTextColor(TFT_CYAN, TFT_BLACK);
    display_obj.tft.drawString(String(item.rssi) + " dBm", TFT_WIDTH / 2, 55, 1);
    display_obj.tft.setTextSize(1);
    const int16_t strength = constrain(map(item.rssi, -100, -30, 0, TFT_WIDTH - 12),
                                       0, TFT_WIDTH - 12);
    display_obj.tft.drawRect(5, 82, TFT_WIDTH - 10, 13, TFT_DARKGREY);
    display_obj.tft.fillRect(7, 84, strength, 9,
                             item.rssi > -60 ? TFT_GREEN : TFT_ORANGE);
    display_obj.tft.setTextDatum(TL_DATUM);
  }
  display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display_obj.tft.setCursor(2, TFT_HEIGHT - 9);
  display_obj.tft.print(locate ? F("L:list R:detail C:exit") :
                                  F("L:list R:locate C:exit"));
}

void showError(const char* message) {
  display_obj.tft.fillScreen(TFT_BLACK);
  drawHeader();
  display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
  display_obj.tft.setTextWrap(true);
  display_obj.tft.setCursor(4, 28);
  display_obj.tft.print(message);
  display_obj.tft.setTextWrap(false);
}

}  // namespace

void run() {
  if (!ensureBuffers()) { showError("Out of memory for Device Scout."); return; }
  releaseControls();
  resetResults();
  if (NimBLEDevice::isInitialized())
    NimBLEDevice::deinit(true);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA;
  esp_err_t wifiError = esp_wifi_set_promiscuous_filter(&filter);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous_rx_cb(wifiCallback);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (wifiError == ESP_OK)
    wifiError = esp_wifi_set_promiscuous(true);
  scanChannel = 1;

  NimBLEDevice::init("Marauder-Scout");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scoutCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(50);
  scan->setWindow(35);
  scan->setMaxResults(0);
  const bool bleScanning = scan->start(0, false, true);
  const bool usable = wifiError == ESP_OK || bleScanning;
  if (!usable)
    showError("Wi-Fi and BLE receivers could not start.");

  uint8_t selected = 0;
  uint8_t detailPage = 0;
  char lockedAddress[18] = {0};   // device the detail/locate view is pinned to
  DeviceKind lockedKind = BLE_DEVICE;
  uint32_t drawnRevision = UINT32_MAX;
  uint32_t nextChannelHop = millis() + CHANNEL_DWELL_MS;
  uint32_t nextWindow = millis() + PERSISTENCE_WINDOW_MS;
  uint32_t nextDraw = 0;
  while (true) {
    if (c_btn.justPressed())
      break;
    const uint32_t now = millis();
    if (wifiError == ESP_OK && static_cast<int32_t>(now - nextChannelHop) >= 0) {
      scanChannel = (scanChannel % 11) + 1;
      esp_wifi_set_channel(scanChannel, WIFI_SECOND_CHAN_NONE);
      nextChannelHop = now + CHANNEL_DWELL_MS;
    }
    if (static_cast<int32_t>(now - nextWindow) >= 0) {
      persistenceWindow++;
      nextWindow = now + PERSISTENCE_WINDOW_MS;
    }

    uint8_t count;
    uint32_t revision;
    portENTER_CRITICAL(&resultMux);
    count = resultCount;
    revision = resultRevision;
    portEXIT_CRITICAL(&resultMux);
    if (!count)
      selected = 0;
    else if (selected >= count)
      selected = count - 1;
    // Only the live list reshuffles on new adverts; the detail/locate view is
    // pinned to one device, so don't repaint it on every advert (that was the
    // rapid flicker) -- it refreshes on its own timer and on button presses.
    bool redraw = (detailPage == 0) && (revision != drawnRevision);
    if (detailPage == 0 && u_btn.justPressed() && selected > 0) {
      selected--;
      redraw = true;
    }
    if (detailPage == 0 && d_btn.justPressed() && selected + 1 < count) {
      selected++;
      redraw = true;
    }
    if (r_btn.justPressed() && count) {
      if (detailPage == 0) {
        // Entering detail from the list: pin onto the highlighted device by
        // address so re-ranking can't swap which device we're looking at.
        // Use the shared PSRAM snapshot buffer -- a stack Device[MAX_RESULTS]
        // here is what overflowed the task stack and crashed the feature.
        const uint8_t snapCount = snapshotBuf ? copyResults(snapshotBuf) : 0;
        if (snapshotBuf && selected < snapCount) {
          strlcpy(lockedAddress, snapshotBuf[selected].address, sizeof(lockedAddress));
          lockedKind = snapshotBuf[selected].kind;
        }
        detailPage = 1;
      } else {
        detailPage = detailPage == 1 ? 2 : 1;  // toggle detail <-> locate
      }
      redraw = true;
    }
    if (l_btn.justPressed() && detailPage) {
      detailPage = 0;
      lockedAddress[0] = '\0';
      redraw = true;
    }
    if (static_cast<int32_t>(now - nextDraw) >= 0) {
      nextDraw = now + (detailPage ? 500 : 350);
      redraw = true;
    }
    if (usable && redraw) {
      if (detailPage)
        drawDetail(lockedAddress, lockedKind, detailPage == 2);
      else
        drawList(selected);
      drawnRevision = revision;
    }
    delay(5);
  }

  if (scan->isScanning())
    scan->stop();
  scan->setScanCallbacks(nullptr);
  scan->clearResults();
  NimBLEDevice::deinit(true);
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(20);
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace WirelessDeviceScout

#else

namespace WirelessDeviceScout {
void run() {}
}

#endif
