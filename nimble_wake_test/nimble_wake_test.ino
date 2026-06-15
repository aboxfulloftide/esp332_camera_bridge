#include <Arduino.h>
#include <NimBLEDevice.h>

static const char *CAMERA_BLE_MAC = "a4:6d:d4:9e:47:32";
static const char *CAMERA_BLE_NAME = "CAM8Z8_NoName_G_E6";
static const char *CAMERA_BLE_NAME_PREFIX = "CAM8Z8_";
static const char *CAMERA_ADVERTISED_SERVICE_UUID = "6e000100-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_GATT_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_NOTIFY_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_DATA_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_BLE_WAKE = "AT+WAKEPULSE=10\r\n";

static const uint32_t SCAN_MS = 12000;
static const uint16_t SCAN_INTERVAL_MS = 160;
static const uint16_t SCAN_WINDOW_MS = 80;

static NimBLEAddress targetAddress;
static NimBLEAdvertisedDevice targetDeviceCopy;
static bool targetFound = false;
static uint32_t advCount = 0;
static uint32_t notifyCount = 0;
static bool sawOk = false;

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
    if (raw.size() < 2) {
      continue;
    }
    if (!out.isEmpty()) {
      out += ",";
    }
    const uint16_t cid = static_cast<uint8_t>(raw[0]) | (static_cast<uint16_t>(static_cast<uint8_t>(raw[1])) << 8);
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
    if (!out.isEmpty()) {
      out += ",";
    }
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
    const NimBLEUUID uuid = dev->getServiceDataUUID(i);
    const std::string raw = dev->getServiceData(i);
    if (!out.isEmpty()) {
      out += ",";
    }
    String uuidText = uuid.toString().c_str();
    uuidText.toLowerCase();
    out += uuidText;
    out += ":";
    out += bytesToHex(raw);
  }
  return out;
}

bool deviceLooksLikeCamera(const NimBLEAdvertisedDevice *dev) {
  String mac = dev->getAddress().toString().c_str();
  mac.toLowerCase();
  if (mac == CAMERA_BLE_MAC) {
    return true;
  }
  if (dev->haveName()) {
    const String name = dev->getName().c_str();
    if (name == CAMERA_BLE_NAME || name.startsWith(CAMERA_BLE_NAME_PREFIX)) {
      return true;
    }
  }
  if (dev->isAdvertisingService(NimBLEUUID(CAMERA_ADVERTISED_SERVICE_UUID))) {
    return true;
  }
  return false;
}

void printObservationLine(const NimBLEAdvertisedDevice *dev) {
  String mac = dev->getAddress().toString().c_str();
  mac.toLowerCase();
  const bool randomized = (dev->getAddressType() != BLE_ADDR_PUBLIC);
  Serial.print("BLE_OBS {\"mac\":\"");
  Serial.print(mac);
  Serial.print("\",\"device_type\":\"BLE\",\"interface\":\"esp32-ble\",\"signal_dbm\":");
  Serial.print(dev->getRSSI());
  Serial.print(",\"channel\":null,\"freq_mhz\":null,\"ssid\":null,\"local_name\":");
  if (dev->haveName()) {
    Serial.print("\"");
    Serial.print(dev->getName().c_str());
    Serial.print("\"");
  } else {
    Serial.print("null");
  }
  Serial.print(",\"is_randomized\":");
  Serial.print(randomized ? "true" : "false");
  Serial.print(",\"addr_type\":");
  Serial.print(dev->getAddressType());
  Serial.print(",\"adv_type\":");
  Serial.print(dev->getAdvType());
  Serial.print(",\"connectable\":");
  Serial.print(dev->isConnectable() ? "true" : "false");
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
  const String serviceData = serviceDataJson(dev);
  if (!serviceData.isEmpty()) {
    Serial.print(",\"adv_service_data\":\"");
    Serial.print(serviceData);
    Serial.print("\"");
  }
  Serial.println("}");
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    ++advCount;
    if (advCount <= 20 || deviceLooksLikeCamera(dev)) {
      printObservationLine(dev);
    }
    if (!targetFound && deviceLooksLikeCamera(dev)) {
      targetFound = true;
      targetAddress = dev->getAddress();
      targetDeviceCopy = *dev;
      Serial.printf("TARGET_FOUND mac=%s rssi=%d name=%s\n",
                    dev->getAddress().toString().c_str(),
                    dev->getRSSI(),
                    dev->haveName() ? dev->getName().c_str() : "");
      NimBLEDevice::getScan()->stop();
    }
  }
};

