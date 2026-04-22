#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>

#include <map>

static const char *TARGET_MAC = "a4:6d:d4:9e:47:32";

static BLEAdvertisedDevice *gTargetDevice = nullptr;
static BLEClient *gClient = nullptr;
static BLERemoteCharacteristic *gWriteChar2 = nullptr;
static BLERemoteCharacteristic *gNotifyChar3 = nullptr;
static BLERemoteCharacteristic *gDataChar4 = nullptr;
static BLERemoteCharacteristic *gHelperCharA = nullptr;
static BLERemoteCharacteristic *gHelperCharB = nullptr;
static bool gDoConnect = false;
static bool gConnected = false;
static bool gScanDone = false;
static volatile unsigned gNotifyCount = 0;
static String gCommandBuffer;
static String gLastNotifyText;
static bool gSawShortOk = false;
static bool gSawWifiJson = false;

void sendBinaryProbe(BLERemoteCharacteristic *ch,
                     const char *label,
                     uint8_t mode,
                     uint16_t typeOrCommand,
                     const uint8_t *payload,
                     size_t payloadLen,
                     bool response = false);
void writeToCharacteristic(BLERemoteCharacteristic *ch,
                           const uint8_t *data,
                           size_t len,
                           const char *label,
                           bool response = false);

struct TextProbeStep {
  const char *label;
  const char *payload;
  unsigned delayMs;
};

struct BinaryProbeStep {
  const char *label;
  uint8_t mode;
  uint16_t typeOrCommand;
  const uint8_t *payload;
  size_t payloadLen;
  unsigned delayMs;
};

enum BinaryProbeMode : uint8_t {
  PROBE_FRAME = 1,
  PROBE_PROTO_LE = 2,
  PROBE_PROTO_BE = 3,
  PROBE_FRAME_BE = 4,
  PROBE_TSS_BE = 5,
};

static String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

static size_t buildBluetoothCommandV2Json(uint8_t *out,
                                          uint16_t cmd,
                                          const String &dataText,
                                          bool includeCmdIndex,
                                          uint16_t cmdIndex) {
  String json = "{\"cmd\":";
  json += String(cmd);
  if (includeCmdIndex) {
    json += ",\"cmdIndex\":";
    json += String(cmdIndex);
  }
  json += ",\"data\":\"";
  json += jsonEscape(dataText);
  json += "\"}";

  const size_t len = json.length();
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(json[i]);
  }
  return len;
}

static size_t buildBluetoothCommandV2JsonValue(uint8_t *out,
                                               uint16_t cmd,
                                               const String &dataJson,
                                               bool includeCmdIndex,
                                               uint16_t cmdIndex) {
  String json = "{\"cmd\":";
  json += String(cmd);
  if (includeCmdIndex) {
    json += ",\"cmdIndex\":";
    json += String(cmdIndex);
  }
  json += ",\"data\":";
  json += dataJson;
  json += "}";

  const size_t len = json.length();
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(json[i]);
  }
  return len;
}

static size_t copyRawJsonBody(uint8_t *out, const String &json) {
  const size_t len = json.length();
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(json[i]);
  }
  return len;
}

static const char *CANDIDATE_UUIDS[] = {
  "0000ffb0-0000-1000-8000-00805f9b34fb",
  "0000ffb1-0000-1000-8000-00805f9b34fb",
  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
  "6E400004-B5A3-F393-E0A9-E50E24DCCA9E",
  "00002902-0000-1000-8000-00805f9b34fb"
};

static bool isStandardUuid(const String &uuid) {
  return uuid.startsWith("000018") ||
         uuid.startsWith("00002a") ||
         uuid.startsWith("000029");
}

static bool isCandidateUuid(const String &uuid) {
  String lower = uuid;
  lower.toLowerCase();
  return lower.indexOf("6e") >= 0 || lower.indexOf("ffb") >= 0;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static void logLine(const char *line) {
  Serial.println(line);
  Serial0.println(line);
}

#define LOGF(...)      \
  do {                 \
    Serial.printf(__VA_ARGS__);  \
    Serial0.printf(__VA_ARGS__); \
  } while (0)

static void notifyCallback(
  BLERemoteCharacteristic *characteristic,
  uint8_t *data,
  size_t length,
  bool isNotify
) {
  ++gNotifyCount;
  LOGF("\n[notify] char=%s len=%u kind=%s data=",
       characteristic->getUUID().toString().c_str(),
       static_cast<unsigned>(length),
       isNotify ? "notify" : "indicate");
  for (size_t i = 0; i < length; ++i) {
    LOGF("%02X", data[i]);
    if (i + 1 < length) {
      Serial.print(" ");
      Serial0.print(" ");
    }
  }
  Serial.println();
  Serial0.println();

  String text;
  bool printable = true;
  for (size_t i = 0; i < length; ++i) {
    char c = static_cast<char>(data[i]);
    if ((c < 0x20 || c > 0x7e) && c != '\r' && c != '\n' && c != '\t') {
      printable = false;
    }
    text += c;
  }
  text.trim();
  gLastNotifyText = text;

  if (printable && !text.isEmpty()) {
    LOGF("[notify-text] %s\n", text.c_str());
    if (text.indexOf("OK") >= 0 && text.length() < 6) {
      gSawShortOk = true;
      logLine("[notify-hint] short OK reply");
    }
    if (text.indexOf("\"ssid\"") >= 0 &&
        text.indexOf("\"bssid\"") >= 0 &&
        text.indexOf("\"pwd\"") >= 0) {
      gSawWifiJson = true;
      logLine("[notify-hint] WiFi JSON reply");
    }
  }
}

static String propsToString(BLERemoteCharacteristic *ch) {
  String s;
  if (ch->canRead()) s += "R";
  if (ch->canWrite()) s += "W";
  if (ch->canWriteNoResponse()) s += "N";
  if (ch->canNotify()) s += "T";
  if (ch->canIndicate()) s += "I";
  if (s.isEmpty()) s = "-";
  return s;
}

class ReconClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *client) override {
    LOGF("BLE connected to %s\n", client->getPeerAddress().toString().c_str());
  }

  void onDisconnect(BLEClient *client) override {
    LOGF("BLE disconnected from %s\n", client->getPeerAddress().toString().c_str());
    gConnected = false;
  }
};

class ReconAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String mac = advertisedDevice.getAddress().toString().c_str();
    LOGF("ADV mac=%s rssi=%d", mac.c_str(), advertisedDevice.getRSSI());
    if (advertisedDevice.haveName()) {
      LOGF(" name=%s", advertisedDevice.getName().c_str());
    }
    if (advertisedDevice.haveServiceUUID()) {
      LOGF(" service=%s", advertisedDevice.getServiceUUID().toString().c_str());
    }
    Serial.println();
    Serial0.println();

    if (mac.equalsIgnoreCase(TARGET_MAC)) {
      logLine("Target camera advertisement found");
      BLEDevice::getScan()->stop();
      gTargetDevice = new BLEAdvertisedDevice(advertisedDevice);
      gDoConnect = true;
      gScanDone = true;
    }
  }
};

void printCandidateUuids() {
  logLine("Candidate UUIDs from APK:");
  for (const char *uuid : CANDIDATE_UUIDS) {
    LOGF("  %s\n", uuid);
  }
}

