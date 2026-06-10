#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <HaLow.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <stdint.h>

extern "C" {
#include <lwip/sockets.h>
}

#if __has_include("local_config.h")
#include "local_config.h"
#endif

/*
 * GardePro dual-radio bridge skeleton
 *
 * Radio split:
 * - Standard WiFi STA -> trail camera hotspot
 * - HaLow STA         -> upstream long-range network / server
 *
 * Confirmed camera-side facts:
 * - Camera IP: 192.168.8.1
 * - Camera HTTP port: 8080
 * - Confirmed HTTP routes include:
 *     /cmd/standby/reset
 *     /cmd/getParaSetting
 *     /cmd/info/1..6
 *     /list/detail/backward/900000/60
 *     /media/getIrStatus
 * - Live video is UDP sourced from camera ports 49152 / 49153
 *
 * This sketch intentionally does not implement BLE activation or the dynamic
 * UDP destination-port negotiation the Android app appears to perform.
 * It is the transport layer you can use once the camera is already awake and
 * reachable over 2.4 GHz.
 */

// 2.4 GHz camera hotspot
static const char *CAMERA_WIFI_SSID = "CAM8Z8_A46DD49E4732";
static const char *CAMERA_WIFI_PASS = "1234567890";
static const IPAddress CAMERA_IP(192, 168, 8, 1);
static const uint16_t CAMERA_HTTP_PORT = 8080;
static const uint16_t CAMERA_RTSP_PORT = 554;

// HaLow upstream network
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

// HTTP proxy served locally on the board.
static const uint16_t BRIDGE_HTTP_PORT = 18080;

// UDP forwarding config.
// These are the local receive ports on the bridge side.
// The actual Android app uses dynamic ports, so tune these once you know the
// live-view negotiation behavior for your deployment.
static const uint16_t LOCAL_MEDIA_PORT_PRIMARY = 25748;
static const uint16_t LOCAL_MEDIA_PORT_SECONDARY = 25749;

// Upstream relay target on the HaLow network.
static const IPAddress UPSTREAM_MEDIA_IP(192, 168, 1, 39);
static const uint16_t UPSTREAM_TUNNEL_PORT = 6000;
static const bool RUN_LOCAL_SERIAL_TEST = true;
static const char *CAMERA_BLE_MAC = "a4:6d:d4:9e:47:32";
static const char *CAMERA_BLE_WAKE = "AT+WAKEPULSE=10\r\n";
static const char *CAMERA_BLE_NAME = "CAM8Z8_NoName_G_E6";
static const char *CAMERA_BLE_NAME_PREFIX = "CAM8Z8_";
static const char *CAMERA_BLE_SERVICE_UUID = "6e000100-b5a3-f393-e0a9-e50e24dcca9e";
static const size_t BLE_RECENT_DEVICE_SLOTS = 16;

#ifndef ONBOARD_CAPTURE_INTERVAL_MS
#define ONBOARD_CAPTURE_INTERVAL_MS 60000UL
#endif
#ifndef ONBOARD_CAPTURE_ENABLED
#define ONBOARD_CAPTURE_ENABLED 1
#endif

static const int BAT_ADC_PIN = 1;
static const int BAT_ADC_CTRL_PIN = 20;
static const int BAT_CHRG_PIN = 15;
static const int BAT_DONE_PIN = 16;
static const unsigned long WIFI_SCAN_CACHE_MS = 60000UL;

WebServer server(BRIDGE_HTTP_PORT);
bool httpServerStarted = false;

volatile bool halowConnected = false;
volatile bool wifiConnected = false;
volatile unsigned bleNotifyCount = 0;
bool bleWakeSawOk = false;
String bleLastNotifyText;
String bleStage = "idle";

unsigned long lastStatusLogMs = 0;
unsigned long lastRescanMs = 0;
uint32_t mediaPrimaryPackets = 0;
uint32_t mediaSecondaryPackets = 0;
uint32_t mediaPrimaryBytes = 0;
uint32_t mediaSecondaryBytes = 0;
bool bleWakeAttempted = false;
bool bleWakeConfirmed = false;
bool cameraWifiEverConnected = false;
bool standbyRequested = false;
String bleScanMode = "idle";
String bleLastSeenMac;
String bleLastSeenName;
int bleLastSeenRssi = -127;
int bleBestSeenRssi = -127;
String bleBestSeenMac;
String bleBestSeenName;
uint32_t bleScanResultCount = 0;
uint32_t bleTargetSeenCount = 0;
uint32_t bleScanAttemptCounter = 0;
String bleRecentMacs[BLE_RECENT_DEVICE_SLOTS];
String bleRecentNames[BLE_RECENT_DEVICE_SLOTS];
String bleRecentServices[BLE_RECENT_DEVICE_SLOTS];
int bleRecentRssis[BLE_RECENT_DEVICE_SLOTS] = {
  -127, -127, -127, -127,
  -127, -127, -127, -127,
  -127, -127, -127, -127,
  -127, -127, -127, -127
};
String serialCommandBuffer;
bool udpInspectorsStarted = false;
bool streamSessionActive = false;
uint32_t halowEventCount = 0;
int halowLastEventId = -1;
unsigned long halowLastEventMs = 0;
bool tunnelEverConnected = false;
uint32_t tunnelPacketsSent = 0;
uint32_t tunnelBytesSent = 0;
uint32_t tunnelSendFailures = 0;
unsigned long tunnelLastConnectMs = 0;
bool halowInitialized = false;
static WiFiClient cameraRtspClient;
bool rtspSessionOpen = false;
String rtspPlayUrl;
String rtspSessionHeader;
unsigned long lastRtspKeepaliveMs = 0;
unsigned long lastBleKeepaliveMs = 0;
unsigned long lastHttpKeepaliveMs = 0;
unsigned long lastTunnelReconnectMs = 0;
unsigned long lastPrimaryPacketMs = 0;
unsigned long lastSecondaryPacketMs = 0;
unsigned long lastStreamRecoveryMs = 0;
unsigned long lastIdleRecoveryMs = 0;
unsigned long streamSessionStartedMs = 0;
unsigned long lastStreamStopMs = 0;
static const unsigned long RTSP_KEEPALIVE_INTERVAL_MS = 5000;
static const unsigned long BLE_KEEPALIVE_INTERVAL_MS = 20000;
static const unsigned long HTTP_KEEPALIVE_INTERVAL_MS = 15000;
static const unsigned long TUNNEL_RECONNECT_INTERVAL_MS = 2000;
static const unsigned long STREAM_STALL_TIMEOUT_MS = 4000;
static const unsigned long STREAM_RECOVERY_COOLDOWN_MS = 10000;
static const unsigned long IDLE_WIFI_RECOVERY_COOLDOWN_MS = 30000;
static const uint8_t HTTP_KEEPALIVE_FAILURE_THRESHOLD = 2;
static const BaseType_t CONTROL_WORKER_CORE = 0;
static const uint8_t BLE_SCAN_ATTEMPTS = 3;
static const uint16_t BLE_SCAN_WINDOW_SEC = 5;
static const unsigned long BLE_SCAN_RETRY_DELAY_MS = 1000;
static const uint8_t BLE_PASSIVE_SCAN_ATTEMPTS = 2;
static const uint16_t BLE_PASSIVE_SCAN_WINDOW_SEC = 8;
static const uint16_t BLE_SCAN_INTERVAL_MS = 80;
static const uint16_t BLE_SCAN_WINDOW_MS = 80;
static const uint8_t BLE_WAKE_PULSE_ATTEMPTS = 3;
static const unsigned long BLE_WAKE_PULSE_DELAY_MS = 350;
static const unsigned long BRINGUP_HOTSPOT_WAIT_MS = 20000;
static const unsigned long BRINGUP_HOTSPOT_POLL_MS = 3000;
static const uint32_t UDP_LOG_FIRST_PACKETS = 8;
static const uint32_t UDP_LOG_EVERY_N_PACKETS = 120;
uint32_t streamRecoveryAttempts = 0;
uint32_t idleRecoveryAttempts = 0;
uint32_t httpKeepaliveFailures = 0;
SemaphoreHandle_t tunnelWriteMutex = nullptr;
TaskHandle_t controlWorkerTaskHandle = nullptr;

SemaphoreHandle_t onboardFrameMutex = nullptr;
TaskHandle_t onboardCaptureTaskHandle = nullptr;
bool onboardCameraReady = false;
bool onboardCaptureEnabled = ONBOARD_CAPTURE_ENABLED != 0;
unsigned long onboardCaptureIntervalMs = ONBOARD_CAPTURE_INTERVAL_MS;
uint8_t *onboardLatestJpeg = nullptr;
size_t onboardLatestJpegLen = 0;
uint32_t onboardCaptureCount = 0;
uint32_t onboardCaptureFailures = 0;
unsigned long onboardLastCaptureMs = 0;

String wifiScanLastJson = "{\"networks\":[]}";
uint16_t wifiScanLastCount = 0;
unsigned long wifiScanLastMs = 0;
bool wifiScanBusy = false;

Preferences runtimePrefs;
uint32_t persistentBootCount = 0;
uint32_t bootSessionId = 0;

struct CameraProbeTarget {
  const char *label;
  const char *path;
};

struct RtspSessionInfo {
  String describeUrl;
  String aggregateControlUrl;
  String mediaControlUrl;
  String sessionHeader;
  String describeResponse;
  String setupResponse;
  String playResponse;
  int describeStatus;
  int setupStatus;
  int playStatus;
};

struct UdpInspectorStats {
  const char *label;
  uint16_t localPort;
  uint32_t packets;
  uint32_t bytes;
  IPAddress lastSourceIp;
  uint16_t lastSourcePort;
  size_t lastPacketLen;
};

static BLEAdvertisedDevice *bleTargetDevice = nullptr;
static BLEClient *bleClient = nullptr;
static BLERemoteCharacteristic *bleNotifyChar3 = nullptr;
static BLERemoteCharacteristic *bleDataChar4 = nullptr;
static UdpInspectorStats udpPrimaryStats = {"primary", LOCAL_MEDIA_PORT_PRIMARY, 0, 0, IPAddress(), 0, 0};
static UdpInspectorStats udpSecondaryStats = {"secondary", LOCAL_MEDIA_PORT_SECONDARY, 0, 0, IPAddress(), 0, 0};
static String lastStreamSdp;
static String lastRtspSessionId;

struct TunnelFrameHeader {
  char magic[4];
  uint8_t version;
  uint8_t streamId;
  uint8_t flags;
  uint8_t reserved;
  uint32_t timestampMs;
  uint16_t payloadLen;
} __attribute__((packed));

struct TunnelSharedState {
  portMUX_TYPE lock;
  int socketFd;
  bool connected;
};

static TunnelSharedState tunnelState = {portMUX_INITIALIZER_UNLOCKED, -1, false};

enum TunnelStreamId : uint8_t {
  STREAM_ID_VIDEO_RTP = 0,
  STREAM_ID_VIDEO_RTCP = 1,
};

enum TunnelControlType : uint8_t {
  CONTROL_TYPE_START = 1,
  CONTROL_TYPE_STOP = 2,
  CONTROL_TYPE_REGISTER = 3,
};

enum ControlAction : uint8_t {
  CONTROL_ACTION_NONE = 0,
  CONTROL_ACTION_BRINGUP = 1,
  CONTROL_ACTION_STREAM_START = 2,
  CONTROL_ACTION_STREAM_STOP = 3,
};

struct ControlState {
  portMUX_TYPE lock;
  ControlAction pendingAction;
  ControlAction activeAction;
  ControlAction lastAction;
  bool busy;
  bool lastOk;
  unsigned long activeSinceMs;
  unsigned long lastFinishedMs;
  char lastMessage[96];
};

static ControlState controlState = {
  portMUX_INITIALIZER_UNLOCKED,
  CONTROL_ACTION_NONE,
  CONTROL_ACTION_NONE,
  CONTROL_ACTION_NONE,
  false,
  false,
  0,
  0,
  ""
};

uint16_t toBigEndian16(uint16_t value) {
  return static_cast<uint16_t>((value >> 8) | (value << 8));
}

uint32_t toBigEndian32(uint32_t value) {
  return ((value & 0x000000FFu) << 24) |
         ((value & 0x0000FF00u) << 8) |
         ((value & 0x00FF0000u) >> 8) |
         ((value & 0xFF000000u) >> 24);
}

int getTunnelSocketSnapshot() {
  portENTER_CRITICAL(&tunnelState.lock);
  const int fd = tunnelState.socketFd;
  portEXIT_CRITICAL(&tunnelState.lock);
  return fd;
}

void setTunnelSocketState(int fd, bool connected) {
  portENTER_CRITICAL(&tunnelState.lock);
  tunnelState.socketFd = fd;
  tunnelState.connected = connected;
  portEXIT_CRITICAL(&tunnelState.lock);
}

void closeTunnelSocket() {
  int fd = -1;
  portENTER_CRITICAL(&tunnelState.lock);
  fd = tunnelState.socketFd;
  tunnelState.socketFd = -1;
  tunnelState.connected = false;
  portEXIT_CRITICAL(&tunnelState.lock);
  if (fd >= 0) {
    close(fd);
  }
}

bool sendAll(int sock, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int written = send(sock, data + sent, len - sent, 0);
    if (written <= 0) {
      return false;
    }
    sent += static_cast<size_t>(written);
  }
  return true;
}

