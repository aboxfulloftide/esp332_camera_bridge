/*
 * HT-HC33 SD status logger.
 *
 * Logs one status sample every 4 minutes to the onboard microSD card and
 * exposes the same status over HaLow at http://<halow-ip>:18080/status.
 */
#include <Arduino.h>
#include <FS.h>
#include <HaLow.h>
#include <Preferences.h>
#include <halow_SD.h>
#include <SPI.h>
#include <sys/time.h>
#include <time.h>
#include <WebServer.h>
#include <WiFiUdp.h>

#if __has_include("local_config.h")
#include "local_config.h"
#else
#include "local_config.example.h"
#endif

#ifndef HALOW_SSID
#define HALOW_SSID "your-halow-ssid"
#endif
#ifndef HALOW_PASS
#define HALOW_PASS "your-halow-password"
#endif

static const char *HALOW_REGION = "US";
static const IPAddress HALOW_LOCAL_IP(192, 168, 1, 30);
static const IPAddress HALOW_GATEWAY_IP(192, 168, 1, 1);
static const IPAddress HALOW_SUBNET_MASK(255, 255, 255, 0);
static const IPAddress HALOW_DNS1(192, 168, 1, 1);
static const IPAddress HALOW_DNS2(8, 8, 8, 8);

static const int BAT_ADC_PIN = 1;
static const int BAT_ADC_CTRL_PIN = 20;

static const int SD_MOSI_PIN = 11;
static const int SD_CLK_PIN = 15;
static const int SD_MISO_PIN = 16;
static const int SD_CS_PIN = 10;

static const uint32_t LOG_INTERVAL_MS = 4UL * 60UL * 1000UL;
static const uint32_t HALOW_RETRY_MS = 30UL * 1000UL;
static const char *STATUS_LOG_PATH = "/status.jsonl";
static const char *LATEST_STATUS_PATH = "/latest.json";

static Preferences runtimePrefs;
static WebServer server(18080);
static WiFiUDP ntpUdp;
static SPIClass sdSpi(HSPI);

static uint32_t bootCount = 0;
static uint32_t bootSessionId = 0;
static uint32_t logSequence = 0;
static uint32_t lastLogMs = 0;
static uint32_t lastHaLowAttemptMs = 0;
static bool sdReady = false;
static bool halowReady = false;
static String sdLastMessage = "not_started";
static bool sdFormatDone = false;
static bool timeSyncStarted = false;
static uint32_t lastNtpAttemptMs = 0;
static String timeLastMessage = "not_started";

static String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
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

