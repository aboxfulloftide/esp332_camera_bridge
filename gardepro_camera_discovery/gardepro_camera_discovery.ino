#include <Arduino.h>
#include <HaLow.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <NimBLEDevice.h>

#if __has_include("local_config.h")
#include "local_config.h"
#endif

#ifndef DISCOVERY_HOSTNAME
#define DISCOVERY_HOSTNAME "trail_esp32_discovery"
#endif
#ifndef CAMERA_WIFI_PASS
#define CAMERA_WIFI_PASS "1234567890"
#endif
#ifndef CAMERA_HTTP_HOST
#define CAMERA_HTTP_HOST "192.168.8.1"
#endif
#ifndef CAMERA_HTTP_PORT
#define CAMERA_HTTP_PORT 8080
#endif

static const char *CAMERA_BLE_NAME_PREFIX = "CAM";
static const char *CAMERA_ADVERTISED_SERVICE_UUID = "6e000100-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_GATT_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_NOTIFY_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_DATA_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_BLE_WAKE = "AT+WAKEPULSE=10\r\n";
static const uint32_t DEFAULT_BLE_SCAN_MS = 15000;
static const uint16_t SCAN_INTERVAL_MS = 160;
static const uint16_t SCAN_WINDOW_MS = 80;

static String serialBuffer;
static String selectedBleMac;
static String selectedWifiSsid;
static NimBLEAdvertisedDevice selectedDevice;
static bool selectedDeviceValid = false;
static uint32_t advCount = 0;
static uint32_t cameraLikeCount = 0;
static uint32_t notifyCount = 0;
static bool sawOk = false;

String jsonEscape(const String &input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '\\' || c == '"') {
      output += '\\';
      output += c;
    } else if (c == '\r') {
      continue;
    } else if (c == '\n') {
      output += "\\n";
    } else {
      output += c;
    }
  }
  return output;
}

String bytesToHex(const std::string &value) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(value.size() * 2);
  for (uint8_t c : value) {
    out += hex[(c >> 4) & 0x0F];
    out += hex[c & 0x0F];
  }
  return out;
}

String manufacturerDataJson(const NimBLEAdvertisedDevice *dev) {
  String out;
  const uint8_t count = dev->getManufacturerDataCount();
  for (uint8_t i = 0; i < count; ++i) {
    const std::string raw = dev->getManufacturerData(i);
    if (raw.size() < 2) continue;
    if (!out.isEmpty()) out += ",";
    const uint16_t cid = static_cast<uint8_t>(raw[0]) |
                         (static_cast<uint16_t>(static_cast<uint8_t>(raw[1])) << 8);
    char cidBuf[5];
    snprintf(cidBuf, sizeof(cidBuf), "%04X", cid);
    out += cidBuf;
    out += ":";
    out += bytesToHex(raw.substr(2));
  }
  return out;
}

String serviceUuidList(const NimBLEAdvertisedDevice *dev) {
  String out;
  const uint8_t count = dev->getServiceUUIDCount();
  for (uint8_t i = 0; i < count; ++i) {
    if (!out.isEmpty()) out += ",";
    String uuid = dev->getServiceUUID(i).toString().c_str();
    uuid.toLowerCase();
    out += uuid;
  }
  return out;
}

String serviceDataJson(const NimBLEAdvertisedDevice *dev) {
  String out;
  const uint8_t count = dev->getServiceDataCount();
  for (uint8_t i = 0; i < count; ++i) {
    if (!out.isEmpty()) out += ",";
    String uuid = dev->getServiceDataUUID(i).toString().c_str();
    uuid.toLowerCase();
    out += uuid;
    out += ":";
    out += bytesToHex(dev->getServiceData(i));
  }
  return out;
}

bool looksLikeCamera(const NimBLEAdvertisedDevice *dev) {
  if (dev->haveName()) {
    String name = dev->getName().c_str();
    name.toUpperCase();
    if (name.startsWith("CAM")) return true;
  }
  if (dev->isAdvertisingService(NimBLEUUID(CAMERA_ADVERTISED_SERVICE_UUID))) return true;
  return false;
}