bool lockTunnelWrite(unsigned long timeoutMs = 1000) {
  if (tunnelWriteMutex == nullptr) {
    tunnelWriteMutex = xSemaphoreCreateMutex();
    if (tunnelWriteMutex == nullptr) {
      Serial.println("[tunnel] failed to create write mutex");
      return false;
    }
  }
  return xSemaphoreTake(tunnelWriteMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockTunnelWrite() {
  if (tunnelWriteMutex != nullptr) {
    xSemaphoreGive(tunnelWriteMutex);
  }
}

bool sendTunnelControlFrame(int sock, TunnelControlType controlType, const String &metadata) {
  if (!lockTunnelWrite()) {
    return false;
  }
  TunnelFrameHeader header{};
  memcpy(header.magic, "GPRT", 4);
  header.version = 1;
  header.streamId = 0xFF;
  header.flags = static_cast<uint8_t>(controlType);
  header.reserved = 0;
  header.timestampMs = toBigEndian32(millis());
  header.payloadLen = toBigEndian16(static_cast<uint16_t>(metadata.length()));
  if (!sendAll(sock, reinterpret_cast<const uint8_t *>(&header), sizeof(header))) {
    unlockTunnelWrite();
    return false;
  }
  if (!metadata.isEmpty()) {
    const bool ok = sendAll(sock,
                            reinterpret_cast<const uint8_t *>(metadata.c_str()),
                            metadata.length());
    unlockTunnelWrite();
    return ok;
  }
  unlockTunnelWrite();
  return true;
}

void cooperativeDelay(unsigned long durationMs) {
  const unsigned long start = millis();
  while (millis() - start < durationMs) {
    delay(1);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

String buildStreamStartMetadata() {
  String metadata = "{";
  metadata += "\"type\":\"start\"";
  metadata += ",\"codec\":\"H264\"";
  metadata += ",\"camera_ip\":\"" + CAMERA_IP.toString() + "\"";
  metadata += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  metadata += ",\"halow_mac\":\"" + HaLow.macAddress() + "\"";
  metadata += ",\"halow_bssid\":\"" + HaLow.BSSIDstr() + "\"";
  metadata += ",\"halow_rssi\":" + String(static_cast<int>(HaLow.RSSI()));
  metadata += ",\"primary_port\":" + String(LOCAL_MEDIA_PORT_PRIMARY);
  metadata += ",\"secondary_port\":" + String(LOCAL_MEDIA_PORT_SECONDARY);
  metadata += ",\"rtsp_session\":\"" + lastRtspSessionId + "\"";
  metadata += ",\"sdp\":\"";
  for (size_t i = 0; i < lastStreamSdp.length(); ++i) {
    const char c = lastStreamSdp[i];
    if (c == '\\' || c == '\"') {
      metadata += '\\';
      metadata += c;
    } else if (c == '\r') {
      continue;
    } else if (c == '\n') {
      metadata += "\\n";
    } else {
      metadata += c;
    }
  }
  metadata += "\"}";
  return metadata;
}

String buildRegistrationMetadata() {
  String metadata = "{";
  metadata += "\"type\":\"register\"";
  metadata += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  metadata += ",\"halow_mac\":\"" + HaLow.macAddress() + "\"";
  metadata += ",\"halow_bssid\":\"" + HaLow.BSSIDstr() + "\"";
  metadata += ",\"halow_rssi\":" + String(static_cast<int>(HaLow.RSSI()));
  metadata += ",\"halow_ssid\":\"" + HaLow.SSID() + "\"";
  metadata += ",\"halow_gateway\":\"" + HaLow.gatewayIP().toString() + "\"";
  metadata += "}";
  return metadata;
}

bool sendBoardRegistration() {
  if (!halowConnected) {
    return false;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    Serial.println("[register] socket create failed");
    return false;
  }

  sockaddr_in upstreamAddr{};
  upstreamAddr.sin_family = AF_INET;
  upstreamAddr.sin_addr.s_addr = inet_addr(UPSTREAM_MEDIA_IP.toString().c_str());
  upstreamAddr.sin_port = htons(UPSTREAM_TUNNEL_PORT);

  if (connect(sock, reinterpret_cast<sockaddr *>(&upstreamAddr), sizeof(upstreamAddr)) != 0) {
    Serial.println("[register] connect failed");
    close(sock);
    return false;
  }

  const String metadata = buildRegistrationMetadata();
  const bool ok = sendTunnelControlFrame(sock, CONTROL_TYPE_REGISTER, metadata);
  close(sock);
  Serial.printf("[register] sent=%s bytes=%u ip=%s mac=%s\n",
                ok ? "yes" : "no",
                static_cast<unsigned>(metadata.length()),
                HaLow.localIP().toString().c_str(),
                HaLow.macAddress().c_str());
  return ok;
}

bool connectTunnelSocket() {
  closeTunnelSocket();

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    Serial.println("[tunnel] socket create failed");
    return false;
  }

  sockaddr_in upstreamAddr{};
  upstreamAddr.sin_family = AF_INET;
  upstreamAddr.sin_addr.s_addr = inet_addr(UPSTREAM_MEDIA_IP.toString().c_str());
  upstreamAddr.sin_port = htons(UPSTREAM_TUNNEL_PORT);

  Serial.printf("[tunnel] connecting to %s:%u\n",
                UPSTREAM_MEDIA_IP.toString().c_str(),
                UPSTREAM_TUNNEL_PORT);
  if (connect(sock, reinterpret_cast<sockaddr *>(&upstreamAddr), sizeof(upstreamAddr)) != 0) {
    Serial.println("[tunnel] connect failed");
    close(sock);
    return false;
  }

  setTunnelSocketState(sock, true);
  tunnelEverConnected = true;
  tunnelLastConnectMs = millis();
  const String metadata = buildStreamStartMetadata();
  if (!sendTunnelControlFrame(sock, CONTROL_TYPE_START, metadata)) {
    Serial.println("[tunnel] failed to send start metadata");
    closeTunnelSocket();
    return false;
  }

  Serial.printf("[tunnel] connected, start metadata bytes=%u\n",
                static_cast<unsigned>(metadata.length()));
  return true;
}

void closeRtspSession() {
  if (cameraRtspClient.connected()) {
    cameraRtspClient.stop();
  }
  rtspSessionOpen = false;
  rtspPlayUrl = "";
  rtspSessionHeader = "";
  lastRtspKeepaliveMs = 0;
  lastBleKeepaliveMs = 0;
  lastHttpKeepaliveMs = 0;
}

void stopTunnelSession(const char *reason) {
  const unsigned long activeMs = msSince(streamSessionStartedMs);
  Serial.printf("[stream] stopping reason=%s active_ms=%lu primary_gap_ms=%lu secondary_gap_ms=%lu\n",
                reason,
                activeMs,
                msSince(lastPrimaryPacketMs),
                msSince(lastSecondaryPacketMs));
  const int sock = getTunnelSocketSnapshot();
  if (sock >= 0) {
    String metadata = "{\"type\":\"stop\",\"reason\":\"";
    metadata += reason;
    metadata += "\"}";
    sendTunnelControlFrame(sock, CONTROL_TYPE_STOP, metadata);
  }
  closeTunnelSocket();
  closeRtspSession();
  streamSessionActive = false;
  lastStreamStopMs = millis();
  streamSessionStartedMs = 0;
}

bool sendTunnelMediaPacket(uint8_t streamId, uint8_t flags, const uint8_t *payload, size_t payloadLen) {
  if (payloadLen > 0xFFFFu) {
    ++tunnelSendFailures;
    return false;
  }

  const int sock = getTunnelSocketSnapshot();
  if (sock < 0) {
    ++tunnelSendFailures;
    return false;
  }

  TunnelFrameHeader header{};
  memcpy(header.magic, "GPRT", 4);
  header.version = 1;
  header.streamId = streamId;
  header.flags = flags;
  header.reserved = 0;
  header.timestampMs = toBigEndian32(millis());
  header.payloadLen = toBigEndian16(static_cast<uint16_t>(payloadLen));

  if (!lockTunnelWrite()) {
    ++tunnelSendFailures;
    return false;
  }
  if (!sendAll(sock, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) ||
      !sendAll(sock, payload, payloadLen)) {
    unlockTunnelWrite();
    ++tunnelSendFailures;
    Serial.println("[tunnel] media send failed, closing socket");
    closeTunnelSocket();
    return false;
  }
  unlockTunnelWrite();

  ++tunnelPacketsSent;
  tunnelBytesSent += payloadLen;
  return true;
}

bool shouldLogUdpPacket(uint32_t packetCount) {
  return packetCount <= UDP_LOG_FIRST_PACKETS ||
         (UDP_LOG_EVERY_N_PACKETS > 0 && (packetCount % UDP_LOG_EVERY_N_PACKETS) == 0);
}

unsigned long msSince(unsigned long timestampMs) {
  if (timestampMs == 0) {
    return 0;
  }
  return millis() - timestampMs;
}

uint32_t readBatteryAdcMilliVolts(int samples = 64) {
  digitalWrite(BAT_ADC_CTRL_PIN, LOW);
  delay(20);
  uint64_t total = 0;
  for (int i = 0; i < samples; ++i) {
    total += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  return static_cast<uint32_t>(total / samples);
}

String buildBatteryJson() {
  const uint32_t adcMv = readBatteryAdcMilliVolts();
  const float batteryV = (adcMv / 1000.0f) * 2.0f;
  String payload = "{";
  payload += "\"adc_pin\":" + String(BAT_ADC_PIN);
  payload += ",\"adc_ctrl_pin\":" + String(BAT_ADC_CTRL_PIN);
  payload += ",\"adc_mv\":" + String(adcMv);
  payload += ",\"battery_est_v\":" + String(batteryV, 3);
  payload += ",\"charging_gpio15\":" + String(digitalRead(BAT_CHRG_PIN));
  payload += ",\"done_gpio16\":" + String(digitalRead(BAT_DONE_PIN));
  payload += "}";
  return payload;
}

void initBootIdentity() {
  if (!runtimePrefs.begin("runtime", false)) {
    Serial.println("[boot] failed to open runtime preferences");
    persistentBootCount = 0;
    bootSessionId = millis();
    return;
  }
  persistentBootCount = runtimePrefs.getUInt("boot_count", 0) + 1;
  runtimePrefs.putUInt("boot_count", persistentBootCount);
  runtimePrefs.end();
  bootSessionId = persistentBootCount;
  Serial.printf("[boot] boot_count=%u boot_session_id=%u uptime_ms=%lu\n",
                persistentBootCount,
                bootSessionId,
                millis());
}

bool initOnboardCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 8;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[onboard-camera] init failed err=0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != nullptr) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);
  }

  for (int i = 0; i < 3; ++i) {
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup != nullptr) {
      esp_camera_fb_return(warmup);
    }
    delay(150);
  }

  onboardCameraReady = true;
  Serial.printf("[onboard-camera] ready psram=%s interval_ms=%lu enabled=%s\n",
                psramFound() ? "yes" : "no",
                onboardCaptureIntervalMs,
                onboardCaptureEnabled ? "yes" : "no");
  return true;
}

bool captureOnboardFrame() {
  if (!onboardCameraReady) {
    ++onboardCaptureFailures;
    return false;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr || fb->len == 0) {
    ++onboardCaptureFailures;
    if (fb != nullptr) {
      esp_camera_fb_return(fb);
    }
    return false;
  }

  uint8_t *copy = static_cast<uint8_t *>(psramFound() ? ps_malloc(fb->len) : malloc(fb->len));
  if (copy == nullptr) {
    ++onboardCaptureFailures;
    esp_camera_fb_return(fb);
    return false;
  }
  memcpy(copy, fb->buf, fb->len);
  const size_t copyLen = fb->len;
  esp_camera_fb_return(fb);

  if (onboardFrameMutex != nullptr) {
    xSemaphoreTake(onboardFrameMutex, portMAX_DELAY);
  }
  uint8_t *old = onboardLatestJpeg;
  onboardLatestJpeg = copy;
  onboardLatestJpegLen = copyLen;
  onboardCaptureCount++;
  onboardLastCaptureMs = millis();
  if (onboardFrameMutex != nullptr) {
    xSemaphoreGive(onboardFrameMutex);
  }
  if (old != nullptr) {
    free(old);
  }

  Serial.printf("[onboard-camera] captured bytes=%u count=%u\n",
                static_cast<unsigned>(copyLen),
                onboardCaptureCount);
  return true;
}

String buildOnboardCameraStatusJson() {
  size_t latestLen = 0;
  uint32_t count = 0;
  uint32_t failures = 0;
  unsigned long lastMs = 0;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreTake(onboardFrameMutex, portMAX_DELAY);
  }
  latestLen = onboardLatestJpegLen;
  count = onboardCaptureCount;
  failures = onboardCaptureFailures;
  lastMs = onboardLastCaptureMs;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreGive(onboardFrameMutex);
  }

  String payload = "{";
  payload += "\"ready\":" + String(onboardCameraReady ? "true" : "false");
  payload += ",\"enabled\":" + String(onboardCaptureEnabled ? "true" : "false");
  payload += ",\"interval_ms\":" + String(onboardCaptureIntervalMs);
  payload += ",\"latest_bytes\":" + String(static_cast<unsigned>(latestLen));
  payload += ",\"captures\":" + String(count);
  payload += ",\"failures\":" + String(failures);
  payload += ",\"last_capture_age_ms\":" + String(msSince(lastMs));
  payload += "}";
  return payload;
}

void onboardCaptureTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    if (onboardCaptureEnabled && onboardCameraReady) {
      captureOnboardFrame();
    }
    const unsigned long delayMs = onboardCaptureIntervalMs < 5000 ? 5000 : onboardCaptureIntervalMs;
    vTaskDelay(pdMS_TO_TICKS(delayMs));
  }
}