void dumpService(BLERemoteService *service) {
  String serviceUuid = service->getUUID().toString().c_str();
  bool standardService = isStandardUuid(serviceUuid);
  LOGF("Service %s%s\n",
       serviceUuid.c_str(),
       isCandidateUuid(serviceUuid) ? " [candidate]" : "");

  auto *chars = service->getCharacteristics();
  for (auto it = chars->begin(); it != chars->end(); ++it) {
    BLERemoteCharacteristic *ch = it->second;
    String charUuid = ch->getUUID().toString().c_str();
    LOGF("  Characteristic %s props=%s%s\n",
         charUuid.c_str(),
         propsToString(ch).c_str(),
         isCandidateUuid(charUuid) ? " [candidate]" : "");

    if (ch->canRead()) {
      try {
        String value = ch->readValue();
        LOGF("    Read (%u bytes): ", static_cast<unsigned>(value.length()));
        for (size_t i = 0; i < value.length(); ++i) {
          LOGF("%02X", static_cast<uint8_t>(value[i]));
          if (i + 1 < value.length()) {
            Serial.print(" ");
            Serial0.print(" ");
          }
        }
        Serial.println();
        Serial0.println();
      } catch (...) {
        logLine("    Read failed");
      }
    }

    auto *descs = ch->getDescriptors();
    for (auto dit = descs->begin(); dit != descs->end(); ++dit) {
      BLERemoteDescriptor *desc = dit->second;
      LOGF("    Descriptor %s\n", desc->getUUID().toString().c_str());
    }

    if (!standardService && (ch->canNotify() || ch->canIndicate())) {
      logLine("    notify-capable");
    }

    String charLower = charUuid;
    charLower.toLowerCase();
    if (charLower == "6e400002-b5a3-f393-e0a9-e50e24dcca9e") {
      gWriteChar2 = ch;
    } else if (charLower == "6e400003-b5a3-f393-e0a9-e50e24dcca9e") {
      gNotifyChar3 = ch;
    } else if (charLower == "6e400004-b5a3-f393-e0a9-e50e24dcca9e") {
      gDataChar4 = ch;
    } else if (charLower == "984227f3-34fc-4045-a5d0-2c581f81a153") {
      gHelperCharA = ch;
    } else if (charLower == "f7bf3564-fb6d-4e53-88a4-5e37e0326063") {
      gHelperCharB = ch;
    }
  }
}

void enableCandidateNotifications() {
  if (gNotifyChar3 != nullptr && gNotifyChar3->canNotify()) {
    logLine("Registering notify on 6e400003");
    gNotifyChar3->registerForNotify(notifyCallback);
    logLine("Notify registration complete on 6e400003");
  }

  if (gDataChar4 != nullptr && (gDataChar4->canNotify() || gDataChar4->canIndicate())) {
    logLine("Registering notify on 6e400004");
    gDataChar4->registerForNotify(notifyCallback);
    logLine("Notify registration complete on 6e400004");
  }
}

void probeAppWakeToken(const char *token,
                       int attempts,
                       unsigned intervalMs,
                       bool response = true) {
  gNotifyCount = 0;
  gLastNotifyText = "";
  gSawShortOk = false;
  gSawWifiJson = false;

  if (gWriteChar2 != nullptr && (gWriteChar2->canWrite() || gWriteChar2->canWriteNoResponse())) {
    LOGF("App-style wake token \"%s\" on 6e400002 attempts=%d interval=%ums response=%s\n",
         token,
         attempts,
         intervalMs,
         response ? "yes" : "no");
    for (int attempt = 1; attempt <= attempts; ++attempt) {
      LOGF("wake attempt %d/%d -> 6e400002 token=\"%s\"\n", attempt, attempts, token);
      gWriteChar2->writeValue(reinterpret_cast<uint8_t *>(const_cast<char *>(token)),
                              strlen(token),
                              response);
      delay(intervalMs);
      if (gSawWifiJson || gSawShortOk) {
        break;
      }
    }
    delay(1500);
    LOGF("Notifications after 6e400002 probe: %u ok=%s json=%s last=\"%s\"\n",
         gNotifyCount,
         gSawShortOk ? "yes" : "no",
         gSawWifiJson ? "yes" : "no",
         gLastNotifyText.c_str());
  }
}

void probeWakeUpWifi() {
  probeAppWakeToken("H", 11, 1000, true);

  if (gNotifyCount == 0 && gDataChar4 != nullptr && (gDataChar4->canWrite() || gDataChar4->canWriteNoResponse())) {
    logLine("Fallback probing wakeUpWifi with repeated ASCII 'H' on 6e400004");
    for (int attempt = 1; attempt <= 5; ++attempt) {
      LOGF("wake attempt %d/5 -> 6e400004\n", attempt);
      gDataChar4->writeValue(String("H"), false);
      delay(1000);
      if (gSawWifiJson || gSawShortOk) {
        break;
      }
    }
    delay(1500);
    LOGF("Notifications after 6e400004 probe: %u ok=%s json=%s last=\"%s\"\n",
         gNotifyCount,
         gSawShortOk ? "yes" : "no",
         gSawWifiJson ? "yes" : "no",
         gLastNotifyText.c_str());
  }
}

void runRecoveredWakePulseSequence() {
  static const char *kWakeCommand = "AT+WAKEPULSE=10\r\n";

  if (gDataChar4 == nullptr) {
    logLine("6e400004 unavailable for recovered wake sequence");
    return;
  }

  resetProbeState();
  LOGF("Running recovered app wake sequence on 6e400004\n");
  LOGF("Payload: %s", kWakeCommand);

  for (int attempt = 1; attempt <= 3; ++attempt) {
    LOGF("Recovered wake attempt %d/3 -> 6e400004\n", attempt);
    writeToCharacteristic(gDataChar4,
                          reinterpret_cast<const uint8_t *>(kWakeCommand),
                          strlen(kWakeCommand),
                          "6e400004",
                          true);
    delay(350);
  }

  delay(2000);
  LOGF("Recovered wake done notify_count=%u ok=%s json=%s last=\"%s\"\n",
       gNotifyCount,
       gSawShortOk ? "yes" : "no",
       gSawWifiJson ? "yes" : "no",
       gLastNotifyText.c_str());
}

void resetProbeState() {
  gNotifyCount = 0;
  gLastNotifyText = "";
  gSawShortOk = false;
  gSawWifiJson = false;
}

void sendUtf8Probe(BLERemoteCharacteristic *ch, const char *label, const char *payload, bool response = false) {
  if (ch == nullptr) {
    LOGF("%s unavailable for probe %s\n", label, payload);
    return;
  }
  LOGF("Probe %s -> %s (response=%s)\n", label, payload, response ? "yes" : "no");
  ch->writeValue(reinterpret_cast<uint8_t *>(const_cast<char *>(payload)), strlen(payload), response);
}

void runTextProbeSequence(const char *name,
                          BLERemoteCharacteristic *ch,
                          const char *label,
                          const TextProbeStep *steps,
                          size_t stepCount,
                          bool response = false) {
  if (ch == nullptr) {
    LOGF("%s unavailable for sequence %s\n", label, name);
    return;
  }

  resetProbeState();
  LOGF("Running sequence %s on %s with %u steps\n",
       name,
       label,
       static_cast<unsigned>(stepCount));

  for (size_t i = 0; i < stepCount; ++i) {
    sendUtf8Probe(ch, label, steps[i].payload, response);
    delay(steps[i].delayMs);
    if (gSawWifiJson) {
      logLine("[sequence] stopping early after WiFi JSON");
      break;
    }
  }

  LOGF("Sequence %s done notify_count=%u ok=%s json=%s last=\"%s\"\n",
       name,
       gNotifyCount,
       gSawShortOk ? "yes" : "no",
       gSawWifiJson ? "yes" : "no",
       gLastNotifyText.c_str());
}