void printBleObservation(const NimBLEAdvertisedDevice *dev, const char *prefix) {
  String mac = dev->getAddress().toString().c_str();
  mac.toLowerCase();
  Serial.print(prefix);
  Serial.print(" {\"mac\":\"");
  Serial.print(mac);
  Serial.print("\",\"rssi\":");
  Serial.print(dev->getRSSI());
  Serial.print(",\"addr_type\":");
  Serial.print(dev->getAddressType());
  Serial.print(",\"adv_type\":");
  Serial.print(dev->getAdvType());
  Serial.print(",\"connectable\":");
  Serial.print(dev->isConnectable() ? "true" : "false");
  Serial.print(",\"name\":");
  if (dev->haveName()) {
    Serial.print("\"");
    Serial.print(jsonEscape(dev->getName().c_str()));
    Serial.print("\"");
  } else {
    Serial.print("null");
  }
  if (dev->haveTXPower()) {
    Serial.print(",\"tx_power\":");
    Serial.print(dev->getTXPower());
  }
  const String mfr = manufacturerDataJson(dev);
  if (!mfr.isEmpty()) {
    Serial.print(",\"manufacturer_data\":\"");
    Serial.print(mfr);
    Serial.print("\"");
  }
  const String services = serviceUuidList(dev);
  if (!services.isEmpty()) {
    Serial.print(",\"adv_services\":\"");
    Serial.print(services);
    Serial.print("\"");
  }
  const String svcData = serviceDataJson(dev);
  if (!svcData.isEmpty()) {
    Serial.print(",\"adv_service_data\":\"");
    Serial.print(svcData);
    Serial.print("\"");
  }
  Serial.println("}");
}

class DiscoveryCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    ++advCount;
    const bool cameraLike = looksLikeCamera(dev);
    String mac = dev->getAddress().toString().c_str();
    mac.toLowerCase();
    if (cameraLike) {
      ++cameraLikeCount;
      printBleObservation(dev, "BLE_CAMERA");
    } else if (advCount <= 12) {
      printBleObservation(dev, "BLE_SAMPLE");
    }
    if (!selectedBleMac.isEmpty() && mac == selectedBleMac) {
      selectedDevice = *dev;
      selectedDeviceValid = true;
      Serial.printf("BLE_SELECTED_MATCH mac=%s rssi=%d\n", mac.c_str(), dev->getRSSI());
    } else if (selectedBleMac.isEmpty() && cameraLike && !selectedDeviceValid) {
      selectedDevice = *dev;
      selectedDeviceValid = true;
      selectedBleMac = mac;
      Serial.printf("BLE_AUTO_SELECTED mac=%s rssi=%d\n", mac.c_str(), dev->getRSSI());
    }
  }
};

static DiscoveryCallbacks discoveryCallbacks;

void notifyCallback(NimBLERemoteCharacteristic *ch, uint8_t *data, size_t len, bool isNotify) {
  (void)isNotify;
  ++notifyCount;
  String text;
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c >= 32 && c <= 126) text += c;
  }
  if (text == "OK") sawOk = true;
  Serial.printf("BLE_NOTIFY char=%s len=%u text=%s\n",
                ch->getUUID().toString().c_str(),
                static_cast<unsigned>(len),
                text.c_str());
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help");
  Serial.println("  status");
  Serial.println("  ble_scan [ms]");
  Serial.println("  select_ble <mac>");
  Serial.println("  wake [mac]");
  Serial.println("  wifi_scan");
  Serial.println("  select_wifi <ssid>");
  Serial.println("  wifi_join [ssid]");
  Serial.println("  probe");
  Serial.println("  take_picture");
  Serial.println("  standby");
}