String buildWifiScanJson(int scanCount) {
  String payload = "{";
  payload += "\"scanner\":\"active_wifi_scan\"";
  payload += ",\"count\":" + String(scanCount);
  payload += ",\"age_ms\":0";
  payload += ",\"networks\":[";
  for (int i = 0; i < scanCount; ++i) {
    if (i > 0) {
      payload += ",";
    }
    payload += "{";
    payload += "\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
    payload += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
    payload += ",\"rssi\":" + String(WiFi.RSSI(i));
    payload += ",\"channel\":" + String(WiFi.channel(i));
    payload += ",\"encryption\":" + String(static_cast<int>(WiFi.encryptionType(i)));
    payload += "}";
  }
  payload += "]}";
  return payload;
}

bool runIdleWifiScan(String &payload, String &error) {
  if (wifiScanBusy) {
    error = "scan_busy";
    return false;
  }
  if (streamSessionActive || wifiConnected || isControlActionActive()) {
    error = "camera_wifi_active";
    return false;
  }

  wifiScanBusy = true;
  const wifi_mode_t previousMode = WiFi.getMode();
  WiFi.mode(WIFI_STA);
  delay(50);
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    error = "scan_failed";
    wifiScanBusy = false;
    return false;
  }
  payload = buildWifiScanJson(count);
  WiFi.scanDelete();
  wifiScanLastJson = payload;
  wifiScanLastCount = count;
  wifiScanLastMs = millis();
  WiFi.mode(previousMode);
  wifiScanBusy = false;
  return true;
}

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

const char *controlActionName(ControlAction action) {
  switch (action) {
    case CONTROL_ACTION_BRINGUP:
      return "bringup";
    case CONTROL_ACTION_STREAM_START:
      return "stream_start";
    case CONTROL_ACTION_STREAM_STOP:
      return "stream_stop";
    case CONTROL_ACTION_NONE:
    default:
      return "none";
  }
}

void snapshotControlState(ControlState &snapshot) {
  portENTER_CRITICAL(&controlState.lock);
  snapshot = controlState;
  portEXIT_CRITICAL(&controlState.lock);
}

bool isControlActionActive() {
  ControlState snapshot{};
  snapshotControlState(snapshot);
  return snapshot.busy || snapshot.pendingAction != CONTROL_ACTION_NONE;
}

bool queueControlAction(ControlAction action, String &message) {
  bool accepted = false;
  const char *messageType = "queued";
  ControlAction messageAction = action;
  portENTER_CRITICAL(&controlState.lock);
  if (controlState.busy) {
    messageType = "busy";
    messageAction = controlState.activeAction;
  } else if (controlState.pendingAction != CONTROL_ACTION_NONE) {
    if (controlState.pendingAction == action) {
      accepted = true;
      messageType = "already_pending";
    } else {
      messageType = "pending";
      messageAction = controlState.pendingAction;
    }
  } else {
    controlState.pendingAction = action;
    accepted = true;
  }
  portEXIT_CRITICAL(&controlState.lock);
  message = String(messageType) + ":" + controlActionName(messageAction);
  return accepted;
}

ControlAction takePendingControlAction() {
  portENTER_CRITICAL(&controlState.lock);
  if (controlState.busy || controlState.pendingAction == CONTROL_ACTION_NONE) {
    portEXIT_CRITICAL(&controlState.lock);
    return CONTROL_ACTION_NONE;
  }
  const ControlAction action = controlState.pendingAction;
  controlState.pendingAction = CONTROL_ACTION_NONE;
  controlState.activeAction = action;
  controlState.busy = true;
  controlState.activeSinceMs = millis();
  portEXIT_CRITICAL(&controlState.lock);
  return action;
}

void finishControlAction(ControlAction action, bool ok, const char *message) {
  portENTER_CRITICAL(&controlState.lock);
  controlState.busy = false;
  controlState.activeAction = CONTROL_ACTION_NONE;
  controlState.lastAction = action;
  controlState.lastOk = ok;
  controlState.lastFinishedMs = millis();
  controlState.activeSinceMs = 0;
  snprintf(controlState.lastMessage,
           sizeof(controlState.lastMessage),
           "%s",
           message == nullptr ? "" : message);
  portEXIT_CRITICAL(&controlState.lock);
}

bool sendRtspKeepalive() {
  if (!rtspSessionOpen || !cameraRtspClient.connected()) {
    return false;
  }

  String responseText;
  int statusCode = 0;
  String extraHeaders = "Session: " + rtspSessionHeader + "\r\n";
  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "OPTIONS",
                                 rtspPlayUrl,
                                 extraHeaders,
                                 responseText,
                                 statusCode)) {
    Serial.println("[rtsp] keepalive failed");
    closeRtspSession();
    return false;
  }

  if (statusCode != 200) {
    Serial.printf("[rtsp] keepalive status=%d\n", statusCode);
    return false;
  }

  Serial.println("[rtsp] keepalive ok");
  lastRtspKeepaliveMs = millis();
  return true;
}

bool sendBleWakePulse() {
  if (bleClient == nullptr || !bleClient->isConnected() || bleDataChar4 == nullptr) {
    return false;
  }
  if (!(bleDataChar4->canWrite() || bleDataChar4->canWriteNoResponse())) {
    return false;
  }

  Serial.println("[BLE] keepalive wake pulse");
  bleDataChar4->writeValue(reinterpret_cast<uint8_t *>(const_cast<char *>(CAMERA_BLE_WAKE)),
                           strlen(CAMERA_BLE_WAKE),
                           true);
  lastBleKeepaliveMs = millis();
  return true;
}

void resetBleScanStats() {
  bleScanMode = "idle";
  bleLastSeenMac = "";
  bleLastSeenName = "";
  bleLastSeenRssi = -127;
  bleBestSeenRssi = -127;
  bleBestSeenMac = "";
  bleBestSeenName = "";
  bleScanResultCount = 0;
  bleTargetSeenCount = 0;
  bleScanAttemptCounter = 0;
  for (size_t i = 0; i < BLE_RECENT_DEVICE_SLOTS; ++i) {
    bleRecentMacs[i] = "";
    bleRecentNames[i] = "";
    bleRecentServices[i] = "";
    bleRecentRssis[i] = -127;
  }
}

void rememberBleAdvertiser(const String &mac, const String &name, const String &serviceUuid, int rssi) {
  size_t slot = BLE_RECENT_DEVICE_SLOTS;
  for (size_t i = 0; i < BLE_RECENT_DEVICE_SLOTS; ++i) {
    if (bleRecentMacs[i] == mac) {
      slot = i;
      break;
    }
  }
  if (slot == BLE_RECENT_DEVICE_SLOTS) {
    for (size_t i = 0; i + 1 < BLE_RECENT_DEVICE_SLOTS; ++i) {
      bleRecentMacs[i] = bleRecentMacs[i + 1];
      bleRecentNames[i] = bleRecentNames[i + 1];
      bleRecentServices[i] = bleRecentServices[i + 1];
      bleRecentRssis[i] = bleRecentRssis[i + 1];
    }
    slot = BLE_RECENT_DEVICE_SLOTS - 1;
  }
  bleRecentMacs[slot] = mac;
  bleRecentNames[slot] = name;
  bleRecentServices[slot] = serviceUuid;
  bleRecentRssis[slot] = rssi;
}

String buildBleRecentDevicesJson() {
  String payload = "[";
  bool first = true;
  for (size_t i = 0; i < BLE_RECENT_DEVICE_SLOTS; ++i) {
    if (bleRecentMacs[i].isEmpty()) {
      continue;
    }
    if (!first) {
      payload += ",";
    }
    first = false;
    payload += "{";
    payload += "\"mac\":\"" + jsonEscape(bleRecentMacs[i]) + "\"";
    payload += ",\"name\":\"" + jsonEscape(bleRecentNames[i]) + "\"";
    payload += ",\"service\":\"" + jsonEscape(bleRecentServices[i]) + "\"";
    payload += ",\"rssi\":" + String(bleRecentRssis[i]);
    payload += "}";
  }
  payload += "]";
  return payload;
}

bool advertisedDeviceLooksLikeCamera(const String &mac, const String &name, const String &serviceUuid) {
  if (mac.equalsIgnoreCase(CAMERA_BLE_MAC)) {
    return true;
  }
  if (!name.isEmpty() && (name == CAMERA_BLE_NAME || name.startsWith(CAMERA_BLE_NAME_PREFIX))) {
    return true;
  }
  if (!serviceUuid.isEmpty() && serviceUuid == CAMERA_BLE_SERVICE_UUID) {
    return true;
  }
  return false;
}

bool tryExistingBleWakeSession() {
  if (bleClient == nullptr || !bleClient->isConnected() || bleDataChar4 == nullptr) {
    return false;
  }

  bleStage = "reuse_wake";
  bleWakeSawOk = false;
  bleLastNotifyText = "";
  bool sent = false;
  for (uint8_t attempt = 1; attempt <= BLE_WAKE_PULSE_ATTEMPTS; ++attempt) {
    Serial.printf("[BLE] cached wake attempt %u/%u\n", attempt, BLE_WAKE_PULSE_ATTEMPTS);
    sent = sendBleWakePulse() || sent;
    cooperativeDelay(BLE_WAKE_PULSE_DELAY_MS);
    if (bleWakeSawOk) {
      break;
    }
  }
  Serial.printf("[BLE] cached wake sent=%s ok=%s last=%s\n",
                sent ? "yes" : "no",
                bleWakeSawOk ? "yes" : "no",
                bleLastNotifyText.c_str());
  return sent;
}

bool sendHttpKeepalive() {
  if (!wifiConnected || standbyRequested) {
    return false;
  }

  String body;
  int statusCode = 0;
  const bool ok = proxyCameraRequest("GET", "/cmd/standby/reset", body, statusCode);
  lastHttpKeepaliveMs = millis();
  Serial.printf("[http] keepalive ok=%s status=%d bytes=%u\n",
                ok ? "yes" : "no",
                statusCode,
                static_cast<unsigned>(body.length()));
  const bool success = ok && statusCode == 200;
  if (success) {
    httpKeepaliveFailures = 0;
  } else {
    ++httpKeepaliveFailures;
  }
  return success;
}

void closeBleWakeSession() {
  if (bleClient != nullptr) {
    if (bleClient->isConnected()) {
      Serial.println("[BLE] closing wake session");
      bleClient->disconnect();
    }
    bleClient = nullptr;
  }
}

void refreshWifiState() {
  wifiConnected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress((uint32_t)0));
}

static void bleNotifyCallback(BLERemoteCharacteristic *characteristic,
                              uint8_t *data,
                              size_t length,
                              bool isNotify) {
  (void)isNotify;
  ++bleNotifyCount;
  String text;
  for (size_t i = 0; i < length; ++i) {
    text += static_cast<char>(data[i]);
  }
  text.trim();
  bleLastNotifyText = text;
  Serial.printf("[BLE notify] char=%s len=%u text=%s\n",
                characteristic->getUUID().toString().c_str(),
                static_cast<unsigned>(length),
                text.c_str());
  if (text == "OK") {
    bleWakeSawOk = true;
  }
}

class BridgeBleClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *client) override {
    Serial.printf("[BLE] connected to %s\n", client->getPeerAddress().toString().c_str());
  }

  void onDisconnect(BLEClient *client) override {
    Serial.printf("[BLE] disconnected from %s\n", client->getPeerAddress().toString().c_str());
  }
};

class BridgeBleAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    const String mac = advertisedDevice.getAddress().toString().c_str();
    String name = "";
    if (advertisedDevice.haveName()) {
      name = advertisedDevice.getName().c_str();
    }
    String serviceUuid = "";
    if (advertisedDevice.haveServiceUUID()) {
      serviceUuid = advertisedDevice.getServiceUUID().toString().c_str();
      serviceUuid.toLowerCase();
    }
    const int rssi = advertisedDevice.getRSSI();
    ++bleScanResultCount;
    bleLastSeenMac = mac;
    bleLastSeenName = name;
    bleLastSeenRssi = rssi;
    rememberBleAdvertiser(mac, name, serviceUuid, rssi);
    if (rssi > bleBestSeenRssi) {
      bleBestSeenRssi = rssi;
      bleBestSeenMac = mac;
      bleBestSeenName = name;
    }
    if (advertisedDeviceLooksLikeCamera(mac, name, serviceUuid)) {
      ++bleTargetSeenCount;
      Serial.printf("[BLE] found target advertisement %s", mac.c_str());
      if (!name.isEmpty()) {
        Serial.printf(" name=%s", name.c_str());
      }
      if (!serviceUuid.isEmpty()) {
        Serial.printf(" service=%s", serviceUuid.c_str());
      }
      Serial.printf(" rssi=%d", rssi);
      Serial.println();
      BLEDevice::getScan()->stop();
      bleTargetDevice = new BLEAdvertisedDevice(advertisedDevice);
    } else if (bleScanResultCount <= 5 || (bleScanResultCount % 25) == 0) {
      Serial.printf("[BLE] saw advertisement mac=%s rssi=%d", mac.c_str(), rssi);
      if (!name.isEmpty()) {
        Serial.printf(" name=%s", name.c_str());
      }
      if (!serviceUuid.isEmpty()) {
        Serial.printf(" service=%s", serviceUuid.c_str());
      }
      Serial.println();
    }
  }
};

bool runBleDiscoveryPass(BLEScan *scan,
                         bool activeScan,
                         uint8_t attempts,
                         uint16_t windowSec,
                         const char *modeLabel) {
  bleScanMode = modeLabel;
  scan->setActiveScan(activeScan);
  for (uint8_t attempt = 1; attempt <= attempts && bleTargetDevice == nullptr; ++attempt) {
    ++bleScanAttemptCounter;
    Serial.printf("[BLE] %s scan attempt %u/%u window=%us results=%u best_mac=%s best_rssi=%d\n",
                  modeLabel,
                  attempt,
                  attempts,
                  static_cast<unsigned>(windowSec),
                  static_cast<unsigned>(bleScanResultCount),
                  bleBestSeenMac.c_str(),
                  bleBestSeenRssi);
    scan->start(windowSec, false);
    if (bleTargetDevice == nullptr) {
      Serial.printf("[BLE] target not found in %s scan window results=%u best_mac=%s best_rssi=%d\n",
                    modeLabel,
                    static_cast<unsigned>(bleScanResultCount),
                    bleBestSeenMac.c_str(),
                    bleBestSeenRssi);
      cooperativeDelay(BLE_SCAN_RETRY_DELAY_MS);
    }
  }
  return bleTargetDevice != nullptr;
}