void runInferredLoginSequence() {
  static const TextProbeStep steps[] = {
    {"wake-1", "H", 1000},
    {"wake-2", "H", 1000},
    {"wake-3", "H", 1000},
    {"auth", "authChallenge", 1200},
    {"login", "loginDeviceByBleLevel0", 1200},
    {"wake-4", "H", 1000},
    {"get-apn", "reqGetApn", 1200},
    {"get-apn-info", "reqGetApnInfo", 1200},
    {"net-status", "reqNetworkStatus", 1200},
    {"version", "reqCmdVersionInfo", 1200},
  };
  runTextProbeSequence("login2", gWriteChar2, "6e400002", steps, sizeof(steps) / sizeof(steps[0]));
}

void runInferredLoginSequenceWithResponse() {
  static const TextProbeStep steps[] = {
    {"wake-1", "H", 1000},
    {"wake-2", "H", 1000},
    {"wake-3", "H", 1000},
    {"auth", "authChallenge", 1200},
    {"login", "loginDeviceByBleLevel0", 1200},
    {"wake-4", "H", 1000},
    {"get-apn", "reqGetApn", 1200},
    {"get-apn-info", "reqGetApnInfo", 1200},
    {"net-status", "reqNetworkStatus", 1200},
    {"version", "reqCmdVersionInfo", 1200},
  };
  runTextProbeSequence("login2_resp", gWriteChar2, "6e400002", steps, sizeof(steps) / sizeof(steps[0]), true);
}

void runWakeThenStatusSequence() {
  static const TextProbeStep steps[] = {
    {"wake-1", "H", 1000},
    {"wake-2", "H", 1000},
    {"wake-3", "H", 1000},
    {"wake-4", "H", 1000},
    {"get-apn", "reqGetApn", 1200},
    {"get-apn-info", "reqGetApnInfo", 1200},
    {"net-status", "reqNetworkStatus", 1200},
    {"base-reg", "reqBaseRegcode", 1200},
    {"version", "reqCmdVersionInfo", 1200},
  };
  runTextProbeSequence("wake_status", gWriteChar2, "6e400002", steps, sizeof(steps) / sizeof(steps[0]));
}

void runAltNotifyPathSequence() {
  static const TextProbeStep steps[] = {
    {"wake-1", "H", 1000},
    {"wake-2", "H", 1000},
    {"auth", "authChallenge", 1200},
    {"get-apn", "reqGetApn", 1200},
  };
  runTextProbeSequence("alt4", gDataChar4, "6e400004", steps, sizeof(steps) / sizeof(steps[0]));
}

void runHelperServiceSequence() {
  static const TextProbeStep steps[] = {
    {"wake-1", "H", 1000},
    {"wake-2", "H", 1000},
    {"auth", "authChallenge", 1200},
    {"login", "loginDeviceByBleLevel0", 1200},
    {"get-apn", "reqGetApn", 1200},
  };
  runTextProbeSequence("helper_a", gHelperCharA, "984227f3", steps, sizeof(steps) / sizeof(steps[0]));
}

void runBinaryProbeSequence(const char *name,
                            BLERemoteCharacteristic *ch,
                            const char *label,
                            const BinaryProbeStep *steps,
                            size_t stepCount) {
  if (ch == nullptr) {
    LOGF("%s unavailable for binary sequence %s\n", label, name);
    return;
  }

  resetProbeState();
  LOGF("Running binary sequence %s on %s with %u steps\n",
       name,
       label,
       static_cast<unsigned>(stepCount));

  for (size_t i = 0; i < stepCount; ++i) {
    sendBinaryProbe(ch,
                    label,
                    steps[i].mode,
                    steps[i].typeOrCommand,
                    steps[i].payload,
                    steps[i].payloadLen);
    delay(steps[i].delayMs);
    if (gSawWifiJson) {
      logLine("[binary-sequence] stopping early after WiFi JSON");
      break;
    }
  }

  LOGF("Binary sequence %s done notify_count=%u ok=%s json=%s last=\"%s\"\n",
       name,
       gNotifyCount,
       gSawShortOk ? "yes" : "no",
       gSawWifiJson ? "yes" : "no",
       gLastNotifyText.c_str());
}

void runFramedWakeAuthSequence() {
  static const uint8_t PAYLOAD_H[] = {0x48};
  static const uint8_t PAYLOAD_AUTH[] = {'a','u','t','h'};
  static const uint8_t PAYLOAD_LOGIN[] = {'l','o','g','i','n'};
  static const BinaryProbeStep steps[] = {
    {"frame-wake-1", PROBE_FRAME, 0x0001, PAYLOAD_H, sizeof(PAYLOAD_H), 1200},
    {"frame-auth", PROBE_FRAME, 0x0002, PAYLOAD_AUTH, sizeof(PAYLOAD_AUTH), 1200},
    {"frame-login", PROBE_FRAME, 0x0003, PAYLOAD_LOGIN, sizeof(PAYLOAD_LOGIN), 1200},
    {"frame-wake-2", PROBE_FRAME, 0x0001, PAYLOAD_H, sizeof(PAYLOAD_H), 1200},
  };
  runBinaryProbeSequence("frame_auth", gWriteChar2, "6e400002", steps, sizeof(steps) / sizeof(steps[0]));
}

void runProtoWakeStatusSequence(bool littleEndian) {
  static const uint8_t PAYLOAD_H[] = {0x48};
  static const uint8_t PAYLOAD_APN[] = {'a','p','n'};
  static const uint8_t PAYLOAD_NET[] = {'n','e','t'};
  static const uint8_t PAYLOAD_VER[] = {'v','e','r'};
  const BinaryProbeStep steps[] = {
    {"proto-wake", littleEndian ? PROBE_PROTO_LE : PROBE_PROTO_BE, 0x01, PAYLOAD_H, sizeof(PAYLOAD_H), 1200},
    {"proto-apn", littleEndian ? PROBE_PROTO_LE : PROBE_PROTO_BE, 0x02, PAYLOAD_APN, sizeof(PAYLOAD_APN), 1200},
    {"proto-net", littleEndian ? PROBE_PROTO_LE : PROBE_PROTO_BE, 0x03, PAYLOAD_NET, sizeof(PAYLOAD_NET), 1200},
    {"proto-ver", littleEndian ? PROBE_PROTO_LE : PROBE_PROTO_BE, 0x04, PAYLOAD_VER, sizeof(PAYLOAD_VER), 1200},
  };
  runBinaryProbeSequence(littleEndian ? "proto_le" : "proto_be",
                         gWriteChar2,
                         "6e400002",
                         steps,
                         sizeof(steps) / sizeof(steps[0]));
}