void bleScan(uint32_t scanMs) {
  advCount = 0;
  cameraLikeCount = 0;
  selectedDeviceValid = false;
  Serial.printf("BLE_SCAN_START ms=%u selected_mac=%s\n",
                static_cast<unsigned>(scanMs),
                selectedBleMac.c_str());
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->stop();
  scan->clearResults();
  scan->setScanCallbacks(&discoveryCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(SCAN_INTERVAL_MS);
  scan->setWindow(SCAN_WINDOW_MS);
  scan->setMaxResults(0);
  scan->start(scanMs, false, true);
  delay(scanMs + 250);
  scan->stop();
  Serial.printf("BLE_SCAN_DONE adv_count=%u camera_like=%u selected_valid=%s selected_mac=%s\n",
                static_cast<unsigned>(advCount),
                static_cast<unsigned>(cameraLikeCount),
                selectedDeviceValid ? "true" : "false",
                selectedBleMac.c_str());
}

bool wakeSelected() {
  if (!selectedDeviceValid) {
    Serial.println("WAKE_NEEDS_SCAN selected device not cached; run ble_scan first");
    return false;
  }
  notifyCount = 0;
  sawOk = false;
  NimBLEClient *client = NimBLEDevice::createClient();
  client->setConnectTimeout(15000);
  client->setConnectionParams(24, 40, 0, 400, 160, 120);
  Serial.printf("BLE_CONNECT_START addr=%s name=%s\n",
                selectedDevice.getAddress().toString().c_str(),
                selectedDevice.haveName() ? selectedDevice.getName().c_str() : "");
  if (!client->connect(&selectedDevice, true, false, false)) {
    Serial.printf("BLE_CONNECT_FAIL last_error=%d\n", client->getLastError());
    NimBLEDevice::deleteClient(client);
    return false;
  }
  Serial.printf("BLE_CONNECT_OK peer=%s rssi=%d\n",
                client->getPeerAddress().toString().c_str(),
                client->getRssi());
  NimBLERemoteService *service = client->getService(CAMERA_GATT_SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("BLE_SERVICE_FAIL expected 6e400001");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }
  NimBLERemoteCharacteristic *notifyChar = service->getCharacteristic(CAMERA_NOTIFY_UUID);
  NimBLERemoteCharacteristic *dataChar = service->getCharacteristic(CAMERA_DATA_UUID);
  if (notifyChar != nullptr && notifyChar->canNotify()) {
    Serial.println("BLE_SUBSCRIBE 6e400003");
    notifyChar->subscribe(true, notifyCallback);
  }
  if (dataChar == nullptr) {
    Serial.println("BLE_DATA_CHAR_FAIL expected 6e400004");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }
  if (dataChar->canNotify() || dataChar->canIndicate()) {
    Serial.println("BLE_SUBSCRIBE 6e400004");
    dataChar->subscribe(dataChar->canNotify(), notifyCallback);
  }
  if (!(dataChar->canWrite() || dataChar->canWriteNoResponse())) {
    Serial.println("BLE_DATA_CHAR_NOT_WRITABLE");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }
  for (int i = 1; i <= 3; ++i) {
    const bool ok = dataChar->writeValue(reinterpret_cast<const uint8_t *>(CAMERA_BLE_WAKE),
                                         strlen(CAMERA_BLE_WAKE),
                                         true);
    Serial.printf("BLE_WAKE_WRITE attempt=%d ok=%s\n", i, ok ? "true" : "false");
    delay(500);
  }
  delay(2000);
  Serial.printf("BLE_WAKE_DONE notify_count=%u saw_ok=%s\n",
                static_cast<unsigned>(notifyCount),
                sawOk ? "true" : "false");
  client->disconnect();
  NimBLEDevice::deleteClient(client);
  return sawOk;
}

void wifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(200);
  Serial.println("WIFI_SCAN_START");
  const int count = WiFi.scanNetworks(false, true);
  Serial.printf("WIFI_SCAN_DONE count=%d\n", count);
  for (int i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    Serial.printf("WIFI_AP {\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"enc\":%d}\n",
                  jsonEscape(ssid).c_str(),
                  WiFi.RSSI(i),
                  WiFi.channel(i),
                  static_cast<int>(WiFi.encryptionType(i)));
    String upper = ssid;
    upper.toUpperCase();
    if (selectedWifiSsid.isEmpty() && upper.startsWith("CAM")) {
      selectedWifiSsid = ssid;
      Serial.printf("WIFI_AUTO_SELECTED ssid=%s\n", selectedWifiSsid.c_str());
    }
  }
  WiFi.scanDelete();
}

bool wifiJoin(const String &ssid) {
  if (ssid.isEmpty()) {
    Serial.println("WIFI_JOIN_SKIP no_ssid");
    return false;
  }
  selectedWifiSsid = ssid;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DISCOVERY_HOSTNAME);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(250);
  Serial.printf("WIFI_JOIN_START ssid=%s\n", selectedWifiSsid.c_str());
  WiFi.begin(selectedWifiSsid.c_str(), CAMERA_WIFI_PASS);
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  const bool ok = WiFi.status() == WL_CONNECTED;
  Serial.printf("WIFI_JOIN_DONE ok=%s status=%d ip=%s gateway=%s rssi=%d\n",
                ok ? "true" : "false",
                static_cast<int>(WiFi.status()),
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                ok ? WiFi.RSSI() : 0);
  return ok;
}

bool cameraGet(const String &path, String &body, int &statusCode) {
  body = "";
  statusCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("CAMERA_GET_SKIP wifi_down");
    return false;
  }
  WiFiClient client;
  if (!client.connect(CAMERA_HTTP_HOST, CAMERA_HTTP_PORT)) {
    Serial.printf("CAMERA_CONNECT_FAIL host=%s port=%u\n", CAMERA_HTTP_HOST, CAMERA_HTTP_PORT);
    statusCode = 502;
    return false;
  }
  client.print("GET " + path + " HTTP/1.1\r\nHost: " + String(CAMERA_HTTP_HOST) + ":" + String(CAMERA_HTTP_PORT) + "\r\nConnection: close\r\n\r\n");
  const unsigned long started = millis();
  while (client.connected() && !client.available() && millis() - started < 6000) delay(10);
  String raw;
  while (client.connected() || client.available()) {
    while (client.available()) raw += client.readString();
    if (millis() - started > 12000) break;
    delay(10);
  }
  client.stop();
  const int headerEnd = raw.indexOf("\r\n\r\n");
  String headers = headerEnd >= 0 ? raw.substring(0, headerEnd) : raw;
  body = headerEnd >= 0 ? raw.substring(headerEnd + 4) : "";
  const int firstSpace = headers.indexOf(' ');
  const int secondSpace = headers.indexOf(' ', firstSpace + 1);
  statusCode = firstSpace > 0 && secondSpace > firstSpace ? headers.substring(firstSpace + 1, secondSpace).toInt() : 0;
  Serial.printf("CAMERA_GET path=%s status=%d bytes=%u\n", path.c_str(), statusCode, static_cast<unsigned>(body.length()));
  if (!body.isEmpty()) {
    String snippet = body.substring(0, min<size_t>(body.length(), 500));
    snippet.replace("\r", "");
    snippet.replace("\n", "\\n");
    Serial.printf("CAMERA_BODY %s\n", snippet.c_str());
  }
  return statusCode >= 200 && statusCode < 300;
}