bool runExactBleWake() {
  bleNotifyCount = 0;
  bleWakeSawOk = false;
  bleLastNotifyText = "";
  bleNotifyChar3 = nullptr;
  bleDataChar4 = nullptr;
  bleStage = "scan";
  resetBleScanStats();

  Serial.printf("[BLE] scanning for target %s\n", CAMERA_BLE_MAC);
  Serial.println("[BLE] warmup before BLE init");
  cooperativeDelay(5000);
  BLEDevice::init("");
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new BridgeBleAdvertisedDeviceCallbacks(), true, true);
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);
  bleTargetDevice = nullptr;
  runBleDiscoveryPass(scan, true, BLE_SCAN_ATTEMPTS, BLE_SCAN_WINDOW_SEC, "active");
  if (bleTargetDevice == nullptr) {
    runBleDiscoveryPass(scan, false, BLE_PASSIVE_SCAN_ATTEMPTS, BLE_PASSIVE_SCAN_WINDOW_SEC, "passive");
  }

  if (bleTargetDevice == nullptr) {
    Serial.printf("[BLE] target advertisement not found results=%u best_mac=%s best_name=%s best_rssi=%d attempts=%u\n",
                  static_cast<unsigned>(bleScanResultCount),
                  bleBestSeenMac.c_str(),
                  bleBestSeenName.c_str(),
                  bleBestSeenRssi,
                  static_cast<unsigned>(bleScanAttemptCounter));
    bleStage = "scan_not_found";
    return false;
  }

  bleStage = "connect";
  bleClient = BLEDevice::createClient();
  bleClient->setClientCallbacks(new BridgeBleClientCallbacks());
  if (!bleClient->connect(bleTargetDevice)) {
    Serial.println("[BLE] connect failed");
    bleStage = "connect_failed";
    return false;
  }

  bleClient->setMTU(517);
  bleStage = "enumerate";

  auto *services = bleClient->getServices();
  Serial.printf("[BLE] discovered %u services\n", static_cast<unsigned>(services->size()));
  for (auto it = services->begin(); it != services->end(); ++it) {
    BLERemoteService *service = it->second;
    auto *chars = service->getCharacteristics();
    for (auto cit = chars->begin(); cit != chars->end(); ++cit) {
      BLERemoteCharacteristic *ch = cit->second;
      String uuid = ch->getUUID().toString().c_str();
      uuid.toLowerCase();
      if (uuid == "6e400003-b5a3-f393-e0a9-e50e24dcca9e") {
        bleNotifyChar3 = ch;
      } else if (uuid == "6e400004-b5a3-f393-e0a9-e50e24dcca9e") {
        bleDataChar4 = ch;
      }
    }
  }

  if (bleNotifyChar3 != nullptr && bleNotifyChar3->canNotify()) {
    Serial.println("[BLE] register notify on 6e400003");
    bleNotifyChar3->registerForNotify(bleNotifyCallback);
  }
  if (bleDataChar4 != nullptr && (bleDataChar4->canNotify() || bleDataChar4->canIndicate())) {
    Serial.println("[BLE] register notify on 6e400004");
    bleDataChar4->registerForNotify(bleNotifyCallback);
  }

  if (bleDataChar4 == nullptr || !(bleDataChar4->canWrite() || bleDataChar4->canWriteNoResponse())) {
    Serial.println("[BLE] 6e400004 unavailable for wake");
    bleStage = "write_char_missing";
    closeBleWakeSession();
    return false;
  }

  bleStage = "wake";
  cooperativeDelay(150);
  for (int attempt = 1; attempt <= 3; ++attempt) {
    Serial.printf("[BLE] wake attempt %d/3 -> 6e400004 payload=%s", attempt, CAMERA_BLE_WAKE);
    bleDataChar4->writeValue(reinterpret_cast<uint8_t *>(const_cast<char *>(CAMERA_BLE_WAKE)),
                             strlen(CAMERA_BLE_WAKE),
                             true);
    lastBleKeepaliveMs = millis();
    cooperativeDelay(350);
  }

  cooperativeDelay(2000);
  Serial.printf("[BLE] wake done notify_count=%u ok=%s last=%s\n",
                bleNotifyCount,
                bleWakeSawOk ? "yes" : "no",
                bleLastNotifyText.c_str());
  bleStage = bleWakeSawOk ? "wake_ok" : "wake_no_notify";
  return bleWakeSawOk;
}

void onHaLowEvent(HaLowEvent_t event) {
  ++halowEventCount;
  halowLastEventId = event;
  halowLastEventMs = millis();
  Serial.printf("[HaLow-event] %d\n", event);
  switch (event) {
    case ARDUINO_HALOW_EVENT_STA_GOT_IP:
      halowConnected = true;
      Serial.printf("HaLow IP: %s\n", HaLow.localIP().toString().c_str());
      break;
    case ARDUINO_HALOW_EVENT_STA_DISCONNECTED:
    case ARDUINO_HALOW_EVENT_STA_LOST_IP:
      halowConnected = false;
      break;
    default:
      break;
  }
}

void printSerialHelp() {
  Serial.println("Serial commands:");
  Serial.println("  help");
  Serial.println("  bringup");
  Serial.println("  halow_up");
  Serial.println("  status");
  Serial.println("  selftest");
  Serial.println("  http <path>");
  Serial.println("  httpm <METHOD> <path>");
  Serial.println("  rtsp_probe");
  Serial.println("  rtsp_live");
  Serial.println("  stream_start");
  Serial.println("  stream_stop");
  Serial.println("  battery");
  Serial.println("  onboard_status");
  Serial.println("  onboard_capture");
  Serial.println("  wifi_scan");
  Serial.println("  rtsp <METHOD> <url>");
  Serial.println("  wake");
  Serial.println("  bleclose");
}

void connectCameraWifi() {
  Serial.printf("Connecting camera WiFi SSID %s\n", CAMERA_WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_MODE_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(true, true);
  cooperativeDelay(250);

  Serial.println("Scanning for camera hotspot before connect");
  const int count = WiFi.scanNetworks();
  bool foundTarget = false;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    const int rssi = WiFi.RSSI(i);
    if (ssid == CAMERA_WIFI_SSID) {
      foundTarget = true;
      Serial.printf("Found target SSID %s RSSI=%d\n", ssid.c_str(), rssi);
    }
  }
  if (!foundTarget) {
    Serial.printf("Target SSID %s not seen in scan\n", CAMERA_WIFI_SSID);
  }

  WiFi.begin(CAMERA_WIFI_SSID, CAMERA_WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    cooperativeDelay(100);
    Serial.print(".");
  }
  Serial.println();

  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    cameraWifiEverConnected = true;
    standbyRequested = false;
    lastHttpKeepaliveMs = millis();
    Serial.printf("Camera WiFi connected, IP=%s gateway=%s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());
  } else {
    Serial.println("Camera WiFi connect timed out");
  }
}

bool recoverActiveStream(const char *reason) {
  if (millis() - lastStreamRecoveryMs < STREAM_RECOVERY_COOLDOWN_MS) {
    return false;
  }
  lastStreamRecoveryMs = millis();
  ++streamRecoveryAttempts;
  Serial.printf("[stream] recovery start reason=%s attempt=%u\n",
                reason,
                streamRecoveryAttempts);

  stopTunnelSession(reason);
  refreshWifiState();

  if (!wifiConnected) {
    closeBleWakeSession();
    bleWakeAttempted = true;
    bleWakeConfirmed = runExactBleWake();
    Serial.printf("[BLE] recovery wake result: %s stage=%s\n",
                  bleWakeConfirmed ? "success" : "no-confirmation",
                  bleStage.c_str());
    const bool hotspotVisible = waitForCameraWifiPresence(30000, 3000);
    Serial.printf("[WiFi] hotspot visibility during recovery: %s\n",
                  hotspotVisible ? "yes" : "no");
    connectCameraWifi();
    if (RUN_LOCAL_SERIAL_TEST) {
      Serial.println("[BLE] keeping wake session open in local serial mode");
    } else {
      closeBleWakeSession();
    }
    refreshWifiState();
  }

  if (!wifiConnected) {
    Serial.println("[stream] recovery aborted, camera WiFi still down");
    return false;
  }

  const bool restarted = startStreamSession();
  Serial.printf("[stream] recovery restart=%s\n", restarted ? "ok" : "failed");
  return restarted;
}

bool recoverIdleWifi(const char *reason) {
  if (isControlActionActive()) {
    Serial.printf("[idle] skipping wifi recovery during control action reason=%s\n", reason);
    return false;
  }
  if (standbyRequested) {
    Serial.printf("[idle] skipping wifi recovery during requested standby reason=%s\n", reason);
    return false;
  }
  if (!cameraWifiEverConnected) {
    Serial.printf("[idle] skipping wifi recovery before first camera WiFi success reason=%s\n", reason);
    return false;
  }
  if (millis() - lastIdleRecoveryMs < IDLE_WIFI_RECOVERY_COOLDOWN_MS) {
    return false;
  }
  lastIdleRecoveryMs = millis();
  ++idleRecoveryAttempts;
  Serial.printf("[idle] wifi recovery start reason=%s attempt=%u\n",
                reason,
                idleRecoveryAttempts);

  closeBleWakeSession();
  WiFi.disconnect(true, true);
  cooperativeDelay(250);
  refreshWifiState();

  const bool restored = runBringupSequence();
  Serial.printf("[idle] wifi recovery restored=%s\n", restored ? "yes" : "no");
  if (restored) {
    httpKeepaliveFailures = 0;
    startHttpServer();
  }
  return restored;
}

void scanCameraWifiPresence() {
  Serial.println("Rescanning for camera hotspot");
  const int count = WiFi.scanNetworks();
  bool foundTarget = false;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid == CAMERA_WIFI_SSID) {
      foundTarget = true;
      Serial.printf("Target SSID %s visible RSSI=%d channel=%d\n",
                    ssid.c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i));
    }
  }
  if (!foundTarget) {
    Serial.printf("Target SSID %s still not visible\n", CAMERA_WIFI_SSID);
  }
}

bool waitForCameraWifiPresence(unsigned long timeoutMs, unsigned long intervalMs) {
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    const int count = WiFi.scanNetworks();
    for (int i = 0; i < count; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid == CAMERA_WIFI_SSID) {
        Serial.printf("Target SSID %s became visible RSSI=%d channel=%d after %lu ms\n",
                      ssid.c_str(),
                      WiFi.RSSI(i),
                      WiFi.channel(i),
                      millis() - start);
        return true;
      }
    }
    Serial.printf("Target SSID %s not visible yet after %lu ms\n",
                  CAMERA_WIFI_SSID,
                  millis() - start);
    cooperativeDelay(intervalMs);
  }
  return false;
}

void connectHaLow() {
  Serial.printf("Connecting HaLow SSID %s\n", HALOW_SSID);
  if (!halowInitialized) {
    HaLow.onEvent(onHaLowEvent);
    HaLow.init(HALOW_REGION);
    halowInitialized = true;
    Serial.printf("HaLow initialized for region %s\n", HALOW_REGION);
  }
  if (!HaLow.config(HALOW_LOCAL_IP, HALOW_GATEWAY_IP, HALOW_SUBNET_MASK, HALOW_DNS1, HALOW_DNS2)) {
    Serial.println("HaLow static IP config failed");
  } else {
    Serial.printf("HaLow static IP configured: ip=%s gateway=%s subnet=%s\n",
                  HALOW_LOCAL_IP.toString().c_str(),
                  HALOW_GATEWAY_IP.toString().c_str(),
                  HALOW_SUBNET_MASK.toString().c_str());
  }
  HaLow.begin(HALOW_SSID, HALOW_PASS);

  unsigned long start = millis();
  while (HaLow.status() != WL_CONNECTED && millis() - start < 20000) {
    cooperativeDelay(100);
    Serial.print("#");
  }
  Serial.println();

  halowConnected = (HaLow.status() == WL_CONNECTED);
  if (halowConnected) {
    Serial.printf("HaLow connected, IP=%s gateway=%s\n",
                  HaLow.localIP().toString().c_str(),
                  HaLow.gatewayIP().toString().c_str());
    sendBoardRegistration();
  } else {
    Serial.println("HaLow connect timed out");
  }
}