void runFrameBigEndianWakeAuthSequence() {
  static const uint8_t PAYLOAD_H[] = {0x48};
  static const uint8_t PAYLOAD_EMPTY[] = {};
  static const BinaryProbeStep steps[] = {
    {"framebe-wake", PROBE_FRAME_BE, 0x0001, PAYLOAD_H, sizeof(PAYLOAD_H), 1200},
    {"framebe-auth", PROBE_FRAME_BE, 0x0002, PAYLOAD_EMPTY, 0, 1200},
    {"framebe-login", PROBE_FRAME_BE, 0x0003, PAYLOAD_EMPTY, 0, 1200},
    {"framebe-keepalive", PROBE_FRAME_BE, 0x0004, PAYLOAD_EMPTY, 0, 1200},
  };
  runBinaryProbeSequence("frame_be_auth",
                         gWriteChar2,
                         "6e400002",
                         steps,
                         sizeof(steps) / sizeof(steps[0]));
}

void runTssHeaderSequence() {
  static const uint8_t PAYLOAD_EMPTY[] = {};
  static const uint8_t PAYLOAD_H[] = {0x48};
  static const BinaryProbeStep steps[] = {
    {"tss-wake", PROBE_TSS_BE, 0x01, PAYLOAD_H, sizeof(PAYLOAD_H), 1200},
    {"tss-auth", PROBE_TSS_BE, 0x02, PAYLOAD_EMPTY, 0, 1200},
    {"tss-login", PROBE_TSS_BE, 0x03, PAYLOAD_EMPTY, 0, 1200},
    {"tss-keepalive", PROBE_TSS_BE, 0x04, PAYLOAD_EMPTY, 0, 1200},
  };
  runBinaryProbeSequence("tss_be",
                         gWriteChar2,
                         "6e400002",
                         steps,
                         sizeof(steps) / sizeof(steps[0]));
}

void runBluetoothCommandV2JsonSequence(bool includeCmdIndex) {
  if (gWriteChar2 == nullptr) {
    logLine("6e400002 unavailable for v2 json sequence");
    return;
  }

  struct V2Step {
    const char *label;
    uint8_t msgType;
    uint16_t cmd;
    const char *dataText;
    uint16_t cmdIndex;
    unsigned delayMs;
  };

  static const V2Step steps[] = {
    {"v2-auth", 0x02, 0x0002, "", 1, 1200},
    {"v2-login", 0x03, 0x0003, "", 2, 1200},
    {"v2-keepalive", 0x04, 0x0004, "", 3, 1200},
  };

  resetProbeState();
  LOGF("Running %s V2 JSON sequence on 6e400002\n",
       includeCmdIndex ? "cmdIndex" : "no-cmdIndex");

  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
    uint8_t body[160];
    const size_t bodyLen = buildBluetoothCommandV2Json(body,
                                                       steps[i].cmd,
                                                       steps[i].dataText,
                                                       includeCmdIndex,
                                                       steps[i].cmdIndex);
    uint8_t packet[192];
    const size_t packetLen = buildTssHeaderV1(packet, steps[i].msgType, body, bodyLen);
    LOGF("V2 JSON %s msgType=0x%02X cmd=%u cmdIndex=%s body=%.*s\n",
         steps[i].label,
         static_cast<unsigned>(steps[i].msgType),
         static_cast<unsigned>(steps[i].cmd),
         includeCmdIndex ? String(steps[i].cmdIndex).c_str() : "-",
         static_cast<int>(bodyLen),
         reinterpret_cast<const char *>(body));
    writeToCharacteristic(gWriteChar2, packet, packetLen, "6e400002", true);
    delay(steps[i].delayMs);
    if (gSawWifiJson || gSawShortOk) {
      break;
    }
  }

  LOGF("V2 JSON sequence done notify_count=%u ok=%s json=%s last=\"%s\"\n",
       gNotifyCount,
       gSawShortOk ? "yes" : "no",
       gSawWifiJson ? "yes" : "no",
       gLastNotifyText.c_str());
}

void runBluetoothCommandV2ObjectSequence(bool includeCmdIndex) {
  if (gWriteChar2 == nullptr) {
    logLine("6e400002 unavailable for v2 object sequence");
    return;
  }

  struct V2Step {
    const char *label;
    uint8_t msgType;
    uint16_t cmd;
    const char *dataJson;
    uint16_t cmdIndex;
    unsigned delayMs;
  };

  static const V2Step steps[] = {
    {"v2obj-auth", 0x02, 0x0002, "{}", 1, 1200},
    {"v2obj-login", 0x03, 0x0003, "{}", 2, 1200},
    {"v2obj-keepalive", 0x04, 0x0004, "{}", 3, 1200},
  };

  resetProbeState();
  LOGF("Running %s V2 object sequence on 6e400002\n",
       includeCmdIndex ? "cmdIndex" : "no-cmdIndex");

  for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
    uint8_t body[192];
    const size_t bodyLen = buildBluetoothCommandV2JsonValue(body,
                                                            steps[i].cmd,
                                                            steps[i].dataJson,
                                                            includeCmdIndex,
                                                            steps[i].cmdIndex);
    uint8_t packet[224];
    const size_t packetLen = buildTssHeaderV1(packet, steps[i].msgType, body, bodyLen);
    LOGF("V2 OBJ %s msgType=0x%02X cmd=%u cmdIndex=%s body=%.*s\n",
         steps[i].label,
         static_cast<unsigned>(steps[i].msgType),
         static_cast<unsigned>(steps[i].cmd),
         includeCmdIndex ? String(steps[i].cmdIndex).c_str() : "-",
         static_cast<int>(bodyLen),
         reinterpret_cast<const char *>(body));
    writeToCharacteristic(gWriteChar2, packet, packetLen, "6e400002", true);
    delay(steps[i].delayMs);
    if (gSawWifiJson || gSawShortOk) {
      break;
    }
  }

  LOGF("V2 object sequence done notify_count=%u ok=%s json=%s last=\"%s\"\n",
       gNotifyCount,
       gSawShortOk ? "yes" : "no",
       gSawWifiJson ? "yes" : "no",
       gLastNotifyText.c_str());
}

void printConsoleHelp() {
  logLine("Console commands:");
  logLine("  status");
  logLine("  wake");
  logLine("  seq_realwake");
  logLine("  wakeapp");
  logLine("  wakeapp_tc");
  logLine("  wakeonce");
  logLine("  seq_login2");
  logLine("  seq_wake_status");
  logLine("  seq_alt4");
  logLine("  seq_helper_a");
  logLine("  seq_login2r");
  logLine("  seq_frame_auth");
  logLine("  seq_frame_be");
  logLine("  seq_proto_le");
  logLine("  seq_proto_be");
  logLine("  seq_tss_be");
  logLine("  seq_v2json");
  logLine("  seq_v2json_idx");
  logLine("  seq_v2obj");
  logLine("  seq_v2obj_idx");
  logLine("  send2 <text>");
  logLine("  send4 <text>");
  logLine("  senda <text>");
  logLine("  sendb <text>");
  logLine("  send2r <text>");
  logLine("  send4r <text>");
  logLine("  hex2 <hex bytes>");
  logLine("  hex4 <hex bytes>");
  logLine("  frame2 <cmdHex> <hex payload>");
  logLine("  frame4 <cmdHex> <hex payload>");
  logLine("  frame2be <cmdHex> <hex payload>");
  logLine("  frame4be <cmdHex> <hex payload>");
  logLine("  frame2ber <cmdHex> <hex payload>");
  logLine("  frame4ber <cmdHex> <hex payload>");
  logLine("  proto2le <typeHex> <hex payload>");
  logLine("  proto4le <typeHex> <hex payload>");
  logLine("  proto2be <typeHex> <hex payload>");
  logLine("  proto4be <typeHex> <hex payload>");
  logLine("  proto2ler <typeHex> <hex payload>");
  logLine("  proto4ler <typeHex> <hex payload>");
  logLine("  proto2ber <typeHex> <hex payload>");
  logLine("  proto4ber <typeHex> <hex payload>");
  logLine("  tss2 <msgTypeHex> <hex payload>");
  logLine("  tss4 <msgTypeHex> <hex payload>");
  logLine("  tss2r <msgTypeHex> <hex payload>");
  logLine("  tss4r <msgTypeHex> <hex payload>");
  logLine("  v2json2 <msgTypeHex> <cmdDecOrHex> <text data>");
  logLine("  v2json2i <msgTypeHex> <cmdDecOrHex> <cmdIndexDecOrHex> <text data>");
  logLine("  v2json4 <msgTypeHex> <cmdDecOrHex> <text data>");
  logLine("  v2json4i <msgTypeHex> <cmdDecOrHex> <cmdIndexDecOrHex> <text data>");
  logLine("  v2obj2 <msgTypeHex> <cmdDecOrHex> <raw JSON data>");
  logLine("  v2obj2i <msgTypeHex> <cmdDecOrHex> <cmdIndexDecOrHex> <raw JSON data>");
  logLine("  v2obj4 <msgTypeHex> <cmdDecOrHex> <raw JSON data>");
  logLine("  v2obj4i <msgTypeHex> <cmdDecOrHex> <cmdIndexDecOrHex> <raw JSON data>");
  logLine("  v2body2 <msgTypeHex> <raw JSON body>");
  logLine("  v2body4 <msgTypeHex> <raw JSON body>");
}