static bool getUtcTimestamp(time_t *epochOut, char *isoOut, size_t isoOutSize) {
  time_t now = time(nullptr);
  if (epochOut) {
    *epochOut = now;
  }
  if (now < 1700000000) {
    if (isoOut && isoOutSize > 0) {
      isoOut[0] = '\0';
    }
    return false;
  }

  struct tm tmUtc;
  gmtime_r(&now, &tmUtc);
  strftime(isoOut, isoOutSize, "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  return true;
}

static uint32_t readBatteryAdcMilliVolts() {
  digitalWrite(BAT_ADC_CTRL_PIN, HIGH);
  delay(20);

  uint64_t total = 0;
  for (int i = 0; i < 32; ++i) {
    total += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  return total / 32;
}

static String buildStatusJson() {
  const uint32_t adcMv = readBatteryAdcMilliVolts();
  const float batteryV = (adcMv / 1000.0f) * 2.0f;
  const int halowStatus = static_cast<int>(HaLow.status());
  time_t epoch = 0;
  char isoTimestamp[32] = "";
  const bool timeSynced = getUtcTimestamp(&epoch, isoTimestamp, sizeof(isoTimestamp));

  String payload;
  payload.reserve(768);
  payload += "{";
  payload += "\"seq\":" + String(logSequence);
  payload += ",\"time_synced\":" + String(timeSynced ? "true" : "false");
  payload += ",\"time_last_message\":\"" + jsonEscape(timeLastMessage) + "\"";
  if (timeSynced) {
    payload += ",\"timestamp_utc\":\"" + String(isoTimestamp) + "\"";
    payload += ",\"epoch_seconds\":" + String(static_cast<unsigned long>(epoch));
  }
  payload += ",\"uptime_ms\":" + String(millis());
  payload += ",\"boot_count\":" + String(bootCount);
  payload += ",\"boot_session_id\":" + String(bootSessionId);
  payload += ",\"battery\":{";
  payload += "\"adc_pin\":" + String(BAT_ADC_PIN);
  payload += ",\"adc_ctrl_pin\":" + String(BAT_ADC_CTRL_PIN);
  payload += ",\"adc_mv\":" + String(adcMv);
  payload += ",\"battery_est_v\":" + String(batteryV, 3);
  payload += ",\"status_note\":\"gpio15_gpio16_shared_with_sd\"";
  payload += "}";
  payload += ",\"halow\":{";
  payload += "\"connected\":" + String(halowStatus == WL_CONNECTED ? "true" : "false");
  payload += ",\"status\":" + String(halowStatus);
  payload += ",\"ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"gateway\":\"" + HaLow.gatewayIP().toString() + "\"";
  payload += ",\"ssid\":\"" + jsonEscape(HaLow.SSID()) + "\"";
  payload += ",\"bssid\":\"" + jsonEscape(HaLow.BSSIDstr()) + "\"";
  payload += ",\"rssi\":" + String(static_cast<int>(HaLow.RSSI()));
  payload += ",\"mac\":\"" + jsonEscape(HaLow.macAddress()) + "\"";
  payload += "}";
  payload += ",\"sd\":{";
  payload += "\"ready\":" + String(sdReady ? "true" : "false");
  payload += ",\"last_message\":\"" + jsonEscape(sdLastMessage) + "\"";
  payload += ",\"format_done\":" + String(sdFormatDone ? "true" : "false");
  if (sdReady) {
    payload += ",\"interface\":\"spi\"";
    payload += ",\"mosi\":" + String(SD_MOSI_PIN);
    payload += ",\"clk\":" + String(SD_CLK_PIN);
    payload += ",\"miso\":" + String(SD_MISO_PIN);
    payload += ",\"cs\":" + String(SD_CS_PIN);
    payload += ",\"card_type\":" + String(static_cast<int>(SD.cardType()));
    payload += ",\"card_size_mb\":" + String(static_cast<unsigned long>(SD.cardSize() / (1024ULL * 1024ULL)));
    payload += ",\"used_mb\":" + String(static_cast<unsigned long>(SD.usedBytes() / (1024ULL * 1024ULL)));
    payload += ",\"total_mb\":" + String(static_cast<unsigned long>(SD.totalBytes() / (1024ULL * 1024ULL)));
  }
  payload += "}";
  payload += ",\"chip_temp_c\":" + String(temperatureRead(), 1);
  payload += "}";
  return payload;
}

static bool writeTextFile(const char *path, const String &text) {
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    return false;
  }
  const bool ok = file.print(text);
  file.close();
  return ok;
}

static bool appendStatusLine(const String &statusJson) {
  File file = SD.open(STATUS_LOG_PATH, FILE_APPEND);
  if (!file) {
    return false;
  }
  const bool ok = file.println(statusJson);
  file.flush();
  file.close();
  return ok;
}

static void markSdFormatDone() {
  if (sdFormatDone) {
    return;
  }
  runtimePrefs.begin("sdlogger", false);
  runtimePrefs.putBool("sd_format_done", true);
  runtimePrefs.end();
  sdFormatDone = true;
}

static bool verifySdWritable() {
  const char *path = "/write_test.txt";
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    sdLastMessage = "write_test_open_failed";
    return false;
  }
  const bool writeOk = file.println("ok");
  file.flush();
  file.close();
  if (!writeOk) {
    sdLastMessage = "write_test_write_failed";
    return false;
  }

  file = SD.open(path, FILE_READ);
  if (!file) {
    sdLastMessage = "write_test_read_open_failed";
    return false;
  }
  const String value = file.readStringUntil('\n');
  file.close();
  String trimmed = value;
  trimmed.trim();
  if (trimmed != "ok") {
    sdLastMessage = "write_test_readback_failed";
    return false;
  }

  SD.remove(path);
  markSdFormatDone();
  return true;
}