bool proxyCameraRequest(const String &method,
                       const String &path,
                       const String &requestBody,
                       const String &contentType,
                       String &responseBody,
                       int &statusCode) {
  WiFiClient client;
  if (!client.connect(CAMERA_IP, CAMERA_HTTP_PORT)) {
    Serial.printf("Failed to connect camera for %s %s\n", method.c_str(), path.c_str());
    statusCode = 502;
    responseBody = "{\"error\":\"camera_connect_failed\"}";
    return false;
  }

  String request = method + " " + path + " HTTP/1.1\r\n";
  request += "Host: " + CAMERA_IP.toString() + ":" + String(CAMERA_HTTP_PORT) + "\r\n";
  request += "User-Agent: esp32-gardepro-bridge/0.1\r\n";
  request += "Connection: close\r\n";
  if (!requestBody.isEmpty()) {
    request += "Content-Type: " + (contentType.isEmpty() ? String("application/json") : contentType) + "\r\n";
    request += "Content-Length: " + String(requestBody.length()) + "\r\n";
  }
  request += "\r\n";
  request += requestBody;
  client.print(request);

  unsigned long timeout = millis();
  while (client.connected() && !client.available()) {
    if (millis() - timeout > 5000) {
      client.stop();
      statusCode = 504;
      responseBody = "{\"error\":\"camera_timeout\"}";
      return false;
    }
    cooperativeDelay(10);
  }

  String raw;
  while (client.available()) {
    raw += client.readString();
  }
  client.stop();

  int headerEnd = raw.indexOf("\r\n\r\n");
  String headers = headerEnd >= 0 ? raw.substring(0, headerEnd) : raw;
  responseBody = headerEnd >= 0 ? raw.substring(headerEnd + 4) : "";

  int firstLineEnd = headers.indexOf("\r\n");
  String statusLine = firstLineEnd >= 0 ? headers.substring(0, firstLineEnd) : headers;
  int firstSpace = statusLine.indexOf(' ');
  int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  if (firstSpace > 0 && secondSpace > firstSpace) {
    statusCode = statusLine.substring(firstSpace + 1, secondSpace).toInt();
  } else {
    statusCode = 200;
  }

  Serial.printf("Camera %s %s -> %d, %u bytes\n",
                method.c_str(), path.c_str(), statusCode, (unsigned)responseBody.length());
  if (statusCode >= 200 && statusCode < 300) {
    if (path == "/cmd/standby/now") {
      standbyRequested = true;
    } else if (path == "/cmd/standby/reset") {
      standbyRequested = false;
    }
  }
  return true;
}

bool proxyCameraRequest(const String &method,
                       const String &path,
                       String &responseBody,
                       int &statusCode) {
  const String emptyBody = "";
  const String emptyContentType = "";
  return proxyCameraRequest(method, path, emptyBody, emptyContentType, responseBody, statusCode);
}

bool exchangeCameraRtspRequest(WiFiClient &client,
                               const String &method,
                               const String &url,
                               const String &extraHeaders,
                               String &responseText,
                               int &statusCode) {
  static uint32_t cseq = 1;
  String request = method + " " + url + " RTSP/1.0\r\n";
  request += "CSeq: " + String(cseq++) + "\r\n";
  request += "User-Agent: esp32-gardepro-bridge/0.1\r\n";
  request += "Accept: application/sdp\r\n";
  if (!extraHeaders.isEmpty()) {
    request += extraHeaders;
    if (!extraHeaders.endsWith("\r\n")) {
      request += "\r\n";
    }
  }
  request += "\r\n";

  responseText = "";
  client.print(request);

  unsigned long start = millis();
  unsigned long lastData = millis();
  while (millis() - start < 4000) {
    while (client.available()) {
      responseText += client.readString();
      lastData = millis();
    }
    if (!client.connected() && !client.available()) {
      break;
    }
    if (responseText.length() > 0 && millis() - lastData > 250) {
      break;
    }
    cooperativeDelay(10);
  }

  statusCode = 0;
  const int lineEnd = responseText.indexOf("\r\n");
  const String statusLine = lineEnd >= 0 ? responseText.substring(0, lineEnd) : responseText;
  const int firstSpace = statusLine.indexOf(' ');
  const int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  if (firstSpace > 0 && secondSpace > firstSpace) {
    statusCode = statusLine.substring(firstSpace + 1, secondSpace).toInt();
  }

  Serial.printf("Camera RTSP %s %s -> %d, %u bytes\n",
                method.c_str(),
                url.c_str(),
                statusCode,
                static_cast<unsigned>(responseText.length()));
  return responseText.length() > 0;
}

bool proxyCameraRtspRequest(const String &method,
                            const String &url,
                            const String &extraHeaders,
                            String &responseText,
                            int &statusCode) {
  WiFiClient client;
  if (!client.connect(CAMERA_IP, CAMERA_RTSP_PORT)) {
    Serial.printf("Failed to connect camera RTSP for %s %s\n", method.c_str(), url.c_str());
    statusCode = 502;
    responseText = "";
    return false;
  }

  const bool ok = exchangeCameraRtspRequest(client, method, url, extraHeaders, responseText, statusCode);
  client.stop();
  return ok;
}

long parseHttpChunkSize(const String &line) {
  String chunkLine = line;
  chunkLine.trim();
  const int semicolon = chunkLine.indexOf(';');
  if (semicolon >= 0) {
    chunkLine = chunkLine.substring(0, semicolon);
  }
  if (chunkLine.isEmpty()) {
    return -1;
  }
  return strtol(chunkLine.c_str(), nullptr, 16);
}

void printBodySnippet(const String &body) {
  String snippet = body;
  snippet.replace("\r", " ");
  snippet.replace("\n", " ");
  snippet.trim();
  if (snippet.length() > 180) {
    snippet = snippet.substring(0, 180);
    snippet += "...";
  }
  Serial.printf("  body: %s\n", snippet.c_str());
}

void printFullTextBlock(const char *label, const String &text) {
  Serial.printf("=== %s (%u bytes) ===\n", label, static_cast<unsigned>(text.length()));
  Serial.print(text);
  if (!text.endsWith("\n")) {
    Serial.println();
  }
  Serial.printf("=== End %s ===\n", label);
}

String rtspHeaderValue(const String &responseText, const String &headerName) {
  const String needle = headerName + ":";
  int start = 0;
  while (start < responseText.length()) {
    int end = responseText.indexOf("\r\n", start);
    if (end < 0) {
      end = responseText.length();
    }
    String line = responseText.substring(start, end);
    if (line.startsWith(needle)) {
      line = line.substring(needle.length());
      line.trim();
      return line;
    }
    if (end >= responseText.length()) {
      break;
    }
    start = end + 2;
  }
  return "";
}

String rtspBody(const String &responseText) {
  const int headerEnd = responseText.indexOf("\r\n\r\n");
  if (headerEnd < 0) {
    return "";
  }
  return responseText.substring(headerEnd + 4);
}

String resolveRtspControlUrl(const String &describeUrl, const String &controlValue) {
  if (controlValue.isEmpty()) {
    return "";
  }
  if (controlValue.startsWith("rtsp://")) {
    return controlValue;
  }
  if (controlValue == "*") {
    return describeUrl;
  }
  if (controlValue.startsWith("/")) {
    const int scheme = describeUrl.indexOf("://");
    const int hostStart = scheme >= 0 ? scheme + 3 : 0;
    const int pathStart = describeUrl.indexOf('/', hostStart);
    if (pathStart < 0) {
      return describeUrl + controlValue;
    }
    return describeUrl.substring(0, pathStart) + controlValue;
  }

  const int lastSlash = describeUrl.lastIndexOf('/');
  if (lastSlash < 0) {
    return describeUrl + "/" + controlValue;
  }
  return describeUrl.substring(0, lastSlash + 1) + controlValue;
}

bool parseRtspDescribe(const String &describeUrl, const String &responseText, RtspSessionInfo &info) {
  info.describeUrl = describeUrl;
  info.aggregateControlUrl = describeUrl;
  info.mediaControlUrl = "";

  const String sdp = rtspBody(responseText);
  int start = 0;
  while (start <= sdp.length()) {
    int end = sdp.indexOf('\n', start);
    if (end < 0) {
      end = sdp.length();
    }
    String line = sdp.substring(start, end);
    line.replace("\r", "");
    line.trim();
    if (line.startsWith("a=control:")) {
      String value = line.substring(10);
      value.trim();
      const String resolved = resolveRtspControlUrl(describeUrl, value);
      if (value == "*" || resolved == describeUrl) {
        info.aggregateControlUrl = resolved;
      } else if (info.mediaControlUrl.isEmpty()) {
        info.mediaControlUrl = resolved;
      }
    }
    if (end >= sdp.length()) {
      break;
    }
    start = end + 1;
  }

  if (info.mediaControlUrl.isEmpty()) {
    info.mediaControlUrl = describeUrl;
  }
  return true;
}

bool runRtspLiveSequence(RtspSessionInfo &info) {
  info.describeStatus = 0;
  info.setupStatus = 0;
  info.playStatus = 0;
  info.describeResponse = "";
  info.setupResponse = "";
  info.playResponse = "";
  info.sessionHeader = "";
  info.describeUrl = "rtsp://192.168.8.1/live.sdp";
  info.aggregateControlUrl = info.describeUrl;
  info.mediaControlUrl = "";

  if (!wifiConnected) {
    Serial.println("[rtsp-live] camera WiFi is down");
    return false;
  }

  closeRtspSession();
  if (!cameraRtspClient.connect(CAMERA_IP, CAMERA_RTSP_PORT)) {
    Serial.println("[rtsp-live] failed to connect RTSP socket");
    return false;
  }

  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "DESCRIBE",
                                 info.describeUrl,
                                 "",
                                 info.describeResponse,
                                 info.describeStatus)) {
    Serial.println("[rtsp-live] DESCRIBE failed");
    closeRtspSession();
    return false;
  }
  printBodySnippet(info.describeResponse);
  printFullTextBlock("RTSP DESCRIBE", info.describeResponse);
  if (info.describeStatus != 200) {
    Serial.printf("[rtsp-live] DESCRIBE status=%d\n", info.describeStatus);
    return false;
  }

  parseRtspDescribe(info.describeUrl, info.describeResponse, info);
  lastStreamSdp = rtspBody(info.describeResponse);
  Serial.printf("[rtsp-live] aggregate=%s media=%s\n",
                info.aggregateControlUrl.c_str(),
                info.mediaControlUrl.c_str());

  String setupHeaders = "Transport: RTP/AVP;unicast;client_port=";
  setupHeaders += String(LOCAL_MEDIA_PORT_PRIMARY);
  setupHeaders += "-";
  setupHeaders += String(LOCAL_MEDIA_PORT_SECONDARY);
  setupHeaders += "\r\n";
  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "SETUP",
                                 info.mediaControlUrl,
                                 setupHeaders,
                                 info.setupResponse,
                                 info.setupStatus)) {
    Serial.println("[rtsp-live] SETUP failed");
    closeRtspSession();
    return false;
  }
  printBodySnippet(info.setupResponse);
  printFullTextBlock("RTSP SETUP", info.setupResponse);
  if (info.setupStatus != 200) {
    Serial.printf("[rtsp-live] SETUP status=%d\n", info.setupStatus);
    return false;
  }

  info.sessionHeader = rtspHeaderValue(info.setupResponse, "Session");
  const int semicolon = info.sessionHeader.indexOf(';');
  if (semicolon > 0) {
    info.sessionHeader = info.sessionHeader.substring(0, semicolon);
  }
  info.sessionHeader.trim();
  if (info.sessionHeader.isEmpty()) {
    Serial.println("[rtsp-live] SETUP missing Session header");
    return false;
  }
  lastRtspSessionId = info.sessionHeader;
  Serial.printf("[rtsp-live] session=%s\n", info.sessionHeader.c_str());

  String playHeaders = "Session: " + info.sessionHeader + "\r\n";
  playHeaders += "Range: npt=0.000-\r\n";
  String playUrl = info.aggregateControlUrl;
  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "PLAY",
                                 playUrl,
                                 playHeaders,
                                 info.playResponse,
                                 info.playStatus)) {
    Serial.println("[rtsp-live] PLAY failed");
    closeRtspSession();
    return false;
  }
  printBodySnippet(info.playResponse);
  printFullTextBlock("RTSP PLAY", info.playResponse);
  if (info.playStatus == 455 && info.mediaControlUrl != info.aggregateControlUrl) {
    Serial.printf("[rtsp-live] retry PLAY on media URL %s\n", info.mediaControlUrl.c_str());
    playUrl = info.mediaControlUrl;
    info.playResponse = "";
    if (!exchangeCameraRtspRequest(cameraRtspClient,
                                   "PLAY",
                                   playUrl,
                                   playHeaders,
                                   info.playResponse,
                                   info.playStatus)) {
      Serial.println("[rtsp-live] PLAY retry failed");
      closeRtspSession();
      return false;
    }
    printBodySnippet(info.playResponse);
    printFullTextBlock("RTSP PLAY RETRY", info.playResponse);
  }
  if (info.playStatus == 200) {
    rtspSessionOpen = true;
    rtspPlayUrl = playUrl;
    rtspSessionHeader = info.sessionHeader;
    lastRtspKeepaliveMs = millis();
    lastBleKeepaliveMs = millis();
    lastHttpKeepaliveMs = millis();
    lastPrimaryPacketMs = millis();
    lastSecondaryPacketMs = millis();
  } else {
    closeRtspSession();
  }
  Serial.printf("[rtsp-live] PLAY status=%d url=%s\n", info.playStatus, playUrl.c_str());
  return info.playStatus == 200;
}

bool startStreamSession() {
  if (streamSessionActive) {
    Serial.println("[stream] session already active");
    return true;
  }
  if (!halowConnected) {
    Serial.println("[stream] HaLow is down, connecting now");
    connectHaLow();
  }
  if (!halowConnected) {
    Serial.println("[stream] HaLow connect failed");
    return false;
  }
  if (!wifiConnected) {
    Serial.println("[stream] camera WiFi is down");
    return false;
  }

  RtspSessionInfo info{};
  if (!runRtspLiveSequence(info)) {
    Serial.println("[stream] RTSP live sequence failed");
    return false;
  }

  if (!connectTunnelSocket()) {
    Serial.println("[stream] tunnel connect failed");
    return false;
  }

  startHttpServer();
  streamSessionActive = true;
  streamSessionStartedMs = millis();
  Serial.printf("[stream] session active started_ms=%lu\n", streamSessionStartedMs);
  return true;
}