static ScanCallbacks scanCallbacks;

void notifyCallback(NimBLERemoteCharacteristic *ch, uint8_t *data, size_t len, bool isNotify) {
  ++notifyCount;
  String text;
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c >= 32 && c <= 126) {
      text += c;
    }
  }
  if (text == "OK") {
    sawOk = true;
  }
  Serial.printf("NOTIFY char=%s len=%u text=%s\n",
                ch->getUUID().toString().c_str(),
                static_cast<unsigned>(len),
                text.c_str());
}

bool wakeTarget() {
  if (!targetFound) {
    Serial.println("WAKE_SKIP no_target");
    return false;
  }
  NimBLEClient *client = NimBLEDevice::createClient();
  client->setConnectTimeout(15000);
  client->setConnectionParams(24, 40, 0, 400, 160, 120);
  Serial.printf("CONNECT_START addr=%s addr_type=%u connectable=%s adv_type=%u\n",
                targetDeviceCopy.getAddress().toString().c_str(),
                targetDeviceCopy.getAddressType(),
                targetDeviceCopy.isConnectable() ? "true" : "false",
                targetDeviceCopy.getAdvType());
  if (!client->connect(&targetDeviceCopy, true, false, false)) {
    Serial.printf("CONNECT_FAIL last_error=%d\n", client->getLastError());
    NimBLEDevice::deleteClient(client);
    return false;
  }
  Serial.printf("CONNECT_OK peer=%s rssi=%d\n", client->getPeerAddress().toString().c_str(), client->getRssi());

  NimBLERemoteService *service = client->getService(CAMERA_GATT_SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("SERVICE_FAIL");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  NimBLERemoteCharacteristic *notifyChar = service->getCharacteristic(CAMERA_NOTIFY_UUID);
  NimBLERemoteCharacteristic *dataChar = service->getCharacteristic(CAMERA_DATA_UUID);
  if (notifyChar != nullptr && notifyChar->canNotify()) {
    Serial.println("SUBSCRIBE_NOTIFY_003");
    notifyChar->subscribe(true, notifyCallback);
  }
  if (dataChar == nullptr) {
    Serial.println("DATA_CHAR_FAIL");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }
  if (dataChar->canNotify() || dataChar->canIndicate()) {
    Serial.println("SUBSCRIBE_NOTIFY_004");
    dataChar->subscribe(dataChar->canNotify(), notifyCallback);
  }
  if (!(dataChar->canWrite() || dataChar->canWriteNoResponse())) {
    Serial.println("DATA_CHAR_NOT_WRITABLE");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  for (int i = 1; i <= 3; ++i) {
    Serial.printf("WAKE_WRITE attempt=%d\n", i);
    const bool ok = dataChar->writeValue(reinterpret_cast<const uint8_t *>(CAMERA_BLE_WAKE),
                                         strlen(CAMERA_BLE_WAKE),
                                         true);
    Serial.printf("WAKE_WRITE_RESULT attempt=%d ok=%s\n", i, ok ? "true" : "false");
    delay(500);
  }
  delay(1500);
  Serial.printf("WAKE_DONE notify_count=%u saw_ok=%s\n", static_cast<unsigned>(notifyCount), sawOk ? "true" : "false");
  client->disconnect();
  NimBLEDevice::deleteClient(client);
  return sawOk;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("nimble_wake_test boot");
  Serial.printf("free_heap_start=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));

  NimBLEDevice::init("trail_esp32_nimble_test");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(SCAN_INTERVAL_MS);
  scan->setWindow(SCAN_WINDOW_MS);
  scan->setMaxResults(0);
  Serial.printf("SCAN_START ms=%u interval_ms=%u window_ms=%u duplicates=true\n",
                static_cast<unsigned>(SCAN_MS),
                SCAN_INTERVAL_MS,
                SCAN_WINDOW_MS);
  scan->start(SCAN_MS, false, true);
  const unsigned long scanStartedMs = millis();
  while (!targetFound && millis() - scanStartedMs < SCAN_MS + 1000) {
    delay(50);
  }
  scan->stop();
  delay(250);
  Serial.printf("SCAN_DONE adv_count=%u target_found=%s free_heap=%u\n",
                static_cast<unsigned>(advCount),
                targetFound ? "true" : "false",
                static_cast<unsigned>(ESP.getFreeHeap()));
  wakeTarget();
  Serial.printf("free_heap_end=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));
}

void loop() {
  delay(1000);
}