static bool tryMountSd(uint32_t frequency, bool formatIfMountFailed) {
  SD.end();
  sdSpi.end();
  delay(50);

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SD_MISO_PIN, INPUT_PULLUP);
  sdSpi.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  Serial.printf("[sd] try freq=%lu format_if_mount_failed=%s\n",
                static_cast<unsigned long>(frequency),
                formatIfMountFailed ? "yes" : "no");

  if (!SD.begin(SD_CS_PIN, sdSpi, frequency, "/sd", 5, formatIfMountFailed)) {
    sdLastMessage = "begin_failed_freq_" + String(frequency) +
                    "_format_" + String(formatIfMountFailed ? "yes" : "no");
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    sdLastMessage = "mounted_but_card_none_freq_" + String(frequency);
    Serial.println("[sd] no SD card attached");
    SD.end();
    return false;
  }

  sdLastMessage = "mounted_freq_" + String(frequency);
  Serial.printf("[sd] mounted card_type=%u size_mb=%llu total_mb=%llu used_mb=%llu\n",
                static_cast<unsigned>(SD.cardType()),
                SD.cardSize() / (1024ULL * 1024ULL),
                SD.totalBytes() / (1024ULL * 1024ULL),
                SD.usedBytes() / (1024ULL * 1024ULL));
  if (!verifySdWritable()) {
    Serial.printf("[sd] write test failed: %s\n", sdLastMessage.c_str());
    SD.end();
    return false;
  }
  sdLastMessage = "mounted_writable_freq_" + String(frequency);
  Serial.println("[sd] write test ok");
  return true;
}

static void logStatusSample(const char *reason) {
  String status = buildStatusJson();
  const bool appendOk = sdReady && appendStatusLine(status);
  const bool latestOk = sdReady && writeTextFile(LATEST_STATUS_PATH, status + "\n");

  Serial.printf("[status-log] reason=%s append=%s latest=%s %s\n",
                reason,
                appendOk ? "ok" : "fail",
                latestOk ? "ok" : "fail",
                status.c_str());
  ++logSequence;
}

static bool mountSdCard() {
  Serial.printf("[sd] mounting SPI mosi=%d clk=%d miso=%d cs=%d\n",
                SD_MOSI_PIN,
                SD_CLK_PIN,
                SD_MISO_PIN,
                SD_CS_PIN);
  const uint32_t frequencies[] = {4000000UL, 1000000UL, 400000UL};
  const uint8_t formatPasses = sdFormatDone ? 1 : 2;
  for (uint8_t formatPass = 0; formatPass < formatPasses; ++formatPass) {
    const bool formatIfMountFailed = formatPass == 1;
    for (uint32_t frequency : frequencies) {
      if (tryMountSd(frequency, formatIfMountFailed)) {
        return true;
      }
    }
  }

  Serial.printf("[sd] mount failed last=%s\n", sdLastMessage.c_str());
  return false;
}

static void handleStatus() {
  server.send(200, "application/json", buildStatusJson());
}

static void handleLogNow() {
  logStatusSample("http");
  server.send(200, "application/json", buildStatusJson());
}

static bool syncTimeWithNtpServer(const char *host) {
  static const uint16_t NTP_PORT = 123;
  static const uint16_t LOCAL_NTP_PORT = 2390;
  static const uint32_t NTP_UNIX_EPOCH_DELTA = 2208988800UL;
  static const size_t NTP_PACKET_SIZE = 48;
  uint8_t packet[NTP_PACKET_SIZE] = {0};

  IPAddress ntpIp;
  if (!HaLow.hostByName(host, ntpIp)) {
    timeLastMessage = String("dns_failed_") + host;
    return false;
  }

  packet[0] = 0b11100011;
  packet[1] = 0;
  packet[2] = 6;
  packet[3] = 0xEC;
  packet[12] = 49;
  packet[13] = 0x4E;
  packet[14] = 49;
  packet[15] = 52;

  ntpUdp.stop();
  if (!ntpUdp.begin(LOCAL_NTP_PORT)) {
    timeLastMessage = "udp_begin_failed";
    return false;
  }
  if (!ntpUdp.beginPacket(ntpIp, NTP_PORT)) {
    timeLastMessage = String("begin_packet_failed_") + host;
    ntpUdp.stop();
    return false;
  }
  ntpUdp.write(packet, NTP_PACKET_SIZE);
  if (!ntpUdp.endPacket()) {
    timeLastMessage = String("send_failed_") + host;
    ntpUdp.stop();
    return false;
  }

  const uint32_t start = millis();
  while (millis() - start < 5000UL) {
    const int packetLen = ntpUdp.parsePacket();
    if (packetLen >= static_cast<int>(NTP_PACKET_SIZE)) {
      ntpUdp.read(packet, NTP_PACKET_SIZE);
      ntpUdp.stop();
      const uint32_t secondsSince1900 =
          (static_cast<uint32_t>(packet[40]) << 24) |
          (static_cast<uint32_t>(packet[41]) << 16) |
          (static_cast<uint32_t>(packet[42]) << 8) |
          static_cast<uint32_t>(packet[43]);
      if (secondsSince1900 <= NTP_UNIX_EPOCH_DELTA) {
        timeLastMessage = String("invalid_ntp_time_") + host;
        return false;
      }
      const time_t epoch = secondsSince1900 - NTP_UNIX_EPOCH_DELTA;
      timeval tv = {.tv_sec = epoch, .tv_usec = 0};
      settimeofday(&tv, nullptr);
      timeLastMessage = String("synced_") + host + "_" + ntpIp.toString();
      Serial.printf("[time] NTP synced host=%s ip=%s epoch=%lu\n",
                    host,
                    ntpIp.toString().c_str(),
                    static_cast<unsigned long>(epoch));
      return true;
    }
    server.handleClient();
    delay(50);
  }

  ntpUdp.stop();
  timeLastMessage = String("timeout_") + host + "_" + ntpIp.toString();
  return false;
}