void runCameraHttpSelfTest() {
  static const CameraProbeTarget targets[] = {
    {"para_setting", "/cmd/getParaSetting"},
    {"info_1", "/cmd/info/1"},
    {"info_2", "/cmd/info/2"},
    {"info_3", "/cmd/info/3"},
    {"info_4", "/cmd/info/4"},
    {"info_5", "/cmd/info/5"},
    {"info_6", "/cmd/info/6"},
    {"gallery", "/list/detail/backward/900000/60"},
    {"ir_status", "/media/getIrStatus"},
  };

  Serial.println("=== Camera HTTP self-test without BLE ===");
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
    String body;
    int statusCode = 0;
    const bool ok = proxyCameraRequest("GET", targets[i].path, body, statusCode);
    Serial.printf("[%s] ok=%s status=%d bytes=%u\n",
                  targets[i].label,
                  ok ? "yes" : "no",
                  statusCode,
                  (unsigned)body.length());
    if (body.length() > 0) {
      printBodySnippet(body);
    }
    cooperativeDelay(250);
  }
  Serial.println("=== End camera HTTP self-test ===");
}

bool runBringupSequence() {
  WiFi.disconnect(true, true);
  cooperativeDelay(250);
  refreshWifiState();
  bleWakeAttempted = true;
  bool hotspotVisible = false;

  if (tryExistingBleWakeSession()) {
    hotspotVisible = waitForCameraWifiPresence(BRINGUP_HOTSPOT_WAIT_MS,
                                               BRINGUP_HOTSPOT_POLL_MS);
    Serial.printf("[WiFi] hotspot visibility after cached BLE wake: %s\n",
                  hotspotVisible ? "yes" : "no");
    if (hotspotVisible) {
      bleWakeConfirmed = true;
    } else {
      Serial.println("[bringup] cached BLE wake did not restore hotspot, falling back to full scan");
    }
  }

  if (!hotspotVisible) {
    closeBleWakeSession();
    bleWakeConfirmed = runExactBleWake();
    Serial.printf("[BLE] exact wake result: %s stage=%s\n",
                  bleWakeConfirmed ? "success" : "no-confirmation",
                  bleStage.c_str());
    if (!bleWakeConfirmed) {
      Serial.println("[bringup] aborting after BLE wake failure");
      refreshWifiState();
      return false;
    }
    hotspotVisible = waitForCameraWifiPresence(BRINGUP_HOTSPOT_WAIT_MS,
                                               BRINGUP_HOTSPOT_POLL_MS);
    Serial.printf("[WiFi] hotspot visibility after BLE wake: %s\n", hotspotVisible ? "yes" : "no");
  }
  if (!hotspotVisible) {
    Serial.println("[bringup] aborting because camera hotspot did not appear");
    refreshWifiState();
    return false;
  }
  connectCameraWifi();
  if (RUN_LOCAL_SERIAL_TEST) {
    Serial.println("[BLE] keeping wake session open in local serial mode");
  } else {
    closeBleWakeSession();
  }
  refreshWifiState();
  return wifiConnected;
}

void printRuntimeStatus() {
  refreshWifiState();
  Serial.printf("[status] uptime_ms=%lu boot_count=%u boot_session_id=%u\n",
                millis(),
                persistentBootCount,
                bootSessionId);
  Serial.printf("[status] wifi=%s ip=%s ble_stage=%s ble_ok=%s notify_count=%u last=%s\n",
                wifiConnected ? "up" : "down",
                WiFi.localIP().toString().c_str(),
                bleStage.c_str(),
                bleWakeConfirmed ? "yes" : "no",
                bleNotifyCount,
                bleLastNotifyText.c_str());
  Serial.printf("[status] halow=%s status=%d ip=%s gw=%s ssid=%s bssid=%s rssi=%d last_event=%d age_ms=%lu event_count=%u\n",
                halowConnected ? "up" : "down",
                static_cast<int>(HaLow.status()),
                HaLow.localIP().toString().c_str(),
                HaLow.gatewayIP().toString().c_str(),
                HaLow.SSID().c_str(),
                HaLow.BSSIDstr().c_str(),
                static_cast<int>(HaLow.RSSI()),
                halowLastEventId,
                msSince(halowLastEventMs),
                halowEventCount);
  Serial.printf("[status] udp_primary port=%u packets=%u bytes=%u last_src=%s:%u last_len=%u\n",
                udpPrimaryStats.localPort,
                udpPrimaryStats.packets,
                udpPrimaryStats.bytes,
                udpPrimaryStats.lastSourceIp.toString().c_str(),
                udpPrimaryStats.lastSourcePort,
                static_cast<unsigned>(udpPrimaryStats.lastPacketLen));
  Serial.printf("[status] udp_secondary port=%u packets=%u bytes=%u last_src=%s:%u last_len=%u\n",
                udpSecondaryStats.localPort,
                udpSecondaryStats.packets,
                udpSecondaryStats.bytes,
                udpSecondaryStats.lastSourceIp.toString().c_str(),
                udpSecondaryStats.lastSourcePort,
                static_cast<unsigned>(udpSecondaryStats.lastPacketLen));
  Serial.printf("[status] stream=%s tunnel_connected=%s tunnel_pkts=%u tunnel_bytes=%u tunnel_failures=%u last_connect_ms=%lu recoveries=%u active_ms=%lu since_stop_ms=%lu primary_gap_ms=%lu secondary_gap_ms=%lu\n",
                streamSessionActive ? "active" : "idle",
                getTunnelSocketSnapshot() >= 0 ? "yes" : "no",
                tunnelPacketsSent,
                tunnelBytesSent,
                tunnelSendFailures,
                tunnelLastConnectMs,
                streamRecoveryAttempts,
                msSince(streamSessionStartedMs),
                msSince(lastStreamStopMs),
                msSince(lastPrimaryPacketMs),
                msSince(lastSecondaryPacketMs));
  Serial.printf("[status] idle_recoveries=%u http_keepalive_failures=%u idle_recovery_age_ms=%lu\n",
                idleRecoveryAttempts,
                httpKeepaliveFailures,
                msSince(lastIdleRecoveryMs));
}

void runHttpPathFromSerial(const String &path) {
  runHttpMethodFromSerial("GET", path);
}

void runHttpMethodFromSerial(const String &method, const String &path) {
  if (!wifiConnected) {
    Serial.println("[http] camera WiFi is down");
    return;
  }

  String body;
  int statusCode = 0;
  const bool ok = proxyCameraRequest(method, path, body, statusCode);
  Serial.printf("[http] method=%s path=%s ok=%s status=%d bytes=%u\n",
                method.c_str(),
                path.c_str(),
                ok ? "yes" : "no",
                statusCode,
                static_cast<unsigned>(body.length()));
  if (body.length() > 0) {
    printBodySnippet(body);
  }
}

void runRtspMethodFromSerial(const String &method, const String &url) {
  if (!wifiConnected) {
    Serial.println("[rtsp] camera WiFi is down");
    return;
  }

  String response;
  int statusCode = 0;
  const bool ok = proxyCameraRtspRequest(method, url, "", response, statusCode);
  Serial.printf("[rtsp] method=%s url=%s ok=%s status=%d bytes=%u\n",
                method.c_str(),
                url.c_str(),
                ok ? "yes" : "no",
                statusCode,
                static_cast<unsigned>(response.length()));
  if (response.length() > 0) {
    printBodySnippet(response);
  }
}

void runRtspProbeSequence() {
  static const char *urls[] = {
    "*",
    "rtsp://192.168.8.1/live.sdp",
    "rtsp://192.168.8.1/",
    "rtsp://192.168.8.1/wifi/live",
  };

  Serial.println("=== RTSP probe sequence ===");
  for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) {
    runRtspMethodFromSerial("OPTIONS", urls[i]);
    cooperativeDelay(200);
  }
  for (size_t i = 1; i < sizeof(urls) / sizeof(urls[0]); ++i) {
    runRtspMethodFromSerial("DESCRIBE", urls[i]);
    cooperativeDelay(200);
  }
  Serial.println("=== End RTSP probe sequence ===");
}

void handleStatus() {
  ControlState controlSnapshot{};
  snapshotControlState(controlSnapshot);
  String payload = "{";
  payload += "\"uptime_ms\":" + String(millis());
  payload += ",\"boot_count\":" + String(persistentBootCount);
  payload += ",\"boot_session_id\":" + String(bootSessionId);
  payload += ",\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  payload += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
  payload += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"halow_mac\":\"" + HaLow.macAddress() + "\"";
  payload += ",\"camera_ip\":\"" + CAMERA_IP.toString() + "\"";
  payload += ",\"camera_wifi_ever_connected\":" + String(cameraWifiEverConnected ? "true" : "false");
  payload += ",\"standby_requested\":" + String(standbyRequested ? "true" : "false");
  payload += ",\"stream_active\":" + String(streamSessionActive ? "true" : "false");
  payload += ",\"tunnel_connected\":" + String(getTunnelSocketSnapshot() >= 0 ? "true" : "false");
  payload += ",\"recoveries\":" + String(streamRecoveryAttempts);
  payload += ",\"idle_recoveries\":" + String(idleRecoveryAttempts);
  payload += ",\"http_keepalive_failures\":" + String(httpKeepaliveFailures);
  payload += ",\"idle_recovery_last_ms\":" + String(msSince(lastIdleRecoveryMs));
  payload += ",\"media_primary_packets\":" + String(mediaPrimaryPackets);
  payload += ",\"media_primary_bytes\":" + String(mediaPrimaryBytes);
  payload += ",\"media_secondary_packets\":" + String(mediaSecondaryPackets);
  payload += ",\"media_secondary_bytes\":" + String(mediaSecondaryBytes);
  payload += ",\"battery\":" + buildBatteryJson();
  payload += ",\"onboard_camera\":" + buildOnboardCameraStatusJson();
  payload += ",\"wifi_scan_busy\":" + String(wifiScanBusy ? "true" : "false");
  payload += ",\"wifi_scan_last_count\":" + String(wifiScanLastCount);
  payload += ",\"wifi_scan_last_age_ms\":" + String(msSince(wifiScanLastMs));
  payload += ",\"ble_wake_attempted\":" + String(bleWakeAttempted ? "true" : "false");
  payload += ",\"ble_wake_confirmed\":" + String(bleWakeConfirmed ? "true" : "false");
  payload += ",\"ble_stage\":\"" + jsonEscape(bleStage) + "\"";
  payload += ",\"ble_scan_mode\":\"" + jsonEscape(bleScanMode) + "\"";
  payload += ",\"ble_scan_results\":" + String(bleScanResultCount);
  payload += ",\"ble_scan_attempts\":" + String(bleScanAttemptCounter);
  payload += ",\"ble_target_seen_count\":" + String(bleTargetSeenCount);
  payload += ",\"ble_last_seen_mac\":\"" + jsonEscape(bleLastSeenMac) + "\"";
  payload += ",\"ble_last_seen_name\":\"" + jsonEscape(bleLastSeenName) + "\"";
  payload += ",\"ble_last_seen_rssi\":" + String(bleLastSeenRssi);
  payload += ",\"ble_best_seen_mac\":\"" + jsonEscape(bleBestSeenMac) + "\"";
  payload += ",\"ble_best_seen_name\":\"" + jsonEscape(bleBestSeenName) + "\"";
  payload += ",\"ble_best_seen_rssi\":" + String(bleBestSeenRssi);
  payload += ",\"ble_recent_devices\":" + buildBleRecentDevicesJson();
  payload += ",\"ble_notify_count\":" + String(bleNotifyCount);
  payload += ",\"ble_last_notify\":\"" + jsonEscape(bleLastNotifyText) + "\"";
  payload += ",\"control_busy\":" + String(controlSnapshot.busy ? "true" : "false");
  payload += ",\"control_pending\":\"" + String(controlActionName(controlSnapshot.pendingAction)) + "\"";
  payload += ",\"control_action\":\"" + String(controlActionName(controlSnapshot.activeAction)) + "\"";
  payload += ",\"control_last_action\":\"" + String(controlActionName(controlSnapshot.lastAction)) + "\"";
  payload += ",\"control_last_ok\":" + String(controlSnapshot.lastOk ? "true" : "false");
  payload += ",\"control_active_ms\":" + String(msSince(controlSnapshot.activeSinceMs));
  payload += ",\"control_last_finished_ms\":" + String(msSince(controlSnapshot.lastFinishedMs));
  payload += ",\"control_last_message\":\"" + jsonEscape(String(controlSnapshot.lastMessage)) + "\"";
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleBatteryStatus() {
  server.send(200, "application/json", buildBatteryJson());
}

void handleOnboardCameraStatus() {
  server.send(200, "application/json", buildOnboardCameraStatusJson());
}

void handleOnboardCameraLatest() {
  if (!onboardCameraReady) {
    server.send(503, "application/json", "{\"error\":\"onboard_camera_not_ready\"}");
    return;
  }

  if (server.hasArg("capture") && server.arg("capture") == "1") {
    captureOnboardFrame();
  }

  uint8_t *buf = nullptr;
  size_t len = 0;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreTake(onboardFrameMutex, portMAX_DELAY);
  }
  buf = onboardLatestJpeg;
  len = onboardLatestJpegLen;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreGive(onboardFrameMutex);
  }

  if (buf == nullptr || len == 0) {
    server.send(404, "application/json", "{\"error\":\"no_onboard_capture\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", reinterpret_cast<const char *>(buf), len);
}

void handleOnboardCameraCapture() {
  if (!onboardCameraReady) {
    server.send(503, "application/json", "{\"error\":\"onboard_camera_not_ready\"}");
    return;
  }
  const bool ok = captureOnboardFrame();
  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  payload += ",\"onboard_camera\":" + buildOnboardCameraStatusJson();
  payload += "}";
  server.send(ok ? 200 : 500, "application/json", payload);
}