void cameraProbe() {
  const char *paths[] = {
    "/cmd/standby/reset",
    "/cmd/info/1",
    "/cmd/info/2",
    "/cmd/info/3",
    "/cmd/info/4",
    "/cmd/info/5",
    "/cmd/info/6",
    "/cmd/getParaSetting",
    "/list/detail/backward/900000/20",
    "/media/getIrStatus",
  };
  for (const char *path : paths) {
    String body;
    int status = 0;
    cameraGet(path, body, status);
    delay(250);
  }
}

void takePicture() {
  String body;
  int status = 0;
  cameraGet("/media/pic/take", body, status);
  for (int i = 1; i <= 10; ++i) {
    delay(2000);
    cameraGet("/media/pic/result", body, status);
  }
}

void printStatus() {
  Serial.printf("STATUS selected_ble=%s selected_ble_valid=%s selected_wifi=%s wifi_status=%d wifi_ip=%s wifi_rssi=%d heap=%u\n",
                selectedBleMac.c_str(),
                selectedDeviceValid ? "true" : "false",
                selectedWifiSsid.c_str(),
                static_cast<int>(WiFi.status()),
                WiFi.localIP().toString().c_str(),
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                static_cast<unsigned>(ESP.getFreeHeap()));
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) return;
  Serial.printf("serial> %s\n", cmd.c_str());
  if (cmd == "help") {
    printHelp();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd.startsWith("select_ble ")) {
    selectedBleMac = cmd.substring(11);
    selectedBleMac.trim();
    selectedBleMac.toLowerCase();
    selectedDeviceValid = false;
    Serial.printf("SELECT_BLE mac=%s\n", selectedBleMac.c_str());
  } else if (cmd.startsWith("ble_scan")) {
    uint32_t ms = DEFAULT_BLE_SCAN_MS;
    const int sp = cmd.indexOf(' ');
    if (sp > 0) ms = static_cast<uint32_t>(cmd.substring(sp + 1).toInt());
    if (ms < 3000) ms = 3000;
    if (ms > 60000) ms = 60000;
    bleScan(ms);
  } else if (cmd.startsWith("wake")) {
    const int sp = cmd.indexOf(' ');
    if (sp > 0) {
      selectedBleMac = cmd.substring(sp + 1);
      selectedBleMac.trim();
      selectedBleMac.toLowerCase();
      selectedDeviceValid = false;
      bleScan(10000);
    }
    wakeSelected();
  } else if (cmd == "wifi_scan") {
    wifiScan();
  } else if (cmd.startsWith("select_wifi ")) {
    selectedWifiSsid = cmd.substring(12);
    selectedWifiSsid.trim();
    Serial.printf("SELECT_WIFI ssid=%s\n", selectedWifiSsid.c_str());
  } else if (cmd.startsWith("wifi_join")) {
    String ssid = selectedWifiSsid;
    const int sp = cmd.indexOf(' ');
    if (sp > 0) {
      ssid = cmd.substring(sp + 1);
      ssid.trim();
    }
    wifiJoin(ssid);
  } else if (cmd == "probe") {
    cameraProbe();
  } else if (cmd == "take_picture") {
    takePicture();
  } else if (cmd == "standby") {
    String body;
    int status = 0;
    cameraGet("/cmd/standby/now", body, status);
  } else {
    Serial.println("UNKNOWN_COMMAND");
    printHelp();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("gardepro_camera_discovery boot");
  Serial.printf("host=%s heap=%u\n", DISCOVERY_HOSTNAME, static_cast<unsigned>(ESP.getFreeHeap()));
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DISCOVERY_HOSTNAME);
  WiFi.disconnect(true, true);
  NimBLEDevice::init(DISCOVERY_HOSTNAME);
  printHelp();
}

void loop() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      handleCommand(serialBuffer);
      serialBuffer = "";
    } else if (serialBuffer.length() < 255) {
      serialBuffer += c;
    }
  }
  delay(10);
}