static bool syncTimeWithNtp() {
  if (HaLow.status() != WL_CONNECTED) {
    timeLastMessage = "halow_not_connected";
    return false;
  }
  lastNtpAttemptMs = millis();
  const char *servers[] = {"time.nist.gov", "pool.ntp.org", "time.google.com"};
  for (const char *serverName : servers) {
    Serial.printf("[time] NTP attempt host=%s\n", serverName);
    if (syncTimeWithNtpServer(serverName)) {
      return true;
    }
    Serial.printf("[time] NTP failed: %s\n", timeLastMessage.c_str());
  }
  return false;
}

static void connectHaLow() {
  lastHaLowAttemptMs = millis();
  Serial.printf("[halow] connecting ssid=%s\n", HALOW_SSID);

  HaLow.config(HALOW_LOCAL_IP, HALOW_GATEWAY_IP, HALOW_SUBNET_MASK, HALOW_DNS1, HALOW_DNS2);
  HaLow.begin(HALOW_SSID, HALOW_PASS);

  const uint32_t start = millis();
  while (HaLow.status() != WL_CONNECTED && millis() - start < 20000UL) {
    delay(250);
    Serial.print("#");
  }
  Serial.println();

  halowReady = HaLow.status() == WL_CONNECTED;
  if (halowReady) {
    Serial.printf("[halow] connected ip=%s gateway=%s rssi=%d mac=%s\n",
                  HaLow.localIP().toString().c_str(),
                  HaLow.gatewayIP().toString().c_str(),
                  static_cast<int>(HaLow.RSSI()),
                  HaLow.macAddress().c_str());
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/log-now", HTTP_POST, handleLogNow);
    server.begin();
    Serial.println("[http] listening on port 18080");
    if (!timeSyncStarted) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      timeSyncStarted = true;
      Serial.println("[time] NTP sync started");
      syncTimeWithNtp();
    }
  } else {
    Serial.printf("[halow] connect failed status=%d\n", static_cast<int>(HaLow.status()));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BAT_ADC_CTRL_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  runtimePrefs.begin("sdlogger", false);
  bootCount = runtimePrefs.getUInt("boot_count", 0) + 1;
  runtimePrefs.putUInt("boot_count", bootCount);
  bootSessionId = runtimePrefs.getUInt("boot_session", 0) + 1;
  runtimePrefs.putUInt("boot_session", bootSessionId);
  sdFormatDone = runtimePrefs.getBool("sd_format_done", false);
  runtimePrefs.end();

  Serial.printf("[boot] sd_status_logger boot_count=%u boot_session_id=%u uptime_ms=%lu\n",
                bootCount,
                bootSessionId,
                millis());

  sdReady = mountSdCard();

  Serial.printf("[battery] %s\n", buildStatusJson().c_str());

  HaLow.init(HALOW_REGION);
  connectHaLow();

  logStatusSample("boot");
  lastLogMs = millis();
}

void loop() {
  if (halowReady) {
    server.handleClient();
  }

  if (HaLow.status() != WL_CONNECTED) {
    halowReady = false;
    if (millis() - lastHaLowAttemptMs >= HALOW_RETRY_MS) {
      connectHaLow();
    }
  }

  if (!sdReady) {
    static uint32_t lastSdRetryMs = 0;
    if (millis() - lastSdRetryMs >= HALOW_RETRY_MS) {
      lastSdRetryMs = millis();
      sdReady = mountSdCard();
    }
  }

  if (HaLow.status() == WL_CONNECTED) {
    time_t epoch = 0;
    char isoTimestamp[32] = "";
    if (!getUtcTimestamp(&epoch, isoTimestamp, sizeof(isoTimestamp)) &&
        millis() - lastNtpAttemptMs >= HALOW_RETRY_MS) {
      syncTimeWithNtp();
    }
  }

  if (millis() - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = millis();
    logStatusSample("interval");
  }

  delay(10);
}