void handleOnboardCameraConfig() {
  if (server.hasArg("enabled")) {
    const String enabled = server.arg("enabled");
    onboardCaptureEnabled = enabled == "1" || enabled == "true" || enabled == "yes";
  }
  if (server.hasArg("interval_ms")) {
    const unsigned long requested = server.arg("interval_ms").toInt();
    if (requested >= 5000) {
      onboardCaptureIntervalMs = requested;
    }
  }
  server.send(200, "application/json", buildOnboardCameraStatusJson());
}

void handleWifiScan() {
  const bool force = server.hasArg("force") && server.arg("force") == "1";
  if (!force && wifiScanLastMs != 0 && millis() - wifiScanLastMs < WIFI_SCAN_CACHE_MS) {
    String cached = wifiScanLastJson;
    cached.replace("\"age_ms\":0", "\"age_ms\":" + String(msSince(wifiScanLastMs)));
    server.send(200, "application/json", cached);
    return;
  }

  String payload;
  String error;
  if (!runIdleWifiScan(payload, error)) {
    String body = "{";
    body += "\"error\":\"" + jsonEscape(error) + "\"";
    body += ",\"hint\":\"scan requires idle trail-camera WiFi\"";
    body += "}";
    server.send(409, "application/json", body);
    return;
  }
  server.send(200, "application/json", payload);
}

void handleCameraRawGet() {
  if (!wifiConnected) {
    server.send(503, "application/json", "{\"error\":\"camera_wifi_down\"}");
    return;
  }

  const String path = server.arg("path");
  if (path.isEmpty() || !path.startsWith("/")) {
    server.send(400, "application/json", "{\"error\":\"missing_path\"}");
    return;
  }

  WiFiClient client;
  if (!client.connect(CAMERA_IP, CAMERA_HTTP_PORT)) {
    server.send(502, "application/json", "{\"error\":\"camera_connect_failed\"}");
    return;
  }

  client.print("GET " + path + " HTTP/1.1\r\n" +
               "Host: " + CAMERA_IP.toString() + ":" + String(CAMERA_HTTP_PORT) + "\r\n" +
               "User-Agent: esp32-gardepro-bridge/0.1\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (client.connected() && !client.available()) {
    if (millis() - timeout > 5000) {
      client.stop();
      Serial.printf("[raw] timeout waiting for headers path=%s\n", path.c_str());
      server.send(504, "application/json", "{\"error\":\"camera_timeout\"}");
      return;
    }
    cooperativeDelay(10);
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  int statusCode = 502;
  const int firstSpace = statusLine.indexOf(' ');
  const int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  if (firstSpace > 0 && secondSpace > firstSpace) {
    statusCode = statusLine.substring(firstSpace + 1, secondSpace).toInt();
  }

  String contentType = "application/octet-stream";
  int contentLength = -1;
  bool transferChunked = false;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) {
      break;
    }
    if (line.startsWith("Content-Type:")) {
      contentType = line.substring(13);
      contentType.trim();
    } else if (line.startsWith("Content-Length:")) {
      String value = line.substring(15);
      value.trim();
      contentLength = value.toInt();
    } else if (line.startsWith("Transfer-Encoding:")) {
      String value = line.substring(18);
      value.trim();
      value.toLowerCase();
      transferChunked = value.indexOf("chunked") >= 0;
    }
  }

  WiFiClient out = server.client();
  const char *reasonPhrase = "OK";
  switch (statusCode) {
    case 200:
      reasonPhrase = "OK";
      break;
    case 400:
      reasonPhrase = "Bad Request";
      break;
    case 401:
      reasonPhrase = "Unauthorized";
      break;
    case 403:
      reasonPhrase = "Forbidden";
      break;
    case 404:
      reasonPhrase = "Not Found";
      break;
    case 500:
      reasonPhrase = "Internal Server Error";
      break;
    case 502:
      reasonPhrase = "Bad Gateway";
      break;
    default:
      reasonPhrase = "Camera Response";
      break;
  }

  out.printf("HTTP/1.1 %d %s\r\n", statusCode, reasonPhrase);
  out.print("Content-Type: " + contentType + "\r\n");
  if (!transferChunked && contentLength >= 0) {
    out.print("Content-Length: " + String(contentLength) + "\r\n");
  }
  out.print("Connection: close\r\n\r\n");

  size_t totalWritten = 0;
  uint8_t buf[1024];
  if (transferChunked) {
    while (client.connected() || client.available()) {
      String sizeLine = client.readStringUntil('\n');
      if (sizeLine.isEmpty() && !client.connected() && !client.available()) {
        break;
      }
      const long chunkSize = parseHttpChunkSize(sizeLine);
      if (chunkSize < 0) {
        Serial.printf("[raw] invalid chunk header path=%s line=%s\n", path.c_str(), sizeLine.c_str());
        break;
      }
      if (chunkSize == 0) {
        while (client.connected()) {
          String trailer = client.readStringUntil('\n');
          trailer.trim();
          if (trailer.isEmpty()) {
            break;
          }
        }
        break;
      }

      long remaining = chunkSize;
      while (remaining > 0) {
        if (!out.connected()) {
          Serial.printf("[raw] downstream disconnected path=%s written=%u\n",
                        path.c_str(),
                        static_cast<unsigned>(totalWritten));
          remaining = 0;
          break;
        }
        const size_t toRead = remaining < static_cast<long>(sizeof(buf))
                                ? static_cast<size_t>(remaining)
                                : sizeof(buf);
        const int n = client.readBytes(buf, toRead);
        if (n <= 0) {
          Serial.printf("[raw] chunk read failed path=%s remaining=%ld\n",
                        path.c_str(),
                        remaining);
          remaining = 0;
          break;
        }
        const size_t written = out.write(buf, static_cast<size_t>(n));
        totalWritten += written;
        if (written != static_cast<size_t>(n)) {
          Serial.printf("[raw] short write path=%s read=%d wrote=%u total=%u\n",
                        path.c_str(),
                        n,
                        static_cast<unsigned>(written),
                        static_cast<unsigned>(totalWritten));
          remaining = 0;
          break;
        }
        remaining -= n;
      }

      client.read();
      client.read();
    }
  } else {
    while (client.connected() || client.available()) {
      const size_t available = client.available();
      if (available == 0) {
        if (!out.connected()) {
          Serial.printf("[raw] downstream disconnected path=%s written=%u\n",
                        path.c_str(),
                        static_cast<unsigned>(totalWritten));
          break;
        }
        delay(1);
        continue;
      }
      const size_t toRead = available < sizeof(buf) ? available : sizeof(buf);
      const int n = client.read(buf, toRead);
      if (n > 0) {
        const size_t written = out.write(buf, static_cast<size_t>(n));
        totalWritten += written;
        if (written != static_cast<size_t>(n)) {
          Serial.printf("[raw] short write path=%s read=%d wrote=%u total=%u\n",
                        path.c_str(),
                        n,
                        static_cast<unsigned>(written),
                        static_cast<unsigned>(totalWritten));
          break;
        }
      }
    }
  }
  Serial.printf("[raw] path=%s status=%d type=%s len=%d streamed=%u\n",
                path.c_str(),
                statusCode,
                contentType.c_str(),
                contentLength,
                static_cast<unsigned>(totalWritten));
  client.stop();
}

void handleControlBringup() {
  String message;
  const bool accepted = queueControlAction(CONTROL_ACTION_BRINGUP, message);
  String payload = "{";
  payload += "\"ok\":" + String(accepted ? "true" : "false");
  payload += ",\"accepted\":" + String(accepted ? "true" : "false");
  payload += ",\"action\":\"bringup\"";
  payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  payload += "}";
  server.send(accepted ? 202 : 409, "application/json", payload);
}

void handleControlStreamStart() {
  String message;
  const bool accepted = queueControlAction(CONTROL_ACTION_STREAM_START, message);
  String payload = "{";
  payload += "\"ok\":" + String(accepted ? "true" : "false");
  payload += ",\"accepted\":" + String(accepted ? "true" : "false");
  payload += ",\"action\":\"stream_start\"";
  payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  payload += "}";
  server.send(accepted ? 202 : 409, "application/json", payload);
}

void handleControlStreamStop() {
  String message;
  const bool accepted = queueControlAction(CONTROL_ACTION_STREAM_STOP, message);
  String payload = "{";
  payload += "\"ok\":" + String(accepted ? "true" : "false");
  payload += ",\"accepted\":" + String(accepted ? "true" : "false");
  payload += ",\"action\":\"stream_stop\"";
  payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  payload += "}";
  server.send(accepted ? 202 : 409, "application/json", payload);
}

void handleCameraRequest() {
  const String method = server.arg("method").isEmpty() ? "GET" : server.arg("method");
  const String path = server.arg("path");
  const String contentType = server.arg("content_type");
  const String requestBody = server.hasArg("plain") ? server.arg("plain") : server.arg("body");
  if (!wifiConnected) {
    server.send(503, "application/json", "{\"error\":\"camera_wifi_down\"}");
    return;
  }
  if (path.isEmpty() || !path.startsWith("/")) {
    server.send(400, "application/json", "{\"error\":\"missing_path\"}");
    return;
  }
  String body;
  int statusCode = 500;
  String normalizedMethod = method;
  normalizedMethod.toUpperCase();
  proxyCameraRequest(normalizedMethod, path, requestBody, contentType, body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleCameraInfo1() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/info/1", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleCameraInfo2() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/info/2", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleParaSettings() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/getParaSetting", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleGalleryList() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/list/detail/backward/900000/60", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleStandbyReset() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/standby/reset", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleCameraInfo3() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/info/3", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleCameraInfo4() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/info/4", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleCameraInfo5() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/info/5", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void handleCameraInfo6() {
  String body;
  int statusCode = 500;
  proxyCameraRequest("GET", "/cmd/info/6", body, statusCode);
  server.send(statusCode, "application/json", body);
}

void controlWorkerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    const ControlAction action = takePendingControlAction();
    if (action == CONTROL_ACTION_NONE) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    bool ok = false;
    const char *message = "unsupported";
    if (action == CONTROL_ACTION_BRINGUP) {
      ok = runBringupSequence();
      message = ok ? "bringup_complete" : "bringup_failed";
    } else if (action == CONTROL_ACTION_STREAM_START) {
      ok = startStreamSession();
      message = ok ? "stream_active" : "stream_start_failed";
    } else if (action == CONTROL_ACTION_STREAM_STOP) {
      stopTunnelSession("http_control_stop");
      ok = true;
      message = "stream_stopped";
    }

    finishControlAction(action, ok, message);
  }
}

void startHttpServer() {
  if (httpServerStarted) {
    return;
  }
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/camera/raw", HTTP_GET, handleCameraRawGet);
  server.on("/camera/request", HTTP_GET, handleCameraRequest);
  server.on("/camera/request", HTTP_POST, handleCameraRequest);
  server.on("/control/bringup", HTTP_POST, handleControlBringup);
  server.on("/control/stream_start", HTTP_POST, handleControlStreamStart);
  server.on("/control/stream_stop", HTTP_POST, handleControlStreamStop);
  server.on("/battery/status", HTTP_GET, handleBatteryStatus);
  server.on("/onboard/status", HTTP_GET, handleOnboardCameraStatus);
  server.on("/onboard/latest.jpg", HTTP_GET, handleOnboardCameraLatest);
  server.on("/onboard/capture", HTTP_POST, handleOnboardCameraCapture);
  server.on("/onboard/config", HTTP_POST, handleOnboardCameraConfig);
  server.on("/scan/wifi", HTTP_GET, handleWifiScan);
  server.on("/camera/info/1", HTTP_GET, handleCameraInfo1);
  server.on("/camera/info/2", HTTP_GET, handleCameraInfo2);
  server.on("/camera/info/3", HTTP_GET, handleCameraInfo3);
  server.on("/camera/info/4", HTTP_GET, handleCameraInfo4);
  server.on("/camera/info/5", HTTP_GET, handleCameraInfo5);
  server.on("/camera/info/6", HTTP_GET, handleCameraInfo6);
  server.on("/camera/getParaSetting", HTTP_GET, handleParaSettings);
  server.on("/camera/gallery", HTTP_GET, handleGalleryList);
  server.on("/camera/standby/reset", HTTP_GET, handleStandbyReset);
  server.begin();
  httpServerStarted = true;
  Serial.printf("Bridge HTTP server on port %u\n", BRIDGE_HTTP_PORT);
}

void udpInspectTask(void *pvParameters) {
  UdpInspectorStats *stats = reinterpret_cast<UdpInspectorStats *>(pvParameters);
  const bool primary = stats->localPort == LOCAL_MEDIA_PORT_PRIMARY;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    Serial.printf("[udp-%s] socket create failed on port %u\n", stats->label, stats->localPort);
    vTaskDelete(nullptr);
    return;
  }

  sockaddr_in bindAddr{};
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  bindAddr.sin_port = htons(stats->localPort);
  if (bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0) {
    Serial.printf("[udp-%s] bind failed on port %u\n", stats->label, stats->localPort);
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("[udp-%s] listening on local port %u\n", stats->label, stats->localPort);

  uint8_t buf[1600];
  while (true) {
    sockaddr_in srcAddr{};
    socklen_t srcLen = sizeof(srcAddr);
    const int len = recvfrom(sock, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr *>(&srcAddr), &srcLen);
    if (len <= 0) {
      delay(10);
      continue;
    }

    stats->packets++;
    stats->bytes += len;
    stats->lastSourceIp = IPAddress(ntohl(srcAddr.sin_addr.s_addr));
    stats->lastSourcePort = ntohs(srcAddr.sin_port);
    stats->lastPacketLen = static_cast<size_t>(len);
    if (primary) {
      lastPrimaryPacketMs = millis();
    } else {
      lastSecondaryPacketMs = millis();
    }

    if (shouldLogUdpPacket(stats->packets)) {
      Serial.printf("[udp-%s] packet %u len=%d from %s:%u first=",
                    stats->label,
                    stats->packets,
                    len,
                    stats->lastSourceIp.toString().c_str(),
                    stats->lastSourcePort);
      const int preview = len < 16 ? len : 16;
      for (int i = 0; i < preview; ++i) {
        Serial.printf("%02X", buf[i]);
        if (i + 1 < preview) {
          Serial.print(" ");
        }
      }
      Serial.println();
    }

    if (streamSessionActive) {
      uint8_t flags = primary ? 0x01 : 0x02;
      if (primary && len >= 2 && (buf[1] & 0x80) != 0) {
        flags |= 0x04;
      }
      sendTunnelMediaPacket(primary ? STREAM_ID_VIDEO_RTP : STREAM_ID_VIDEO_RTCP,
                            flags,
                            buf,
                            static_cast<size_t>(len));
    }
  }
}