void writeToCharacteristic(BLERemoteCharacteristic *ch, const uint8_t *data, size_t len, const char *label, bool response) {
  if (ch == nullptr) {
    LOGF("%s unavailable\n", label);
    return;
  }

  LOGF("Writing %u bytes to %s (response=%s): ",
       static_cast<unsigned>(len),
       label,
       response ? "yes" : "no");
  for (size_t i = 0; i < len; ++i) {
    LOGF("%02X", data[i]);
    if (i + 1 < len) {
      Serial.print(" ");
      Serial0.print(" ");
    }
  }
  Serial.println();
  Serial0.println();

  ch->writeValue(const_cast<uint8_t *>(data), len, response);
}

bool parseHexBytes(const String &input, uint8_t *out, size_t *outLen) {
  size_t count = 0;
  int high = -1;
  for (size_t i = 0; i < input.length(); ++i) {
    int nibble = hexNibble(input[i]);
    if (nibble < 0) {
      continue;
    }
    if (high < 0) {
      high = nibble;
    } else {
      if (count >= 128) {
        return false;
      }
      out[count++] = static_cast<uint8_t>((high << 4) | nibble);
      high = -1;
    }
  }

  if (high >= 0) {
    return false;
  }

  *outLen = count;
  return count > 0;
}

bool parseHexUint16(const String &input, uint16_t *value) {
  String s = input;
  s.trim();
  if (s.startsWith("0x") || s.startsWith("0X")) {
    s = s.substring(2);
  }
  if (s.isEmpty() || s.length() > 4) {
    return false;
  }

  uint16_t v = 0;
  for (size_t i = 0; i < s.length(); ++i) {
    int nib = hexNibble(s[i]);
    if (nib < 0) {
      return false;
    }
    v = static_cast<uint16_t>((v << 4) | nib);
  }
  *value = v;
  return true;
}

size_t buildFrame(uint8_t *out, uint16_t command, const uint8_t *payload, size_t payloadLen) {
  // Working hypothesis from libapp.so strings:
  // [magicStart:2][command:2][length:2][payload:N][magicEnd:2]
  const uint16_t magicStart = 0x55AA;
  const uint16_t magicEnd = 0x0D0A;

  out[0] = static_cast<uint8_t>(magicStart & 0xFF);
  out[1] = static_cast<uint8_t>((magicStart >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>(command & 0xFF);
  out[3] = static_cast<uint8_t>((command >> 8) & 0xFF);
  out[4] = static_cast<uint8_t>(payloadLen & 0xFF);
  out[5] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  for (size_t i = 0; i < payloadLen; ++i) {
    out[6 + i] = payload[i];
  }
  out[6 + payloadLen] = static_cast<uint8_t>(magicEnd & 0xFF);
  out[7 + payloadLen] = static_cast<uint8_t>((magicEnd >> 8) & 0xFF);
  return payloadLen + 8;
}

size_t buildFrameBigEndian(uint8_t *out, uint16_t command, const uint8_t *payload, size_t payloadLen) {
  // Alternate hypothesis using the observed big-endian helpers in libapp.so.
  const uint16_t magicStart = 0x55AA;
  const uint16_t magicEnd = 0x0D0A;

  out[0] = static_cast<uint8_t>((magicStart >> 8) & 0xFF);
  out[1] = static_cast<uint8_t>(magicStart & 0xFF);
  out[2] = static_cast<uint8_t>((command >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(command & 0xFF);
  out[4] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  out[5] = static_cast<uint8_t>(payloadLen & 0xFF);
  for (size_t i = 0; i < payloadLen; ++i) {
    out[6 + i] = payload[i];
  }
  out[6 + payloadLen] = static_cast<uint8_t>((magicEnd >> 8) & 0xFF);
  out[7 + payloadLen] = static_cast<uint8_t>(magicEnd & 0xFF);
  return payloadLen + 8;
}

size_t buildProtoV1(uint8_t *out, uint8_t type, const uint8_t *payload, size_t payloadLen, bool littleEndianLength) {
  // Working hypothesis from DeviceCodeProtocolV1Obj + toByteStream + ByteOrder/Endian:
  // [type:1][length:2][payload:N]
  out[0] = type;
  if (littleEndianLength) {
    out[1] = static_cast<uint8_t>(payloadLen & 0xFF);
    out[2] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  } else {
    out[1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>(payloadLen & 0xFF);
  }
  for (size_t i = 0; i < payloadLen; ++i) {
    out[3 + i] = payload[i];
  }
  return payloadLen + 3;
}

size_t buildTssHeaderV1(uint8_t *out, uint8_t msgType, const uint8_t *payload, size_t payloadLen) {
  // Hypothesis from `TSS header.msgType:0x` plus magicStart/magicEnd strings.
  // [magicStart:2][msgType:1][length:2][payload:N][magicEnd:2]
  const uint16_t magicStart = 0x55AA;
  const uint16_t magicEnd = 0x0D0A;

  out[0] = static_cast<uint8_t>((magicStart >> 8) & 0xFF);
  out[1] = static_cast<uint8_t>(magicStart & 0xFF);
  out[2] = msgType;
  out[3] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
  out[4] = static_cast<uint8_t>(payloadLen & 0xFF);
  for (size_t i = 0; i < payloadLen; ++i) {
    out[5 + i] = payload[i];
  }
  out[5 + payloadLen] = static_cast<uint8_t>((magicEnd >> 8) & 0xFF);
  out[6 + payloadLen] = static_cast<uint8_t>(magicEnd & 0xFF);
  return payloadLen + 7;
}

void sendBinaryProbe(BLERemoteCharacteristic *ch,
                     const char *label,
                     uint8_t mode,
                     uint16_t typeOrCommand,
                     const uint8_t *payload,
                     size_t payloadLen,
                     bool response) {
  if (ch == nullptr) {
    LOGF("%s unavailable for binary probe\n", label);
    return;
  }

  uint8_t packet[160];
  size_t packetLen = 0;
  const char *modeName = "unknown";

  if (mode == PROBE_FRAME) {
    packetLen = buildFrame(packet, typeOrCommand, payload, payloadLen);
    modeName = "frame";
  } else if (mode == PROBE_PROTO_LE) {
    packetLen = buildProtoV1(packet, static_cast<uint8_t>(typeOrCommand), payload, payloadLen, true);
    modeName = "proto-le";
  } else if (mode == PROBE_PROTO_BE) {
    packetLen = buildProtoV1(packet, static_cast<uint8_t>(typeOrCommand), payload, payloadLen, false);
    modeName = "proto-be";
  } else if (mode == PROBE_FRAME_BE) {
    packetLen = buildFrameBigEndian(packet, typeOrCommand, payload, payloadLen);
    modeName = "frame-be";
  } else if (mode == PROBE_TSS_BE) {
    packetLen = buildTssHeaderV1(packet, static_cast<uint8_t>(typeOrCommand), payload, payloadLen);
    modeName = "tss-be";
  } else {
    LOGF("Unknown binary probe mode for %s\n", label);
    return;
  }

  LOGF("Binary probe %s mode=%s type=0x%X len=%u\n",
       label,
       modeName,
       static_cast<unsigned>(typeOrCommand),
       static_cast<unsigned>(packetLen));
  writeToCharacteristic(ch, packet, packetLen, label, response);
}

void handleConsoleCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  if (cmd.isEmpty()) {
    return;
  }

  LOGF("console> %s\n", cmd.c_str());

  if (cmd == "help") {
    printConsoleHelp();
    return;
  }

  if (cmd == "status") {
    LOGF("connected=%s notify_count=%u char2=%s char3=%s char4=%s helperA=%s helperB=%s ok=%s json=%s last=\"%s\"\n",
         gConnected ? "yes" : "no",
         gNotifyCount,
         gWriteChar2 ? "yes" : "no",
         gNotifyChar3 ? "yes" : "no",
         gDataChar4 ? "yes" : "no",
         gHelperCharA ? "yes" : "no",
         gHelperCharB ? "yes" : "no",
         gSawShortOk ? "yes" : "no",
         gSawWifiJson ? "yes" : "no",
         gLastNotifyText.c_str());
    return;
  }

  if (cmd == "wake") {
    probeWakeUpWifi();
    return;
  }

  if (cmd == "seq_realwake") {
    runRecoveredWakePulseSequence();
    return;
  }

  if (cmd == "wakeapp") {
    probeAppWakeToken("H", 11, 1000, true);
    return;
  }

  if (cmd == "wakeapp_tc") {
    probeAppWakeToken("TCWAKEUP", 11, 1000, true);
    return;
  }

  if (cmd == "wakeonce") {
    resetProbeState();
    if (gWriteChar2 != nullptr && (gWriteChar2->canWrite() || gWriteChar2->canWriteNoResponse())) {
      logLine("Single wakeUpWifi probe with ASCII 'H' on 6e400002");
      gWriteChar2->writeValue(String("H"), false);
      delay(1500);
      LOGF("Notifications after wakeonce: %u ok=%s json=%s last=\"%s\"\n",
           gNotifyCount,
           gSawShortOk ? "yes" : "no",
           gSawWifiJson ? "yes" : "no",
           gLastNotifyText.c_str());
    } else {
      logLine("6e400002 unavailable");
    }
    return;
  }

  if (cmd == "seq_login2") {
    runInferredLoginSequence();
    return;
  }

  if (cmd == "seq_wake_status") {
    runWakeThenStatusSequence();
    return;
  }

  if (cmd == "seq_alt4") {
    runAltNotifyPathSequence();
    return;
  }

  if (cmd == "seq_helper_a") {
    runHelperServiceSequence();
    return;
  }

  if (cmd == "seq_login2r") {
    runInferredLoginSequenceWithResponse();
    return;
  }

  if (cmd == "seq_frame_auth") {
    runFramedWakeAuthSequence();
    return;
  }

  if (cmd == "seq_frame_be") {
    runFrameBigEndianWakeAuthSequence();
    return;
  }

  if (cmd == "seq_proto_le") {
    runProtoWakeStatusSequence(true);
    return;
  }

  if (cmd == "seq_proto_be") {
    runProtoWakeStatusSequence(false);
    return;
  }

  if (cmd == "seq_tss_be") {
    runTssHeaderSequence();
    return;
  }

  if (cmd == "seq_v2json") {
    runBluetoothCommandV2JsonSequence(false);
    return;
  }

  if (cmd == "seq_v2json_idx") {
    runBluetoothCommandV2JsonSequence(true);
    return;
  }

  if (cmd == "seq_v2obj") {
    runBluetoothCommandV2ObjectSequence(false);
    return;
  }

  if (cmd == "seq_v2obj_idx") {
    runBluetoothCommandV2ObjectSequence(true);
    return;
  }

  if (cmd.startsWith("send2 ")) {
    String payload = cmd.substring(6);
    writeToCharacteristic(gWriteChar2,
                          reinterpret_cast<const uint8_t *>(payload.c_str()),
                          payload.length(),
                          "6e400002");
    return;
  }

  if (cmd.startsWith("send2r ")) {
    String payload = cmd.substring(7);
    if (gWriteChar2 == nullptr) {
      logLine("6e400002 unavailable");
      return;
    }
    sendUtf8Probe(gWriteChar2, "6e400002", payload.c_str(), true);
    return;
  }

  if (cmd.startsWith("send4 ")) {
    String payload = cmd.substring(6);
    writeToCharacteristic(gDataChar4,
                          reinterpret_cast<const uint8_t *>(payload.c_str()),
                          payload.length(),
                          "6e400004");
    return;
  }

  if (cmd.startsWith("send4r ")) {
    String payload = cmd.substring(7);
    if (gDataChar4 == nullptr) {
      logLine("6e400004 unavailable");
      return;
    }
    sendUtf8Probe(gDataChar4, "6e400004", payload.c_str(), true);
    return;
  }

  if (cmd.startsWith("senda ")) {
    String payload = cmd.substring(6);
    writeToCharacteristic(gHelperCharA,
                          reinterpret_cast<const uint8_t *>(payload.c_str()),
                          payload.length(),
                          "984227f3");
    return;
  }

  if (cmd.startsWith("sendb ")) {
    String payload = cmd.substring(6);
    writeToCharacteristic(gHelperCharB,
                          reinterpret_cast<const uint8_t *>(payload.c_str()),
                          payload.length(),
                          "f7bf3564");
    return;
  }

  if (cmd.startsWith("hex2 ") || cmd.startsWith("hex4 ")) {
    uint8_t buf[128];
    size_t len = 0;
    String payload = cmd.substring(5);
    if (!parseHexBytes(payload, buf, &len)) {
      logLine("hex parse failed");
      return;
    }
    writeToCharacteristic(cmd.startsWith("hex2 ") ? gWriteChar2 : gDataChar4,
                          buf,
                          len,
                          cmd.startsWith("hex2 ") ? "6e400002" : "6e400004");
    return;
  }

  if (cmd.startsWith("frame2 ") || cmd.startsWith("frame4 ") ||
      cmd.startsWith("frame2be ") || cmd.startsWith("frame4be ") ||
      cmd.startsWith("frame2ber ") || cmd.startsWith("frame4ber ")) {
    const bool bigEndian = cmd.startsWith("frame2be ") || cmd.startsWith("frame4be ") ||
                           cmd.startsWith("frame2ber ") || cmd.startsWith("frame4ber ");
    const bool response = cmd.startsWith("frame2ber ") || cmd.startsWith("frame4ber ");
    const int prefixLen = response ? 10 : (bigEndian ? 9 : 7);
    int space = cmd.indexOf(' ', prefixLen);
    if (space < 0) {
      logLine("usage: frame[24][be] <cmdHex> <hex payload>");
      return;
    }
    uint16_t command = 0;
    if (!parseHexUint16(cmd.substring(prefixLen, space), &command)) {
      logLine("bad command hex");
      return;
    }
    uint8_t payload[128];
    size_t payloadLen = 0;
    if (!parseHexBytes(cmd.substring(space + 1), payload, &payloadLen)) {
      logLine("hex parse failed");
      return;
    }
    uint8_t frame[160];
    size_t frameLen = bigEndian
      ? buildFrameBigEndian(frame, command, payload, payloadLen)
      : buildFrame(frame, command, payload, payloadLen);
    writeToCharacteristic((cmd.startsWith("frame2 ") || cmd.startsWith("frame2be ") || cmd.startsWith("frame2ber ")) ? gWriteChar2 : gDataChar4,
                          frame,
                          frameLen,
                          (cmd.startsWith("frame2 ") || cmd.startsWith("frame2be ") || cmd.startsWith("frame2ber ")) ? "6e400002" : "6e400004",
                          response);
    return;
  }

  if (cmd.startsWith("proto2le ") || cmd.startsWith("proto4le ") ||
      cmd.startsWith("proto2be ") || cmd.startsWith("proto4be ") ||
      cmd.startsWith("proto2ler ") || cmd.startsWith("proto4ler ") ||
      cmd.startsWith("proto2ber ") || cmd.startsWith("proto4ber ")) {
    const bool response = cmd.startsWith("proto2ler ") || cmd.startsWith("proto4ler ") ||
                          cmd.startsWith("proto2ber ") || cmd.startsWith("proto4ber ");
    const int prefixLen = response ? 10 : 9;
    int space = cmd.indexOf(' ', prefixLen);
    if (space < 0) {
      logLine("usage: proto[24][lb]e <typeHex> <hex payload>");
      return;
    }
    uint16_t typeValue = 0;
    if (!parseHexUint16(cmd.substring(prefixLen, space), &typeValue) || typeValue > 0xFF) {
      logLine("bad type hex");
      return;
    }
    uint8_t payload[128];
    size_t payloadLen = 0;
    if (!parseHexBytes(cmd.substring(space + 1), payload, &payloadLen)) {
      logLine("hex parse failed");
      return;
    }
    uint8_t packet[160];
    bool littleEndianLength = cmd.startsWith("proto2le ") || cmd.startsWith("proto4le ") ||
                              cmd.startsWith("proto2ler ") || cmd.startsWith("proto4ler ");
    size_t packetLen = buildProtoV1(packet,
                                    static_cast<uint8_t>(typeValue),
                                    payload,
                                    payloadLen,
                                    littleEndianLength);
    writeToCharacteristic((cmd.startsWith("proto2le ") || cmd.startsWith("proto2be ") || cmd.startsWith("proto2ler ") || cmd.startsWith("proto2ber ")) ? gWriteChar2 : gDataChar4,
                          packet,
                          packetLen,
                          (cmd.startsWith("proto2le ") || cmd.startsWith("proto2be ") || cmd.startsWith("proto2ler ") || cmd.startsWith("proto2ber ")) ? "6e400002" : "6e400004",
                          response);
    return;
  }

  if (cmd.startsWith("tss2 ") || cmd.startsWith("tss4 ") ||
      cmd.startsWith("tss2r ") || cmd.startsWith("tss4r ")) {
    const bool response = cmd.startsWith("tss2r ") || cmd.startsWith("tss4r ");
    const int prefixLen = response ? 6 : 5;
    int space = cmd.indexOf(' ', prefixLen);
    if (space < 0) {
      logLine("usage: tss[24] <msgTypeHex> <hex payload>");
      return;
    }
    uint16_t typeValue = 0;
    if (!parseHexUint16(cmd.substring(prefixLen, space), &typeValue) || typeValue > 0xFF) {
      logLine("bad msg type hex");
      return;
    }
    uint8_t payload[128];
    size_t payloadLen = 0;
    String payloadText = cmd.substring(space + 1);
    if (!payloadText.isEmpty() && !parseHexBytes(payloadText, payload, &payloadLen)) {
      logLine("hex parse failed");
      return;
    }
    uint8_t packet[160];
    size_t packetLen = buildTssHeaderV1(packet, static_cast<uint8_t>(typeValue), payload, payloadLen);
    writeToCharacteristic((cmd.startsWith("tss2 ") || cmd.startsWith("tss2r ")) ? gWriteChar2 : gDataChar4,
                          packet,
                          packetLen,
                          (cmd.startsWith("tss2 ") || cmd.startsWith("tss2r ")) ? "6e400002" : "6e400004",
                          response);
    return;
  }

  if (cmd.startsWith("v2json2 ") || cmd.startsWith("v2json4 ") ||
      cmd.startsWith("v2json2i ") || cmd.startsWith("v2json4i ")) {
    const bool includeCmdIndex = cmd.startsWith("v2json2i ") || cmd.startsWith("v2json4i ");
    const bool useChar2 = cmd.startsWith("v2json2 ") || cmd.startsWith("v2json2i ");
    const int prefixLen = includeCmdIndex ? 9 : 8;
    int firstSpace = cmd.indexOf(' ', prefixLen);
    if (firstSpace < 0) {
      logLine("usage: v2json[24][i] <msgType> <cmd> [cmdIndex] <text data>");
      return;
    }
    int secondSpace = cmd.indexOf(' ', firstSpace + 1);
    if (secondSpace < 0) {
      logLine("usage: v2json[24][i] <msgType> <cmd> [cmdIndex] <text data>");
      return;
    }

    uint16_t msgType = 0;
    uint16_t command = 0;
    if (!parseHexUint16(cmd.substring(prefixLen, firstSpace), &msgType) || msgType > 0xFF) {
      logLine("bad msgType");
      return;
    }
    if (!parseHexUint16(cmd.substring(firstSpace + 1, secondSpace), &command)) {
      logLine("bad cmd");
      return;
    }

    uint16_t cmdIndex = 0;
    String dataText;
    if (includeCmdIndex) {
      int thirdSpace = cmd.indexOf(' ', secondSpace + 1);
      if (thirdSpace < 0) {
        logLine("usage: v2json[24]i <msgType> <cmd> <cmdIndex> <text data>");
        return;
      }
      if (!parseHexUint16(cmd.substring(secondSpace + 1, thirdSpace), &cmdIndex)) {
        logLine("bad cmdIndex");
        return;
      }
      dataText = cmd.substring(thirdSpace + 1);
    } else {
      dataText = cmd.substring(secondSpace + 1);
    }

    uint8_t body[160];
    const size_t bodyLen = buildBluetoothCommandV2Json(body,
                                                       command,
                                                       dataText,
                                                       includeCmdIndex,
                                                       cmdIndex);
    uint8_t packet[192];
    const size_t packetLen = buildTssHeaderV1(packet, static_cast<uint8_t>(msgType), body, bodyLen);
    LOGF("Manual V2 JSON msgType=0x%02X cmd=%u cmdIndex=%s data=\"%s\"\n",
         static_cast<unsigned>(msgType),
         static_cast<unsigned>(command),
         includeCmdIndex ? String(cmdIndex).c_str() : "-",
         dataText.c_str());
    writeToCharacteristic(useChar2 ? gWriteChar2 : gDataChar4,
                          packet,
                          packetLen,
                          useChar2 ? "6e400002" : "6e400004",
                          true);
    return;
  }

  if (cmd.startsWith("v2obj2 ") || cmd.startsWith("v2obj4 ") ||
      cmd.startsWith("v2obj2i ") || cmd.startsWith("v2obj4i ")) {
    const bool includeCmdIndex = cmd.startsWith("v2obj2i ") || cmd.startsWith("v2obj4i ");
    const bool useChar2 = cmd.startsWith("v2obj2 ") || cmd.startsWith("v2obj2i ");
    const int prefixLen = includeCmdIndex ? 8 : 7;
    int firstSpace = cmd.indexOf(' ', prefixLen);
    if (firstSpace < 0) {
      logLine("usage: v2obj[24][i] <msgType> <cmd> [cmdIndex] <raw JSON data>");
      return;
    }
    int secondSpace = cmd.indexOf(' ', firstSpace + 1);
    if (secondSpace < 0) {
      logLine("usage: v2obj[24][i] <msgType> <cmd> [cmdIndex] <raw JSON data>");
      return;
    }

    uint16_t msgType = 0;
    uint16_t command = 0;
    if (!parseHexUint16(cmd.substring(prefixLen, firstSpace), &msgType) || msgType > 0xFF) {
      logLine("bad msgType");
      return;
    }
    if (!parseHexUint16(cmd.substring(firstSpace + 1, secondSpace), &command)) {
      logLine("bad cmd");
      return;
    }

    uint16_t cmdIndex = 0;
    String dataJson;
    if (includeCmdIndex) {
      int thirdSpace = cmd.indexOf(' ', secondSpace + 1);
      if (thirdSpace < 0) {
        logLine("usage: v2obj[24]i <msgType> <cmd> <cmdIndex> <raw JSON data>");
        return;
      }
      if (!parseHexUint16(cmd.substring(secondSpace + 1, thirdSpace), &cmdIndex)) {
        logLine("bad cmdIndex");
        return;
      }
      dataJson = cmd.substring(thirdSpace + 1);
    } else {
      dataJson = cmd.substring(secondSpace + 1);
    }

    uint8_t body[192];
    const size_t bodyLen = buildBluetoothCommandV2JsonValue(body,
                                                            command,
                                                            dataJson,
                                                            includeCmdIndex,
                                                            cmdIndex);
    uint8_t packet[224];
    const size_t packetLen = buildTssHeaderV1(packet, static_cast<uint8_t>(msgType), body, bodyLen);
    LOGF("Manual V2 OBJ msgType=0x%02X cmd=%u cmdIndex=%s data=%s\n",
         static_cast<unsigned>(msgType),
         static_cast<unsigned>(command),
         includeCmdIndex ? String(cmdIndex).c_str() : "-",
         dataJson.c_str());
    writeToCharacteristic(useChar2 ? gWriteChar2 : gDataChar4,
                          packet,
                          packetLen,
                          useChar2 ? "6e400002" : "6e400004",
                          true);
    return;
  }

  if (cmd.startsWith("v2body2 ") || cmd.startsWith("v2body4 ")) {
    const bool useChar2 = cmd.startsWith("v2body2 ");
    const int prefixLen = 8;
    int firstSpace = cmd.indexOf(' ', prefixLen);
    if (firstSpace < 0) {
      logLine("usage: v2body[24] <msgType> <raw JSON body>");
      return;
    }

    uint16_t msgType = 0;
    if (!parseHexUint16(cmd.substring(prefixLen, firstSpace), &msgType) || msgType > 0xFF) {
      logLine("bad msgType");
      return;
    }

    String bodyJson = cmd.substring(firstSpace + 1);
    uint8_t body[192];
    const size_t bodyLen = copyRawJsonBody(body, bodyJson);
    uint8_t packet[224];
    const size_t packetLen = buildTssHeaderV1(packet, static_cast<uint8_t>(msgType), body, bodyLen);
    LOGF("Manual V2 BODY msgType=0x%02X body=%s\n",
         static_cast<unsigned>(msgType),
         bodyJson.c_str());
    writeToCharacteristic(useChar2 ? gWriteChar2 : gDataChar4,
                          packet,
                          packetLen,
                          useChar2 ? "6e400002" : "6e400004",
                          true);
    return;
  }

  logLine("unknown command");
  printConsoleHelp();
}

void pollConsole() {
  while (Serial0.available()) {
    char c = static_cast<char>(Serial0.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleConsoleCommand(gCommandBuffer);
      gCommandBuffer = "";
      continue;
    }
    if (gCommandBuffer.length() < 255) {
      gCommandBuffer += c;
    }
  }
}

bool connectAndEnumerate() {
  if (gTargetDevice == nullptr) {
    logLine("No target device available");
    return false;
  }

  gClient = BLEDevice::createClient();
  gClient->setClientCallbacks(new ReconClientCallbacks());

  LOGF("Connecting to target %s\n", gTargetDevice->getAddress().toString().c_str());
  if (!gClient->connect(gTargetDevice)) {
    logLine("BLE connect failed");
    return false;
  }

  gClient->setMTU(517);
  gConnected = true;

  auto *services = gClient->getServices();
  LOGF("Discovered %u services\n", static_cast<unsigned>(services->size()));
  gWriteChar2 = nullptr;
  gNotifyChar3 = nullptr;
  gDataChar4 = nullptr;
  gHelperCharA = nullptr;
  gHelperCharB = nullptr;

  for (auto it = services->begin(); it != services->end(); ++it) {
    dumpService(it->second);
  }

  enableCandidateNotifications();
  logLine("Enumeration complete. Waiting for console probes or notifications...");
  return true;
}

void startScan() {
  LOGF("Starting BLE scan for target MAC %s\n", TARGET_MAC);
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ReconAdvertisedDeviceCallbacks());
  scan->setInterval(1349);
  scan->setWindow(449);
  scan->setActiveScan(true);
  scan->start(15, false);
}

void setup() {
  Serial0.begin(115200);
  Serial.begin(115200);
  delay(1500);
  logLine("GardePro BLE recon");
  printCandidateUuids();
  for (int i = 0; i < 10; ++i) {
    LOGF("Pre-BLE heartbeat %d/10 free_heap=%u\n", i + 1, static_cast<unsigned>(ESP.getFreeHeap()));
    delay(1000);
  }

  BLEDevice::init("");
  logLine("BLEDevice::init complete");
  printConsoleHelp();
  startScan();
}

void loop() {
  pollConsole();

  if (gDoConnect) {
    gDoConnect = false;
    if (!connectAndEnumerate()) {
      logLine("Retrying scan in 3 seconds");
      delay(3000);
      startScan();
    }
  }

  if (!gConnected && gScanDone && !gDoConnect) {
    logLine("Target disconnected; rescanning in 5 seconds");
    gScanDone = false;
    delay(5000);
    startScan();
  }

  delay(250);
}