void startLocalUdpInspectors() {
  if (udpInspectorsStarted) {
    return;
  }
  udpInspectorsStarted = true;
  xTaskCreatePinnedToCore(udpInspectTask, "udp-inspect-primary", 4096, &udpPrimaryStats, 1, nullptr, 0);
  xTaskCreatePinnedToCore(udpInspectTask, "udp-inspect-secondary", 4096, &udpSecondaryStats, 1, nullptr, 0);
}

void udpForwardTask(void *pvParameters) {
  const bool primary = reinterpret_cast<uintptr_t>(pvParameters) == 0;
  const uint16_t localPort = primary ? LOCAL_MEDIA_PORT_PRIMARY : LOCAL_MEDIA_PORT_SECONDARY;

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    Serial.printf("UDP socket create failed on port %u\n", localPort);
    vTaskDelete(nullptr);
    return;
  }

  sockaddr_in bindAddr{};
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  bindAddr.sin_port = htons(localPort);
  if (bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0) {
    Serial.printf("UDP bind failed on port %u\n", localPort);
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("UDP forwarder listening on %u for tunnel stream_id=%u\n",
                localPort,
                primary ? STREAM_ID_VIDEO_RTP : STREAM_ID_VIDEO_RTCP);

  uint8_t buf[1600];
  while (true) {
    sockaddr_in srcAddr{};
    socklen_t srcLen = sizeof(srcAddr);
    int len = recvfrom(sock, buf, sizeof(buf), 0,
                       reinterpret_cast<sockaddr *>(&srcAddr), &srcLen);
    if (len <= 0) {
      delay(10);
      continue;
    }

    if (primary) {
      mediaPrimaryPackets++;
      mediaPrimaryBytes += len;
    } else {
      mediaSecondaryPackets++;
      mediaSecondaryBytes += len;
    }

    if (!streamSessionActive) {
      continue;
    }

    uint8_t flags = primary ? 0x01 : 0x02;
    if (primary && len >= 2 && (buf[1] & 0x80) != 0) {
      flags |= 0x04;
    }
    sendTunnelMediaPacket(primary ? STREAM_ID_VIDEO_RTP : STREAM_ID_VIDEO_RTCP,
                          flags,
                          buf,
                          static_cast<size_t>(len));
  }
}

void handleSerialCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  if (cmd.isEmpty()) {
    return;
  }

  Serial.printf("serial> %s\n", cmd.c_str());
  if (cmd == "help") {
    printSerialHelp();
    return;
  }
  if (cmd == "bringup") {
    runBringupSequence();
    return;
  }
  if (cmd == "halow_up") {
    connectHaLow();
    if (halowConnected) {
      startHttpServer();
    }
    return;
  }
  if (cmd == "status") {
    printRuntimeStatus();
    Serial.printf("[battery] %s\n", buildBatteryJson().c_str());
    Serial.printf("[onboard] %s\n", buildOnboardCameraStatusJson().c_str());
    return;
  }
  if (cmd == "battery") {
    Serial.println(buildBatteryJson());
    return;
  }
  if (cmd == "onboard_status") {
    Serial.println(buildOnboardCameraStatusJson());
    return;
  }
  if (cmd == "onboard_capture") {
    const bool ok = captureOnboardFrame();
    Serial.printf("[onboard-camera] capture=%s %s\n",
                  ok ? "ok" : "failed",
                  buildOnboardCameraStatusJson().c_str());
    return;
  }
  if (cmd == "wifi_scan") {
    String payload;
    String error;
    if (runIdleWifiScan(payload, error)) {
      Serial.println(payload);
    } else {
      Serial.printf("[wifi-scan] failed error=%s\n", error.c_str());
    }
    return;
  }
  if (cmd == "selftest") {
    runCameraHttpSelfTest();
    return;
  }
  if (cmd == "rtsp_probe") {
    runRtspProbeSequence();
    return;
  }
  if (cmd == "rtsp_live") {
    RtspSessionInfo info{};
    const bool ok = runRtspLiveSequence(info);
    Serial.printf("[rtsp-live] ok=%s describe=%d setup=%d play=%d session=%s\n",
                  ok ? "yes" : "no",
                  info.describeStatus,
                  info.setupStatus,
                  info.playStatus,
                  info.sessionHeader.c_str());
    return;
  }
  if (cmd == "stream_start") {
    startStreamSession();
    return;
  }
  if (cmd == "stream_stop") {
    stopTunnelSession("serial_stop");
    Serial.println("[stream] session stopped");
    return;
  }
  if (cmd == "wake") {
    closeBleWakeSession();
    bleWakeAttempted = true;
    bleWakeConfirmed = runExactBleWake();
    Serial.printf("[BLE] exact wake result: %s stage=%s\n",
                  bleWakeConfirmed ? "success" : "no-confirmation",
                  bleStage.c_str());
    return;
  }
  if (cmd == "bleclose") {
    closeBleWakeSession();
    bleStage = "closed";
    return;
  }
  if (cmd.startsWith("http ")) {
    runHttpPathFromSerial(cmd.substring(5));
    return;
  }
  if (cmd.startsWith("httpm ")) {
    const int split = cmd.indexOf(' ', 6);
    if (split < 0) {
      Serial.println("Usage: httpm <METHOD> <path>");
      return;
    }
    String method = cmd.substring(6, split);
    method.toUpperCase();
    const String path = cmd.substring(split + 1);
    runHttpMethodFromSerial(method, path);
    return;
  }
  if (cmd.startsWith("rtsp ")) {
    const int split = cmd.indexOf(' ', 5);
    if (split < 0) {
      Serial.println("Usage: rtsp <METHOD> <url>");
      return;
    }
    String method = cmd.substring(5, split);
    method.toUpperCase();
    const String url = cmd.substring(split + 1);
    runRtspMethodFromSerial(method, url);
    return;
  }

  Serial.println("Unknown command");
  printSerialHelp();
}

void pollSerialConsole() {
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleSerialCommand(serialCommandBuffer);
      serialCommandBuffer = "";
      continue;
    }
    if (serialCommandBuffer.length() < 255) {
      serialCommandBuffer += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("GardePro dual-radio bridge starting");
  initBootIdentity();

  pinMode(BAT_ADC_CTRL_PIN, OUTPUT);
  digitalWrite(BAT_ADC_CTRL_PIN, LOW);
  pinMode(BAT_CHRG_PIN, INPUT_PULLUP);
  pinMode(BAT_DONE_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  Serial.printf("[battery] %s\n", buildBatteryJson().c_str());

  onboardFrameMutex = xSemaphoreCreateMutex();
  initOnboardCamera();
  if (onboardCaptureTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(onboardCaptureTask,
                            "onboard-capture",
                            8192,
                            nullptr,
                            1,
                            &onboardCaptureTaskHandle,
                            1);
  }

  Serial.println("Pre-initializing HaLow transport");
  HaLow.onEvent(onHaLowEvent);
  HaLow.init(HALOW_REGION);
  halowInitialized = true;
  Serial.printf("HaLow initialized for region %s\n", HALOW_REGION);

  if (RUN_LOCAL_SERIAL_TEST) {
    Serial.println("Local serial test mode active; starting HaLow control plane before camera wake");
    connectHaLow();
    if (halowConnected) {
      startHttpServer();
    } else {
      Serial.println("[HaLow] local serial mode boot did not reach control plane");
    }
    startLocalUdpInspectors();
    printSerialHelp();
    printRuntimeStatus();
    if (controlWorkerTaskHandle == nullptr) {
      xTaskCreatePinnedToCore(controlWorkerTask,
                              "control-worker",
                              8192,
                              nullptr,
                              1,
                              &controlWorkerTaskHandle,
                              CONTROL_WORKER_CORE);
    }
    return;
  }

  bleWakeAttempted = true;
  bleWakeConfirmed = runExactBleWake();
  Serial.printf("[BLE] exact wake result: %s stage=%s\n",
                bleWakeConfirmed ? "success" : "no-confirmation",
                bleStage.c_str());
  Serial.println("Waiting up to 60s for camera hotspot after BLE wake");
  const bool hotspotVisible = waitForCameraWifiPresence(60000, 5000);
  Serial.printf("[WiFi] hotspot visibility after BLE wake: %s\n", hotspotVisible ? "yes" : "no");
  delay(1000);

  connectCameraWifi();
  if (!RUN_LOCAL_SERIAL_TEST) {
    closeBleWakeSession();
  } else {
    Serial.println("[BLE] keeping wake session open in local serial mode");
  }
  if (wifiConnected) {
    runCameraHttpSelfTest();
    startLocalUdpInspectors();
    printSerialHelp();
    printRuntimeStatus();
  } else {
    Serial.println("Skipping camera HTTP self-test because camera WiFi is down");
  }
  connectHaLow();
  startHttpServer();
  if (controlWorkerTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(controlWorkerTask,
                            "control-worker",
                            8192,
                            nullptr,
                            1,
                            &controlWorkerTaskHandle,
                            CONTROL_WORKER_CORE);
  }

  xTaskCreatePinnedToCore(udpForwardTask, "udp-primary", 4096, reinterpret_cast<void *>(0), 1, nullptr, 0);
  xTaskCreatePinnedToCore(udpForwardTask, "udp-secondary", 4096, reinterpret_cast<void *>(1), 1, nullptr, 0);
}

void loop() {
  pollSerialConsole();

  if (RUN_LOCAL_SERIAL_TEST) {
    if (httpServerStarted) {
      server.handleClient();
    }
    refreshWifiState();
    if (streamSessionActive && rtspSessionOpen && millis() - lastRtspKeepaliveMs > RTSP_KEEPALIVE_INTERVAL_MS) {
      sendRtspKeepalive();
    }
    if (wifiConnected &&
        millis() - lastHttpKeepaliveMs > HTTP_KEEPALIVE_INTERVAL_MS) {
      const bool keepaliveOk = sendHttpKeepalive();
      if (!streamSessionActive &&
          !keepaliveOk &&
          httpKeepaliveFailures >= HTTP_KEEPALIVE_FAILURE_THRESHOLD) {
        recoverIdleWifi("http_keepalive_failed");
      }
    }
    if (streamSessionActive && bleClient != nullptr && bleClient->isConnected() &&
        millis() - lastBleKeepaliveMs > BLE_KEEPALIVE_INTERVAL_MS) {
      sendBleWakePulse();
    }
    if (streamSessionActive && getTunnelSocketSnapshot() < 0 && wifiConnected &&
        millis() - lastTunnelReconnectMs > TUNNEL_RECONNECT_INTERVAL_MS) {
      lastTunnelReconnectMs = millis();
      Serial.println("[tunnel] attempting reconnect");
      connectTunnelSocket();
    }
    if (streamSessionActive && !wifiConnected) {
      recoverActiveStream("camera_wifi_lost");
    }
    if (streamSessionActive && wifiConnected &&
        lastPrimaryPacketMs > 0 &&
        millis() - lastPrimaryPacketMs > STREAM_STALL_TIMEOUT_MS) {
      Serial.printf("[stream] primary RTP stalled for %lu ms\n", msSince(lastPrimaryPacketMs));
      recoverActiveStream("rtp_stall");
    }
    if (!streamSessionActive && !wifiConnected && cameraWifiEverConnected && !isControlActionActive() &&
        millis() - lastRescanMs > 15000) {
      lastRescanMs = millis();
      Serial.printf("[state] ble_attempted=%s ble_ok=%s notify_count=%u last=%s\n",
                    bleWakeAttempted ? "yes" : "no",
                    bleWakeConfirmed ? "yes" : "no",
                    bleNotifyCount,
                    bleLastNotifyText.c_str());
      Serial.printf("[state] ble_stage=%s\n", bleStage.c_str());
      scanCameraWifiPresence();
      recoverIdleWifi("idle_wifi_down");
    }
    if (wifiConnected && millis() - lastStatusLogMs > 10000) {
      lastStatusLogMs = millis();
      printRuntimeStatus();
    }
    cooperativeDelay(25);
    return;
  }

  server.handleClient();

  if (millis() - lastStatusLogMs > 5000) {
    lastStatusLogMs = millis();
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    halowConnected = (HaLow.status() == WL_CONNECTED);
    Serial.printf("WiFi=%s (%s) HaLow=%s (%s) UDP1=%u/%u UDP2=%u/%u\n",
                  wifiConnected ? "up" : "down",
                  WiFi.localIP().toString().c_str(),
                  halowConnected ? "up" : "down",
                  HaLow.localIP().toString().c_str(),
                  mediaPrimaryPackets, mediaPrimaryBytes,
                  mediaSecondaryPackets, mediaSecondaryBytes);
  }
}
