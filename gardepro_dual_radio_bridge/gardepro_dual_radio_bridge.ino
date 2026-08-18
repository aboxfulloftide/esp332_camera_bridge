#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <HaLow.h>
#include <NimBLEDevice.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <FS.h>
#include <halow_SD.h>
#include <SPI.h>
#include <uri/UriRegex.h>
#include <algorithm>
#include <vector>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>

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
#ifndef BOARD_HOSTNAME
#define BOARD_HOSTNAME "trail_esp32"
#endif
#ifndef SCANNER_HOST
#define SCANNER_HOST BOARD_HOSTNAME
#endif
#ifndef UPSTREAM_API_HOST
#define UPSTREAM_API_HOST "192.168.1.42"
#endif
#ifndef UPSTREAM_API_PORT
#define UPSTREAM_API_PORT 80
#endif
#ifndef UPSTREAM_API_PREFIX
#define UPSTREAM_API_PREFIX "/trail_cam"
#endif
#ifndef UPSTREAM_API_TOKEN
#define UPSTREAM_API_TOKEN ""
#endif
#ifndef AIR_SCAN_API_HOST
#define AIR_SCAN_API_HOST "192.168.1.22"
#endif
#ifndef AIR_SCAN_API_PORT
#define AIR_SCAN_API_PORT 8002
#endif
#ifndef BOARD_TIMEZONE
#define BOARD_TIMEZONE "EST5EDT,M3.2.0/2,M11.1.0/2"
#endif
#ifndef UPSTREAM_TUNNEL_HOST
#define UPSTREAM_TUNNEL_HOST UPSTREAM_API_HOST
#endif
#ifndef UPSTREAM_TUNNEL_PORT
#define UPSTREAM_TUNNEL_PORT 6000
#endif
static const char *HALOW_REGION = "US";

// HTTP proxy served locally on the board.
static const uint16_t BRIDGE_HTTP_PORT = 18080;

// UDP forwarding config.
// These are the local receive ports on the bridge side.
// The actual Android app uses dynamic ports, so tune these once you know the
// live-view negotiation behavior for your deployment.
static const uint16_t LOCAL_MEDIA_PORT_PRIMARY = 25748;
static const uint16_t LOCAL_MEDIA_PORT_SECONDARY = 25749;

// Upstream relay target on the HaLow network. Defaults to the same host as the
// API server unless local_config.h defines UPSTREAM_TUNNEL_HOST separately.
static const bool RUN_LOCAL_SERIAL_TEST = true;
static const char *FIRMWARE_NAME = "gardepro_unified";
static const char *FIRMWARE_VERSION = "0.1.0";
static const char *FIRMWARE_BUILD = __DATE__ " " __TIME__;
static const char *CAMERA_BLE_MAC = "a4:6d:d4:9e:47:32";
static const char *CAMERA_BLE_WAKE = "AT+WAKEPULSE=10\r\n";
static const char *CAMERA_BLE_NAME = "CAM8Z8_NoName_G_E6";
static const char *CAMERA_BLE_NAME_PREFIX = "CAM8Z8_";
static const char *CAMERA_BLE_SERVICE_UUID = "6e000100-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_BLE_GATT_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_BLE_NOTIFY_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
static const char *CAMERA_BLE_DATA_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";
static const size_t BLE_RECENT_DEVICE_SLOTS = 16;

#ifndef ONBOARD_CAPTURE_INTERVAL_MS
#define ONBOARD_CAPTURE_INTERVAL_MS 1800000UL
#endif
#ifndef ONBOARD_CAPTURE_ENABLED
#define ONBOARD_CAPTURE_ENABLED 1
#endif
#ifndef ONBOARD_CAPTURE_START_MINUTE
#define ONBOARD_CAPTURE_START_MINUTE 360
#endif
#ifndef ONBOARD_CAPTURE_END_MINUTE
#define ONBOARD_CAPTURE_END_MINUTE 1080
#endif

static const int BAT_ADC_PIN = 1;
static const int BAT_ADC_CTRL_PIN = 20;
static const int BAT_CHRG_PIN = 15;
static const int BAT_DONE_PIN = 16;
static const int SD_MOSI_PIN = 11;
static const int SD_CLK_PIN = 15;
static const int SD_MISO_PIN = 16;
static const int SD_CS_PIN = 10;
static const unsigned long WIFI_SCAN_CACHE_MS = 60000UL;
static const unsigned long WIFI_SCAN_DAY_INTERVAL_MS = 120000UL;
static const unsigned long WIFI_SCAN_NIGHT_INTERVAL_MS = 900000UL;
static const unsigned long WIFI_SCAN_INITIAL_DELAY_MS = 15000UL;
static const uint16_t WIFI_SCAN_DAY_START_MINUTE = 420;
static const uint16_t WIFI_SCAN_DAY_END_MINUTE = 1260;

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
bool bleInitialized = false;
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
static const unsigned long TUNNEL_CONNECT_TIMEOUT_MS = 3000;
static const unsigned long BLE_KEEPALIVE_INTERVAL_MS = 20000;
static const unsigned long HTTP_KEEPALIVE_INTERVAL_MS = 15000;
static const unsigned long TUNNEL_RECONNECT_INTERVAL_MS = 2000;
static const unsigned long STREAM_STALL_TIMEOUT_MS = 4000;
static const unsigned long STREAM_RECOVERY_COOLDOWN_MS = 10000;
static const unsigned long IDLE_WIFI_RECOVERY_COOLDOWN_MS = 30000;
static const unsigned long CAMERA_SESSION_DEFAULT_LEASE_MS = 120000;
static const unsigned long CAMERA_SESSION_MAX_LEASE_MS = 600000;
static const unsigned long CAMERA_IDLE_HOLD_MS = 120000;
static const uint8_t HTTP_KEEPALIVE_FAILURE_THRESHOLD = 2;
static const BaseType_t CONTROL_WORKER_CORE = 0;
static const uint8_t BLE_SCAN_ATTEMPTS = 3;
static const uint16_t BLE_SCAN_WINDOW_SEC = 5;
static const unsigned long BLE_SCAN_RETRY_DELAY_MS = 1000;
static const uint8_t BLE_PASSIVE_SCAN_ATTEMPTS = 2;
static const uint16_t BLE_PASSIVE_SCAN_WINDOW_SEC = 8;
static const uint16_t BLE_SCAN_INTERVAL_MS = 160;
static const uint16_t BLE_SCAN_WINDOW_MS = 80;
static const unsigned long BLE_INIT_WARMUP_MS = 5000;
static const unsigned long BLE_REUSE_WARMUP_MS = 750;
static const unsigned long BLE_SCAN_CONNECT_SETTLE_MS = 500;
static const unsigned long BLE_CONNECT_RETRY_DELAY_MS = 1000;
static const uint8_t BLE_CONNECT_ATTEMPTS = 3;
static const uint16_t BLE_CONNECT_TIMEOUT_MS = 15000;
static const uint8_t BLE_WAKE_PULSE_ATTEMPTS = 3;
static const unsigned long BLE_WAKE_PULSE_DELAY_MS = 350;
static const size_t BLE_OBSERVATION_DEVICE_LIMIT = 96;
static const size_t BLE_OBSERVATION_FIELD_LIMIT = 512;
static const uint32_t BLE_OBSERVATION_SCAN_WINDOW_MS = 10000UL;
static const unsigned long BRINGUP_HOTSPOT_WAIT_MS = 20000;
static const unsigned long BRINGUP_HOTSPOT_POLL_MS = 3000;
static const unsigned long BRINGUP_HOTSPOT_FAST_WINDOW_MS = 10000;
static const unsigned long BRINGUP_HOTSPOT_FAST_POLL_MS = 1000;
static const uint32_t UDP_LOG_FIRST_PACKETS = 8;
static const uint32_t UDP_LOG_EVERY_N_PACKETS = 120;
uint32_t streamRecoveryAttempts = 0;
uint32_t idleRecoveryAttempts = 0;
uint32_t httpKeepaliveFailures = 0;
String lastStreamStartStage = "idle";
String lastStreamStartMessage = "";
String lastStreamPlayUrl = "";
unsigned long lastStreamStartElapsedMs = 0;
unsigned long lastStreamStartMs = 0;
int lastStreamDescribeStatus = 0;
int lastStreamSetupStatus = 0;
int lastStreamPlayStatus = 0;
int lastTunnelConnectError = 0;
int bleLastConnectError = 0;
uint8_t bleConnectAttempts = 0;
bool cameraSessionLeaseActive = false;
bool cameraSessionLeaseStandbyOnExpire = true;
bool cameraSessionLeaseExpiredStandbySent = false;
unsigned long cameraSessionLeaseStartedMs = 0;
unsigned long cameraSessionLeaseExpiresMs = 0;
unsigned long cameraSessionLeaseDurationMs = 0;
unsigned long lastCameraRequestMs = 0;
unsigned long lastBringupElapsedMs = 0;
unsigned long lastBleWakeElapsedMs = 0;
unsigned long lastHotspotWaitElapsedMs = 0;
unsigned long lastWifiJoinElapsedMs = 0;
unsigned long lastCameraHttpElapsedMs = 0;
SemaphoreHandle_t tunnelWriteMutex = nullptr;
TaskHandle_t controlWorkerTaskHandle = nullptr;
TaskHandle_t wifiScannerTaskHandle = nullptr;
SemaphoreHandle_t observationUploadMutex = nullptr;

SemaphoreHandle_t onboardFrameMutex = nullptr;
SemaphoreHandle_t onboardStorageMutex = nullptr;
TaskHandle_t onboardCaptureTaskHandle = nullptr;
bool onboardCameraReady = false;
bool onboardStorageReady = false;
String onboardStorageError;
uint32_t onboardStoredPhotoCount = 0;
uint32_t onboardLatestMediaId = 0;
uint32_t onboardNextMediaId = 1;
static const char *ONBOARD_MEDIA_DIR = "/onboard";
static const uint32_t ONBOARD_MEDIA_META_MAGIC = 0x4f4d4431;
static const char *OBSERVATION_QUEUE_DIR = "/obsq";
static const size_t OBSERVATION_QUEUE_MAX_FILES = 48;
static const size_t OBSERVATION_QUEUE_MIN_FREE_BYTES = 32768;
static SPIClass sdSpi(HSPI);
bool sdReady = false;
String sdLastMessage = "not_started";
uint32_t sdMountAttempts = 0;
uint32_t sdMountSuccesses = 0;
bool onboardCaptureEnabled = ONBOARD_CAPTURE_ENABLED != 0;
unsigned long onboardCaptureIntervalMs = ONBOARD_CAPTURE_INTERVAL_MS;
uint16_t onboardCaptureStartMinute = ONBOARD_CAPTURE_START_MINUTE;
uint16_t onboardCaptureEndMinute = ONBOARD_CAPTURE_END_MINUTE;
int16_t onboardCaptureTzOffsetMin = 0;
static const unsigned long ONBOARD_SCHEDULER_TICK_MS = 1000;
static const unsigned long ONBOARD_TIMELAPSE_DEFAULT_INTERVAL_MS = 300000;
static const unsigned long ONBOARD_TIMELAPSE_MIN_INTERVAL_MS = 5000;
static const unsigned long ONBOARD_TIMELAPSE_MAX_DURATION_MS = 7UL * 24UL * 60UL * 60UL * 1000UL;
bool onboardTimelapseActive = false;
unsigned long onboardTimelapseStartedMs = 0;
unsigned long onboardTimelapseDurationMs = 0;
unsigned long onboardTimelapseIntervalMs = ONBOARD_TIMELAPSE_DEFAULT_INTERVAL_MS;
unsigned long onboardTimelapseLastCaptureMs = 0;
uint32_t onboardTimelapseCaptureCount = 0;
uint32_t onboardTimelapseCompletedCount = 0;
String onboardTimelapseLastState = "idle";
uint8_t *onboardLatestJpeg = nullptr;
size_t onboardLatestJpegLen = 0;
uint32_t onboardCaptureCount = 0;
uint32_t onboardCaptureFailures = 0;
uint32_t onboardCaptureScheduleSkips = 0;
unsigned long onboardLastCaptureMs = 0;
unsigned long onboardLastScheduleAttemptMs = 0;
unsigned long onboardLastClockInvalidSkipLogMs = 0;
framesize_t onboardFrameSize = FRAMESIZE_UXGA;
int onboardJpegQuality = 8;
int onboardBrightness = 0;
int onboardContrast = 1;
int onboardSaturation = 0;
int onboardSharpness = 1;
bool onboardVflip = true;
bool onboardHmirror = false;
bool onboardAwb = true;
bool onboardAwbGain = true;
int onboardWbMode = 0;
bool onboardAec = true;
bool onboardAec2 = true;
int onboardAeLevel = -1;
int onboardAecValue = 300;
bool onboardAgc = true;
int onboardAgcGain = 0;
int onboardSpecialEffect = 0;

struct OnboardMediaInfo {
  uint32_t id;
  uint32_t recordedAt;
  size_t bytes;
  uint16_t width;
  uint16_t height;
  uint8_t captureKind;
};

struct OnboardMediaMetaDisk {
  uint32_t magic;
  uint32_t id;
  uint32_t recordedAt;
  uint32_t bytes;
  uint16_t width;
  uint16_t height;
  uint8_t captureKind;
  uint8_t reserved[3];
  uint32_t capturedUptimeSec;
  uint32_t bootSessionId;
};

struct OnboardCameraSettings {
  framesize_t frameSize;
  int jpegQuality;
  int brightness;
  int contrast;
  int saturation;
  int sharpness;
  bool vflip;
  bool hmirror;
  bool awb;
  bool awbGain;
  int wbMode;
  bool aec;
  bool aec2;
  int aeLevel;
  int aecValue;
  bool agc;
  int agcGain;
  int specialEffect;
};

String wifiScanLastJson = "{\"networks\":[]}";
String wifiObservationsLastJson = "";
uint16_t wifiScanLastCount = 0;
unsigned long wifiScanLastMs = 0;
bool wifiScanBusy = false;
unsigned long wifiScannerLastRunMs = 0;
unsigned long wifiScannerLastUploadMs = 0;
uint32_t wifiScannerRunCount = 0;
uint32_t wifiScannerUploadSuccessCount = 0;
uint32_t wifiScannerUploadFailureCount = 0;
String wifiScannerLastError = "idle";
unsigned long bleScannerLastRunMs = 0;
unsigned long bleScannerLastUploadMs = 0;
uint32_t bleScannerRunCount = 0;
uint32_t bleScannerLastCount = 0;
uint32_t bleScannerUploadSuccessCount = 0;
uint32_t bleScannerUploadFailureCount = 0;
uint32_t bleScannerLastManufacturerCount = 0;
uint32_t bleScannerLastServicesCount = 0;
uint32_t bleScannerLastServiceDataCount = 0;
uint32_t bleScannerLastTxPowerCount = 0;
uint32_t bleScannerLastNameCount = 0;
String bleScannerLastError = "idle";
String bleObservationsLastJson = "";

struct BleObservationEntry {
  bool active;
  String mac;
  int rssi;
  bool isRandomized;
  bool hasTxPower;
  int txPower;
  String localName;
  String manufacturerData;
  String advServices;
  String advServiceData;
};

BleObservationEntry bleObservationEntries[BLE_OBSERVATION_DEVICE_LIMIT];
size_t bleObservationEntryCount = 0;

Preferences runtimePrefs;
uint32_t persistentBootCount = 0;
uint32_t bootSessionId = 0;

static const size_t UPLOAD_EVENT_QUEUE_SIZE = 12;
String uploadEventQueue[UPLOAD_EVENT_QUEUE_SIZE];
size_t uploadEventHead = 0;
size_t uploadEventCount = 0;
uint32_t uploadAttemptCount = 0;
uint32_t uploadSuccessCount = 0;
uint32_t uploadFailureCount = 0;
int uploadLastStatusCode = 0;
String uploadLastMessage = "idle";
unsigned long uploadLastAttemptMs = 0;
unsigned long uploadLastSuccessMs = 0;
unsigned long lastClockSyncAttemptMs = 0;
uint32_t observationQueueNextId = 1;
uint32_t observationQueueEnqueueCount = 0;
uint32_t observationQueueDropCount = 0;
uint32_t observationQueueReplaySuccessCount = 0;
uint32_t observationQueueReplayFailureCount = 0;
String observationQueueLastError = "";
size_t observationQueueCachedCount = 0;
size_t observationQueueCachedBytes = 0;

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

static NimBLEAdvertisedDevice bleTargetDevice;
static bool bleTargetDeviceFound = false;
static NimBLEClient *bleClient = nullptr;
static NimBLERemoteCharacteristic *bleNotifyChar3 = nullptr;
static NimBLERemoteCharacteristic *bleDataChar4 = nullptr;
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

bool connectSocketWithTimeout(int sock, const sockaddr_in &addr, unsigned long timeoutMs, int &errorCode) {
  errorCode = 0;
  const int flags = fcntl(sock, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  }

  const int rc = connect(sock,
                         reinterpret_cast<const sockaddr *>(&addr),
                         sizeof(addr));
  if (rc == 0) {
    if (flags >= 0) {
      fcntl(sock, F_SETFL, flags);
    }
    return true;
  }
  if (errno != EINPROGRESS) {
    errorCode = errno;
    if (flags >= 0) {
      fcntl(sock, F_SETFL, flags);
    }
    return false;
  }

  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(sock, &writeSet);
  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000UL;
  timeout.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000UL) * 1000UL);
  const int selected = select(sock + 1, nullptr, &writeSet, nullptr, &timeout);
  if (selected <= 0) {
    errorCode = selected == 0 ? ETIMEDOUT : errno;
    if (flags >= 0) {
      fcntl(sock, F_SETFL, flags);
    }
    return false;
  }

  int socketError = 0;
  socklen_t socketErrorLen = sizeof(socketError);
  if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLen) != 0) {
    errorCode = errno;
    if (flags >= 0) {
      fcntl(sock, F_SETFL, flags);
    }
    return false;
  }
  if (socketError != 0) {
    errorCode = socketError;
    if (flags >= 0) {
      fcntl(sock, F_SETFL, flags);
    }
    return false;
  }

  if (flags >= 0) {
    fcntl(sock, F_SETFL, flags);
  }
  return true;
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
  metadata += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
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
  upstreamAddr.sin_addr.s_addr = inet_addr(UPSTREAM_TUNNEL_HOST);
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
  lastTunnelConnectError = 0;

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    Serial.println("[tunnel] socket create failed");
    lastTunnelConnectError = errno;
    return false;
  }

  sockaddr_in upstreamAddr{};
  upstreamAddr.sin_family = AF_INET;
  upstreamAddr.sin_addr.s_addr = inet_addr(UPSTREAM_TUNNEL_HOST);
  upstreamAddr.sin_port = htons(UPSTREAM_TUNNEL_PORT);

  Serial.printf("[tunnel] connecting to %s:%u\n",
                UPSTREAM_TUNNEL_HOST,
                UPSTREAM_TUNNEL_PORT);
  if (!connectSocketWithTimeout(sock, upstreamAddr, TUNNEL_CONNECT_TIMEOUT_MS, lastTunnelConnectError)) {
    Serial.printf("[tunnel] connect failed error=%d timeout_ms=%lu\n",
                  lastTunnelConnectError,
                  TUNNEL_CONNECT_TIMEOUT_MS);
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

bool cameraSessionLeaseExpired(unsigned long nowMs = millis()) {
  return cameraSessionLeaseActive &&
         cameraSessionLeaseExpiresMs > 0 &&
         static_cast<long>(nowMs - cameraSessionLeaseExpiresMs) >= 0;
}

unsigned long cameraSessionLeaseRemainingMs(unsigned long nowMs = millis()) {
  if (!cameraSessionLeaseActive || cameraSessionLeaseExpiresMs == 0 ||
      cameraSessionLeaseExpired(nowMs)) {
    return 0;
  }
  return cameraSessionLeaseExpiresMs - nowMs;
}

bool cameraIdleHoldActive(unsigned long nowMs = millis()) {
  return lastCameraRequestMs > 0 && nowMs - lastCameraRequestMs < CAMERA_IDLE_HOLD_MS;
}

void refreshCameraSessionLease() {
  if (cameraSessionLeaseExpired()) {
    cameraSessionLeaseActive = false;
    cameraSessionLeaseExpiredStandbySent = false;
  }
}

void markCameraActivity() {
  lastCameraRequestMs = millis();
  if (cameraSessionLeaseActive) {
    cameraSessionLeaseExpiresMs = lastCameraRequestMs + cameraSessionLeaseDurationMs;
    cameraSessionLeaseExpiredStandbySent = false;
  }
}

void startCameraSessionLease(unsigned long durationMs, bool standbyOnExpire) {
  if (durationMs == 0) {
    durationMs = CAMERA_SESSION_DEFAULT_LEASE_MS;
  }
  if (durationMs > CAMERA_SESSION_MAX_LEASE_MS) {
    durationMs = CAMERA_SESSION_MAX_LEASE_MS;
  }
  const unsigned long nowMs = millis();
  cameraSessionLeaseActive = true;
  cameraSessionLeaseStandbyOnExpire = standbyOnExpire;
  cameraSessionLeaseExpiredStandbySent = false;
  cameraSessionLeaseStartedMs = nowMs;
  cameraSessionLeaseDurationMs = durationMs;
  cameraSessionLeaseExpiresMs = nowMs + durationMs;
  lastCameraRequestMs = nowMs;
  standbyRequested = false;
}

void releaseCameraSessionLease() {
  cameraSessionLeaseActive = false;
  cameraSessionLeaseExpiresMs = 0;
  cameraSessionLeaseDurationMs = 0;
  cameraSessionLeaseExpiredStandbySent = false;
}

String buildCameraSessionJson() {
  refreshCameraSessionLease();
  String payload = "{";
  payload += "\"lease_active\":" + String(cameraSessionLeaseActive ? "true" : "false");
  payload += ",\"lease_remaining_ms\":" + String(cameraSessionLeaseRemainingMs());
  payload += ",\"lease_duration_ms\":" + String(cameraSessionLeaseDurationMs);
  payload += ",\"lease_started_age_ms\":" + String(msSince(cameraSessionLeaseStartedMs));
  payload += ",\"lease_standby_on_expire\":" + String(cameraSessionLeaseStandbyOnExpire ? "true" : "false");
  payload += ",\"idle_hold_active\":" + String(cameraIdleHoldActive() ? "true" : "false");
  payload += ",\"idle_hold_ms\":" + String(CAMERA_IDLE_HOLD_MS);
  payload += ",\"last_camera_request_age_ms\":" + String(msSince(lastCameraRequestMs));
  payload += "}";
  return payload;
}

String buildTimingJson() {
  String payload = "{";
  payload += "\"last_bringup_elapsed_ms\":" + String(lastBringupElapsedMs);
  payload += ",\"last_ble_wake_elapsed_ms\":" + String(lastBleWakeElapsedMs);
  payload += ",\"last_hotspot_wait_elapsed_ms\":" + String(lastHotspotWaitElapsedMs);
  payload += ",\"last_wifi_join_elapsed_ms\":" + String(lastWifiJoinElapsedMs);
  payload += ",\"last_camera_http_elapsed_ms\":" + String(lastCameraHttpElapsedMs);
  payload += "}";
  return payload;
}

String buildStreamStatusJson() {
  String payload = "{";
  payload += "\"last_stage\":\"" + jsonEscape(lastStreamStartStage) + "\"";
  payload += ",\"last_message\":\"" + jsonEscape(lastStreamStartMessage) + "\"";
  payload += ",\"last_elapsed_ms\":" + String(lastStreamStartElapsedMs);
  payload += ",\"last_started_age_ms\":" + String(msSince(lastStreamStartMs));
  payload += ",\"describe_status\":" + String(lastStreamDescribeStatus);
  payload += ",\"setup_status\":" + String(lastStreamSetupStatus);
  payload += ",\"play_status\":" + String(lastStreamPlayStatus);
  payload += ",\"play_url\":\"" + jsonEscape(lastStreamPlayUrl) + "\"";
  payload += ",\"tunnel_target\":\"" + jsonEscape(String(UPSTREAM_TUNNEL_HOST) + ":" + String(UPSTREAM_TUNNEL_PORT)) + "\"";
  payload += ",\"tunnel_connect_error\":" + String(lastTunnelConnectError);
  payload += ",\"tunnel_packets_sent\":" + String(tunnelPacketsSent);
  payload += ",\"tunnel_bytes_sent\":" + String(tunnelBytesSent);
  payload += ",\"tunnel_send_failures\":" + String(tunnelSendFailures);
  payload += ",\"udp_primary\":{";
  payload += "\"port\":" + String(udpPrimaryStats.localPort);
  payload += ",\"packets\":" + String(udpPrimaryStats.packets);
  payload += ",\"bytes\":" + String(udpPrimaryStats.bytes);
  payload += ",\"last_source\":\"" + udpPrimaryStats.lastSourceIp.toString() + ":" + String(udpPrimaryStats.lastSourcePort) + "\"";
  payload += ",\"last_packet_len\":" + String(static_cast<unsigned>(udpPrimaryStats.lastPacketLen));
  payload += ",\"last_packet_age_ms\":" + String(msSince(lastPrimaryPacketMs));
  payload += "}";
  payload += ",\"udp_secondary\":{";
  payload += "\"port\":" + String(udpSecondaryStats.localPort);
  payload += ",\"packets\":" + String(udpSecondaryStats.packets);
  payload += ",\"bytes\":" + String(udpSecondaryStats.bytes);
  payload += ",\"last_source\":\"" + udpSecondaryStats.lastSourceIp.toString() + ":" + String(udpSecondaryStats.lastSourcePort) + "\"";
  payload += ",\"last_packet_len\":" + String(static_cast<unsigned>(udpSecondaryStats.lastPacketLen));
  payload += ",\"last_packet_age_ms\":" + String(msSince(lastSecondaryPacketMs));
  payload += "}";
  payload += "}";
  return payload;
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

float readChipTemperatureC() {
  return temperatureRead();
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

String buildHaLowStatusJson() {
  halowConnected = (HaLow.status() == WL_CONNECTED);
  String payload = "{";
  payload += "\"connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"status\":" + String(static_cast<int>(HaLow.status()));
  payload += ",\"ssid\":\"" + jsonEscape(HaLow.SSID()) + "\"";
  payload += ",\"bssid\":\"" + jsonEscape(HaLow.BSSIDstr()) + "\"";
  payload += ",\"mac\":\"" + jsonEscape(HaLow.macAddress()) + "\"";
  payload += ",\"ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"gateway\":\"" + HaLow.gatewayIP().toString() + "\"";
  payload += ",\"rssi_dbm\":" + String(static_cast<int>(HaLow.RSSI()));
  payload += ",\"last_event\":" + String(halowLastEventId);
  payload += ",\"last_event_age_ms\":" + String(msSince(halowLastEventMs));
  payload += ",\"event_count\":" + String(halowEventCount);
  payload += ",\"snr_db\":null";
  payload += ",\"noise_dbm\":null";
  payload += ",\"snr_note\":\"not_exposed_by_current_halow_wrapper\"";
  payload += ",\"rate_control\":null";
  payload += ",\"rate_control_note\":\"not_exposed_by_current_halow_wrapper\"";
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

String minuteOfDayToString(uint16_t minuteOfDay) {
  minuteOfDay %= 1440;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", minuteOfDay / 60, minuteOfDay % 60);
  return String(buf);
}

bool parseMinuteOfDay(const String &value, uint16_t &minuteOfDay) {
  const int colon = value.indexOf(':');
  if (colon < 0) {
    const int raw = value.toInt();
    if (raw < 0 || raw > 1439) {
      return false;
    }
    minuteOfDay = static_cast<uint16_t>(raw);
    return true;
  }
  const int hour = value.substring(0, colon).toInt();
  const int minute = value.substring(colon + 1).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return false;
  }
  minuteOfDay = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

bool onboardClockValid() {
  return time(nullptr) >= 1700000000;
}

int onboardLocalMinuteOfDay() {
  if (!onboardClockValid()) {
    return -1;
  }
  time_t adjusted = time(nullptr);
  setenv("TZ", BOARD_TIMEZONE, 1);
  tzset();
  struct tm tmValue;
  localtime_r(&adjusted, &tmValue);
  if (onboardCaptureTzOffsetMin != 0) {
    adjusted += static_cast<time_t>(onboardCaptureTzOffsetMin) * 60;
    localtime_r(&adjusted, &tmValue);
  }
  return tmValue.tm_hour * 60 + tmValue.tm_min;
}

bool onboardCaptureWindowActive() {
  const int minute = onboardLocalMinuteOfDay();
  if (minute < 0) {
    return false;
  }
  if (onboardCaptureStartMinute == onboardCaptureEndMinute) {
    return true;
  }
  if (onboardCaptureStartMinute < onboardCaptureEndMinute) {
    return minute >= onboardCaptureStartMinute && minute < onboardCaptureEndMinute;
  }
  return minute >= onboardCaptureStartMinute || minute < onboardCaptureEndMinute;
}

unsigned long onboardTimelapseElapsedMs(unsigned long nowMs = millis()) {
  if (!onboardTimelapseActive || onboardTimelapseStartedMs == 0) {
    return 0;
  }
  return nowMs - onboardTimelapseStartedMs;
}

unsigned long onboardTimelapseRemainingMs(unsigned long nowMs = millis()) {
  if (!onboardTimelapseActive || onboardTimelapseDurationMs == 0) {
    return 0;
  }
  const unsigned long elapsed = onboardTimelapseElapsedMs(nowMs);
  if (elapsed >= onboardTimelapseDurationMs) {
    return 0;
  }
  return onboardTimelapseDurationMs - elapsed;
}

void stopOnboardTimelapse(const char *state) {
  if (onboardTimelapseActive) {
    ++onboardTimelapseCompletedCount;
    Serial.printf("[onboard-timelapse] stopped state=%s captures=%u\n",
                  state,
                  onboardTimelapseCaptureCount);
  }
  onboardTimelapseActive = false;
  onboardTimelapseLastState = state;
}

void refreshOnboardTimelapseState() {
  if (!onboardTimelapseActive) {
    return;
  }
  if (onboardTimelapseDurationMs > 0 && onboardTimelapseElapsedMs() >= onboardTimelapseDurationMs) {
    stopOnboardTimelapse("complete");
  }
}

void startOnboardTimelapse(unsigned long durationMs, unsigned long intervalMs) {
  onboardTimelapseDurationMs = durationMs;
  onboardTimelapseIntervalMs = intervalMs < ONBOARD_TIMELAPSE_MIN_INTERVAL_MS ? ONBOARD_TIMELAPSE_MIN_INTERVAL_MS : intervalMs;
  onboardTimelapseStartedMs = millis();
  onboardTimelapseLastCaptureMs = 0;
  onboardTimelapseCaptureCount = 0;
  onboardTimelapseActive = true;
  onboardTimelapseLastState = "active";
  Serial.printf("[onboard-timelapse] started duration_ms=%lu interval_ms=%lu\n",
                onboardTimelapseDurationMs,
                onboardTimelapseIntervalMs);
}

const char *frameSizeName(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QQVGA:
      return "QQVGA";
    case FRAMESIZE_QVGA:
      return "QVGA";
    case FRAMESIZE_VGA:
      return "VGA";
    case FRAMESIZE_SVGA:
      return "SVGA";
    case FRAMESIZE_XGA:
      return "XGA";
    case FRAMESIZE_SXGA:
      return "SXGA";
    case FRAMESIZE_UXGA:
      return "UXGA";
    default:
      return "UNKNOWN";
  }
}

bool parseFrameSize(const String &input, framesize_t &size) {
  String value = input;
  value.toUpperCase();
  if (value == "QQVGA") {
    size = FRAMESIZE_QQVGA;
  } else if (value == "QVGA") {
    size = FRAMESIZE_QVGA;
  } else if (value == "VGA") {
    size = FRAMESIZE_VGA;
  } else if (value == "SVGA") {
    size = FRAMESIZE_SVGA;
  } else if (value == "XGA") {
    size = FRAMESIZE_XGA;
  } else if (value == "SXGA") {
    size = FRAMESIZE_SXGA;
  } else if (value == "UXGA") {
    size = FRAMESIZE_UXGA;
  } else {
    return false;
  }
  return true;
}

bool serverHasOnboardSensorArgs() {
  return server.hasArg("framesize") ||
         server.hasArg("jpeg_quality") ||
         server.hasArg("quality") ||
         server.hasArg("brightness") ||
         server.hasArg("contrast") ||
         server.hasArg("saturation") ||
         server.hasArg("sharpness") ||
         server.hasArg("vflip") ||
         server.hasArg("hmirror") ||
         server.hasArg("awb") ||
         server.hasArg("awb_gain") ||
         server.hasArg("wb_mode") ||
         server.hasArg("aec") ||
         server.hasArg("aec2") ||
         server.hasArg("ae_level") ||
         server.hasArg("aec_value") ||
         server.hasArg("agc") ||
         server.hasArg("agc_gain") ||
         server.hasArg("special_effect");
}

OnboardCameraSettings snapshotOnboardCameraSettings() {
  OnboardCameraSettings settings{};
  settings.frameSize = onboardFrameSize;
  settings.jpegQuality = onboardJpegQuality;
  settings.brightness = onboardBrightness;
  settings.contrast = onboardContrast;
  settings.saturation = onboardSaturation;
  settings.sharpness = onboardSharpness;
  settings.vflip = onboardVflip;
  settings.hmirror = onboardHmirror;
  settings.awb = onboardAwb;
  settings.awbGain = onboardAwbGain;
  settings.wbMode = onboardWbMode;
  settings.aec = onboardAec;
  settings.aec2 = onboardAec2;
  settings.aeLevel = onboardAeLevel;
  settings.aecValue = onboardAecValue;
  settings.agc = onboardAgc;
  settings.agcGain = onboardAgcGain;
  settings.specialEffect = onboardSpecialEffect;
  return settings;
}

bool applyOnboardCameraSettings(const OnboardCameraSettings &settings, String &error) {
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    error = "sensor_unavailable";
    return false;
  }

  if (!psramFound() && settings.frameSize > FRAMESIZE_SVGA) {
    error = "framesize_requires_psram";
    return false;
  }

  onboardFrameSize = settings.frameSize;
  onboardJpegQuality = settings.jpegQuality;
  onboardBrightness = settings.brightness;
  onboardContrast = settings.contrast;
  onboardSaturation = settings.saturation;
  onboardSharpness = settings.sharpness;
  onboardVflip = settings.vflip;
  onboardHmirror = settings.hmirror;
  onboardAwb = settings.awb;
  onboardAwbGain = settings.awbGain;
  onboardWbMode = settings.wbMode;
  onboardAec = settings.aec;
  onboardAec2 = settings.aec2;
  onboardAeLevel = settings.aeLevel;
  onboardAecValue = settings.aecValue;
  onboardAgc = settings.agc;
  onboardAgcGain = settings.agcGain;
  onboardSpecialEffect = settings.specialEffect;

  s->set_framesize(s, onboardFrameSize);
  s->set_quality(s, onboardJpegQuality);
  s->set_brightness(s, onboardBrightness);
  s->set_contrast(s, onboardContrast);
  s->set_saturation(s, onboardSaturation);
  s->set_sharpness(s, onboardSharpness);
  s->set_vflip(s, onboardVflip);
  s->set_hmirror(s, onboardHmirror);
  s->set_whitebal(s, onboardAwb);
  s->set_awb_gain(s, onboardAwbGain);
  s->set_wb_mode(s, onboardWbMode);
  s->set_exposure_ctrl(s, onboardAec);
  s->set_aec2(s, onboardAec2);
  s->set_ae_level(s, onboardAeLevel);
  s->set_aec_value(s, onboardAecValue);
  s->set_gain_ctrl(s, onboardAgc);
  s->set_agc_gain(s, onboardAgcGain);
  s->set_special_effect(s, onboardSpecialEffect);
  return true;
}

bool boolLikeValue(const String &value) {
  String normalized = value;
  normalized.toLowerCase();
  return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

void clampOnboardConfig() {
  if (onboardCaptureIntervalMs < 5000UL) {
    onboardCaptureIntervalMs = 5000UL;
  }
  if (onboardCaptureStartMinute > 1439) {
    onboardCaptureStartMinute = ONBOARD_CAPTURE_START_MINUTE;
  }
  if (onboardCaptureEndMinute > 1439) {
    onboardCaptureEndMinute = ONBOARD_CAPTURE_END_MINUTE;
  }
  if (onboardJpegQuality < 4) {
    onboardJpegQuality = 4;
  }
  if (onboardJpegQuality > 63) {
    onboardJpegQuality = 63;
  }
  if (!psramFound() && onboardFrameSize > FRAMESIZE_SVGA) {
    onboardFrameSize = FRAMESIZE_SVGA;
    onboardJpegQuality = 10;
  }
}

void loadOnboardConfig() {
  if (!runtimePrefs.begin("onboard", true)) {
    Serial.println("[onboard-config] failed to open preferences for read");
    onboardFrameSize = psramFound() ? FRAMESIZE_UXGA : FRAMESIZE_SVGA;
    onboardJpegQuality = psramFound() ? 8 : 10;
    clampOnboardConfig();
    return;
  }
  onboardCaptureEnabled = runtimePrefs.getBool("enabled", onboardCaptureEnabled);
  onboardCaptureIntervalMs = runtimePrefs.getULong("interval", onboardCaptureIntervalMs);
  onboardCaptureStartMinute = runtimePrefs.getUShort("start", onboardCaptureStartMinute);
  onboardCaptureEndMinute = runtimePrefs.getUShort("end", onboardCaptureEndMinute);
  onboardCaptureTzOffsetMin = runtimePrefs.getShort("tz", onboardCaptureTzOffsetMin);
  onboardFrameSize = static_cast<framesize_t>(runtimePrefs.getUChar("framesize", psramFound() ? FRAMESIZE_UXGA : FRAMESIZE_SVGA));
  onboardJpegQuality = runtimePrefs.getUChar("quality", psramFound() ? 8 : 10);
  onboardBrightness = runtimePrefs.getChar("bright", onboardBrightness);
  onboardContrast = runtimePrefs.getChar("contrast", onboardContrast);
  onboardSaturation = runtimePrefs.getChar("saturate", onboardSaturation);
  onboardSharpness = runtimePrefs.getChar("sharp", onboardSharpness);
  onboardVflip = runtimePrefs.getBool("vflip", onboardVflip);
  onboardHmirror = runtimePrefs.getBool("hmirror", onboardHmirror);
  onboardAwb = runtimePrefs.getBool("awb", onboardAwb);
  onboardAwbGain = runtimePrefs.getBool("awb_gain", onboardAwbGain);
  onboardWbMode = runtimePrefs.getChar("wb_mode", onboardWbMode);
  onboardAec = runtimePrefs.getBool("aec", onboardAec);
  onboardAec2 = runtimePrefs.getBool("aec2", onboardAec2);
  onboardAeLevel = runtimePrefs.getChar("ae_level", onboardAeLevel);
  onboardAecValue = runtimePrefs.getUShort("aec_value", onboardAecValue);
  onboardAgc = runtimePrefs.getBool("agc", onboardAgc);
  onboardAgcGain = runtimePrefs.getUChar("agc_gain", onboardAgcGain);
  onboardSpecialEffect = runtimePrefs.getUChar("effect", onboardSpecialEffect);
  runtimePrefs.end();
  clampOnboardConfig();
  Serial.printf("[onboard-config] enabled=%s interval_ms=%lu window=%s-%s tz_offset_min=%d framesize=%s quality=%d brightness=%d contrast=%d sharpness=%d ae_level=%d\n",
                onboardCaptureEnabled ? "yes" : "no",
                onboardCaptureIntervalMs,
                minuteOfDayToString(onboardCaptureStartMinute).c_str(),
                minuteOfDayToString(onboardCaptureEndMinute).c_str(),
                onboardCaptureTzOffsetMin,
                frameSizeName(onboardFrameSize),
                onboardJpegQuality,
                onboardBrightness,
                onboardContrast,
                onboardSharpness,
                onboardAeLevel);
}

void saveOnboardConfig() {
  if (!runtimePrefs.begin("onboard", false)) {
    Serial.println("[onboard-config] failed to open preferences for write");
    return;
  }
  runtimePrefs.putBool("enabled", onboardCaptureEnabled);
  runtimePrefs.putULong("interval", onboardCaptureIntervalMs);
  runtimePrefs.putUShort("start", onboardCaptureStartMinute);
  runtimePrefs.putUShort("end", onboardCaptureEndMinute);
  runtimePrefs.putShort("tz", onboardCaptureTzOffsetMin);
  runtimePrefs.putUChar("framesize", static_cast<uint8_t>(onboardFrameSize));
  runtimePrefs.putUChar("quality", static_cast<uint8_t>(onboardJpegQuality));
  runtimePrefs.putChar("bright", static_cast<int8_t>(onboardBrightness));
  runtimePrefs.putChar("contrast", static_cast<int8_t>(onboardContrast));
  runtimePrefs.putChar("saturate", static_cast<int8_t>(onboardSaturation));
  runtimePrefs.putChar("sharp", static_cast<int8_t>(onboardSharpness));
  runtimePrefs.putBool("vflip", onboardVflip);
  runtimePrefs.putBool("hmirror", onboardHmirror);
  runtimePrefs.putBool("awb", onboardAwb);
  runtimePrefs.putBool("awb_gain", onboardAwbGain);
  runtimePrefs.putChar("wb_mode", static_cast<int8_t>(onboardWbMode));
  runtimePrefs.putBool("aec", onboardAec);
  runtimePrefs.putBool("aec2", onboardAec2);
  runtimePrefs.putChar("ae_level", static_cast<int8_t>(onboardAeLevel));
  runtimePrefs.putUShort("aec_value", static_cast<uint16_t>(onboardAecValue));
  runtimePrefs.putBool("agc", onboardAgc);
  runtimePrefs.putUChar("agc_gain", static_cast<uint8_t>(onboardAgcGain));
  runtimePrefs.putUChar("effect", static_cast<uint8_t>(onboardSpecialEffect));
  runtimePrefs.end();
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

  clampOnboardConfig();
  if (psramFound()) {
    config.frame_size = onboardFrameSize;
    config.jpeg_quality = onboardJpegQuality;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = onboardFrameSize;
    config.jpeg_quality = onboardJpegQuality;
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
    s->set_framesize(s, onboardFrameSize);
    s->set_quality(s, onboardJpegQuality);
    s->set_brightness(s, onboardBrightness);
    s->set_contrast(s, onboardContrast);
    s->set_saturation(s, onboardSaturation);
    s->set_sharpness(s, onboardSharpness);
    s->set_vflip(s, onboardVflip);
    s->set_hmirror(s, onboardHmirror);
    s->set_whitebal(s, onboardAwb);
    s->set_awb_gain(s, onboardAwbGain);
    s->set_wb_mode(s, onboardWbMode);
    s->set_exposure_ctrl(s, onboardAec);
    s->set_aec2(s, onboardAec2);
    s->set_ae_level(s, onboardAeLevel);
    s->set_aec_value(s, onboardAecValue);
    s->set_gain_ctrl(s, onboardAgc);
    s->set_agc_gain(s, onboardAgcGain);
    s->set_special_effect(s, onboardSpecialEffect);
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

const char *onboardCaptureKindName(uint8_t kind) {
  if (kind == 1) return "scheduled";
  if (kind == 2) return "timelapse";
  return "manual";
}

uint8_t onboardCaptureKindValue(const char *kind) {
  if (kind != nullptr && strcmp(kind, "scheduled") == 0) return 1;
  if (kind != nullptr && strcmp(kind, "timelapse") == 0) return 2;
  return 0;
}

String onboardMediaIdString(uint32_t id) {
  char value[12];
  snprintf(value, sizeof(value), "%08lu", static_cast<unsigned long>(id));
  return String(value);
}

String onboardMediaImagePath(uint32_t id) {
  return String(ONBOARD_MEDIA_DIR) + "/" + onboardMediaIdString(id) + ".jpg";
}

String onboardMediaMetaPath(uint32_t id) {
  return String(ONBOARD_MEDIA_DIR) + "/" + onboardMediaIdString(id) + ".meta";
}

fs::FS &onboardMediaFs() {
  return sdReady ? static_cast<fs::FS &>(SD) : static_cast<fs::FS &>(LittleFS);
}

fs::FS &observationQueueFs() {
  return sdReady ? static_cast<fs::FS &>(SD) : static_cast<fs::FS &>(LittleFS);
}

const char *persistentStorageType() {
  return sdReady ? "sd" : "littlefs";
}

uint64_t persistentStorageTotalBytes() {
  return sdReady ? SD.totalBytes() : LittleFS.totalBytes();
}

uint64_t persistentStorageUsedBytes() {
  return sdReady ? SD.usedBytes() : LittleFS.usedBytes();
}

uint64_t persistentStorageFreeBytes() {
  const uint64_t total = persistentStorageTotalBytes();
  const uint64_t used = persistentStorageUsedBytes();
  return total > used ? total - used : 0;
}

bool ensureFsDir(fs::FS &fs, const char *path) {
  return fs.exists(path) || fs.mkdir(path);
}

String onboardRecordedAtJson(uint32_t epoch) {
  if (epoch < 1700000000UL) return "null";
  time_t timestamp = static_cast<time_t>(epoch);
  struct tm tmValue{};
  gmtime_r(&timestamp, &tmValue);
  char formatted[24];
  strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &tmValue);
  return "\"" + String(formatted) + "\"";
}

bool readOnboardMediaInfo(uint32_t id, OnboardMediaInfo &info) {
  if (!onboardStorageReady || id == 0) return false;
  fs::FS &fs = onboardMediaFs();
  const String imagePath = onboardMediaImagePath(id);
  const String metaPath = onboardMediaMetaPath(id);
  File image = fs.open(imagePath, FILE_READ);
  if (!image || image.isDirectory()) return false;
  const size_t imageBytes = image.size();
  image.close();
  File metaFile = fs.open(metaPath, FILE_READ);
  if (!metaFile) return false;
  OnboardMediaMetaDisk disk{};
  const size_t metaSize = metaFile.size();
  const size_t readLen = metaSize < sizeof(disk) ? metaSize : sizeof(disk);
  const size_t readBytes = metaFile.read(reinterpret_cast<uint8_t *>(&disk), readLen);
  metaFile.close();
  if (readBytes != readLen || readLen < 24 || disk.magic != ONBOARD_MEDIA_META_MAGIC || disk.id != id) return false;
  info.id = id;
  info.recordedAt = disk.recordedAt;
  info.bytes = imageBytes;
  info.width = disk.width;
  info.height = disk.height;
  info.captureKind = disk.captureKind;
  return true;
}

void refreshOnboardMediaState() {
  onboardStoredPhotoCount = 0;
  onboardLatestMediaId = 0;
  if (!onboardStorageReady) return;
  fs::FS &fs = onboardMediaFs();
  File dir = fs.open(ONBOARD_MEDIA_DIR, FILE_READ);
  if (!dir || !dir.isDirectory()) return;
  File file = dir.openNextFile(FILE_READ);
  while (file) {
    const String name = file.name();
    if (!file.isDirectory() && name.endsWith(".jpg")) {
      const int slash = name.lastIndexOf('/');
      const String idText = name.substring(slash + 1, name.length() - 4);
      const uint32_t id = strtoul(idText.c_str(), nullptr, 10);
      OnboardMediaInfo info{};
      if (id > 0 && readOnboardMediaInfo(id, info)) {
        ++onboardStoredPhotoCount;
        if (id > onboardLatestMediaId) onboardLatestMediaId = id;
      }
    }
    file.close();
    file = dir.openNextFile(FILE_READ);
  }
  dir.close();
  if (onboardLatestMediaId >= onboardNextMediaId) onboardNextMediaId = onboardLatestMediaId + 1;
}

void backfillOnboardMediaTimestamps() {
  if (!onboardStorageReady || !onboardClockValid()) return;
  fs::FS &fs = onboardMediaFs();
  File dir = fs.open(ONBOARD_MEDIA_DIR, FILE_READ);
  if (!dir || !dir.isDirectory()) return;
  const uint32_t nowEpoch = static_cast<uint32_t>(time(nullptr));
  const uint32_t nowUptimeSec = millis() / 1000UL;
  unsigned updated = 0;
  File file = dir.openNextFile(FILE_READ);
  while (file) {
    const String name = file.name();
    file.close();
    if (name.endsWith(".meta")) {
      const String path = name.startsWith("/") ? name : String(ONBOARD_MEDIA_DIR) + "/" + name;
      File metaFile = fs.open(path, FILE_READ);
      OnboardMediaMetaDisk disk{};
      const size_t readLen = metaFile ? metaFile.read(reinterpret_cast<uint8_t *>(&disk), sizeof(disk)) : 0;
      if (metaFile) metaFile.close();
      if (readLen >= 24 && disk.magic == ONBOARD_MEDIA_META_MAGIC && disk.recordedAt < 1700000000UL && disk.capturedUptimeSec > 0 && disk.capturedUptimeSec <= nowUptimeSec) {
        disk.recordedAt = nowEpoch - (nowUptimeSec - disk.capturedUptimeSec);
        const String tempPath = path + ".tmp";
        fs.remove(tempPath);
        File out = fs.open(tempPath, FILE_WRITE);
        const size_t written = out ? out.write(reinterpret_cast<const uint8_t *>(&disk), sizeof(disk)) : 0;
        if (out) out.close();
        if (written == sizeof(disk) && fs.rename(tempPath, path)) {
          ++updated;
        } else {
          fs.remove(tempPath);
        }
      }
    }
    file = dir.openNextFile(FILE_READ);
  }
  dir.close();
  if (updated > 0) {
    refreshOnboardMediaState();
    Serial.printf("[onboard-storage] backfilled %u media timestamps\n", updated);
  }
}

bool initOnboardStorage() {
  onboardStorageReady = LittleFS.begin(true);
  if (!onboardStorageReady) {
    onboardStorageError = "storage_unavailable";
    Serial.println("[onboard-storage] LittleFS mount failed");
    return false;
  }
  if (!ensureFsDir(LittleFS, ONBOARD_MEDIA_DIR)) {
    onboardStorageReady = false;
    onboardStorageError = "storage_unavailable";
    Serial.println("[onboard-storage] media directory creation failed");
    return false;
  }
  if (!ensureFsDir(LittleFS, OBSERVATION_QUEUE_DIR)) {
    onboardStorageReady = false;
    onboardStorageError = "storage_unavailable";
    Serial.println("[observation-queue] directory creation failed");
    return false;
  }
  Preferences mediaPrefs;
  if (mediaPrefs.begin("media", true)) {
    onboardNextMediaId = mediaPrefs.getULong("next_id", 1);
    mediaPrefs.end();
  }
  Preferences queuePrefs;
  if (queuePrefs.begin("obsq", true)) {
    observationQueueNextId = queuePrefs.getULong("next_id", 1);
    queuePrefs.end();
  }
  refreshOnboardMediaState();
  refreshObservationQueueCachedStats();
  onboardStorageError = "";
  Serial.printf("[onboard-storage] ready total=%u used=%u photos=%u latest=%lu next=%lu\n",
                static_cast<unsigned>(LittleFS.totalBytes()),
                static_cast<unsigned>(LittleFS.usedBytes()),
                onboardStoredPhotoCount,
                static_cast<unsigned long>(onboardLatestMediaId),
                static_cast<unsigned long>(onboardNextMediaId));
  return true;
}

bool mountSdCard(uint32_t frequency = 4000000UL) {
  ++sdMountAttempts;
  SD.end();
  sdSpi.end();
  delay(50);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SD_MISO_PIN, INPUT_PULLUP);
  sdSpi.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, sdSpi, frequency, "/sd", 5, false)) {
    sdReady = false;
    sdLastMessage = "begin_failed";
    Serial.printf("[sd] mount failed freq=%lu\n", static_cast<unsigned long>(frequency));
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    SD.end();
    sdReady = false;
    sdLastMessage = "card_none";
    Serial.println("[sd] no card attached");
    return false;
  }
  sdReady = true;
  sdLastMessage = "mounted";
  if (!ensureFsDir(SD, ONBOARD_MEDIA_DIR)) {
    sdLastMessage = "media_dir_failed";
    Serial.println("[sd] media directory creation failed");
  }
  if (!ensureFsDir(SD, OBSERVATION_QUEUE_DIR)) {
    sdLastMessage = "queue_dir_failed";
    Serial.println("[sd] observation queue directory creation failed");
  }
  if (onboardStorageReady) {
    refreshOnboardMediaState();
    refreshObservationQueueCachedStats();
    if (onboardClockValid()) backfillOnboardMediaTimestamps();
  }
  ++sdMountSuccesses;
  Serial.printf("[sd] mounted type=%u size_mb=%llu total_mb=%llu used_mb=%llu\n",
                static_cast<unsigned>(SD.cardType()),
                SD.cardSize() / (1024ULL * 1024ULL),
                SD.totalBytes() / (1024ULL * 1024ULL),
                SD.usedBytes() / (1024ULL * 1024ULL));
  return true;
}

String buildSdStatusJson() {
  String payload = "{";
  payload += "\"ready\":" + String(sdReady ? "true" : "false");
  payload += ",\"last_message\":\"" + jsonEscape(sdLastMessage) + "\"";
  payload += ",\"mount_attempts\":" + String(sdMountAttempts);
  payload += ",\"mount_successes\":" + String(sdMountSuccesses);
  payload += ",\"pins\":{";
  payload += "\"mosi\":" + String(SD_MOSI_PIN);
  payload += ",\"clk\":" + String(SD_CLK_PIN);
  payload += ",\"miso\":" + String(SD_MISO_PIN);
  payload += ",\"cs\":" + String(SD_CS_PIN);
  payload += "}";
  if (sdReady) {
    payload += ",\"card_type\":" + String(static_cast<unsigned>(SD.cardType()));
    payload += ",\"card_size_bytes\":" + String(static_cast<unsigned long long>(SD.cardSize()));
    payload += ",\"storage_total_bytes\":" + String(static_cast<unsigned long long>(SD.totalBytes()));
    payload += ",\"storage_used_bytes\":" + String(static_cast<unsigned long long>(SD.usedBytes()));
    const uint64_t freeBytes = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
    payload += ",\"storage_free_bytes\":" + String(static_cast<unsigned long long>(freeBytes));
  }
  payload += "}";
  return payload;
}

String buildOnboardMediaJson(const OnboardMediaInfo &info, bool includeCaptureFields) {
  const String id = onboardMediaIdString(info.id);
  String payload = "{";
  payload += "\"id\":\"" + id + "\"";
  payload += ",\"filename\":\"" + id + ".jpg\"";
  payload += ",\"recorded_at\":" + onboardRecordedAtJson(info.recordedAt);
  payload += ",\"bytes\":" + String(static_cast<unsigned>(info.bytes));
  payload += ",\"content_type\":\"image/jpeg\"";
  payload += ",\"width\":" + String(info.width);
  payload += ",\"height\":" + String(info.height);
  if (includeCaptureFields) {
    payload += ",\"capture_kind\":\"" + String(onboardCaptureKindName(info.captureKind)) + "\"";
    payload += ",\"path\":\"/onboard/media/" + id + "\"";
    payload += ",\"thumb_path\":null";
  }
  payload += "}";
  return payload;
}

bool captureOnboardFrame(const char *captureKind = "manual", OnboardMediaInfo *savedMedia = nullptr, String *captureError = nullptr) {
  if (!onboardCameraReady) {
    ++onboardCaptureFailures;
    if (captureError != nullptr) *captureError = "onboard_camera_not_ready";
    return false;
  }
  if (!onboardStorageReady) {
    ++onboardCaptureFailures;
    if (captureError != nullptr) *captureError = "storage_unavailable";
    return false;
  }

  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  fs::FS &fs = onboardMediaFs();

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr || fb->len == 0) {
    ++onboardCaptureFailures;
    if (fb != nullptr) {
      esp_camera_fb_return(fb);
    }
    if (captureError != nullptr) *captureError = "capture_failed";
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }

  const uint64_t freeBytes = persistentStorageFreeBytes();
  if (freeBytes < fb->len + sizeof(OnboardMediaMetaDisk) + 4096) {
    ++onboardCaptureFailures;
    esp_camera_fb_return(fb);
    onboardStorageError = "storage_full";
    if (captureError != nullptr) *captureError = "storage_full";
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }

  uint8_t *copy = static_cast<uint8_t *>(psramFound() ? ps_malloc(fb->len) : malloc(fb->len));
  if (copy == nullptr) {
    ++onboardCaptureFailures;
    esp_camera_fb_return(fb);
    if (captureError != nullptr) *captureError = "capture_failed";
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }
  memcpy(copy, fb->buf, fb->len);
  const size_t copyLen = fb->len;
  const uint16_t width = fb->width;
  const uint16_t height = fb->height;
  const uint32_t id = onboardNextMediaId;
  const uint32_t recordedAt = onboardClockValid() ? static_cast<uint32_t>(time(nullptr)) : 0;
  const String imagePath = onboardMediaImagePath(id);
  const String metaPath = onboardMediaMetaPath(id);
  const String tempImagePath = String(ONBOARD_MEDIA_DIR) + "/.capture.jpg.tmp";
  const String tempMetaPath = String(ONBOARD_MEDIA_DIR) + "/.capture.meta.tmp";
  fs.remove(tempImagePath);
  fs.remove(tempMetaPath);
  File imageFile = fs.open(tempImagePath, FILE_WRITE);
  const size_t imageWritten = imageFile ? imageFile.write(fb->buf, fb->len) : 0;
  if (imageFile) imageFile.close();
  esp_camera_fb_return(fb);

  OnboardMediaMetaDisk disk{};
  disk.magic = ONBOARD_MEDIA_META_MAGIC;
  disk.id = id;
  disk.recordedAt = recordedAt;
  disk.bytes = copyLen;
  disk.width = width;
  disk.height = height;
  disk.captureKind = onboardCaptureKindValue(captureKind);
  disk.capturedUptimeSec = millis() / 1000UL;
  disk.bootSessionId = bootSessionId;
  File metaFile = fs.open(tempMetaPath, FILE_WRITE);
  const size_t metaWritten = metaFile ? metaFile.write(reinterpret_cast<const uint8_t *>(&disk), sizeof(disk)) : 0;
  if (metaFile) metaFile.close();
  const bool stored = imageWritten == copyLen && metaWritten == sizeof(disk) &&
                      fs.rename(tempImagePath, imagePath) && fs.rename(tempMetaPath, metaPath);
  if (!stored) {
    fs.remove(tempImagePath);
    fs.remove(tempMetaPath);
    fs.remove(imagePath);
    fs.remove(metaPath);
    free(copy);
    ++onboardCaptureFailures;
    onboardStorageError = "storage_unavailable";
    if (captureError != nullptr) *captureError = "storage_unavailable";
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }

  ++onboardNextMediaId;
  Preferences mediaPrefs;
  if (mediaPrefs.begin("media", false)) {
    mediaPrefs.putULong("next_id", onboardNextMediaId);
    mediaPrefs.end();
  }
  ++onboardStoredPhotoCount;
  onboardLatestMediaId = id;
  onboardStorageError = "";

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

  if (savedMedia != nullptr) {
    savedMedia->id = id;
    savedMedia->recordedAt = recordedAt;
    savedMedia->bytes = copyLen;
    savedMedia->width = width;
    savedMedia->height = height;
    savedMedia->captureKind = disk.captureKind;
  }
  if (captureError != nullptr) *captureError = "";
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);

  Serial.printf("[onboard-camera] captured id=%s bytes=%u kind=%s count=%u\n",
                onboardMediaIdString(id).c_str(),
                static_cast<unsigned>(copyLen),
                onboardCaptureKindName(disk.captureKind),
                onboardCaptureCount);
  return true;
}

String buildOnboardCameraStatusJson() {
  refreshOnboardTimelapseState();
  size_t latestLen = 0;
  uint32_t count = 0;
  uint32_t failures = 0;
  uint32_t scheduleSkips = 0;
  unsigned long lastMs = 0;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreTake(onboardFrameMutex, portMAX_DELAY);
  }
  latestLen = onboardLatestJpegLen;
  count = onboardCaptureCount;
  failures = onboardCaptureFailures;
  scheduleSkips = onboardCaptureScheduleSkips;
  lastMs = onboardLastCaptureMs;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreGive(onboardFrameMutex);
  }
  OnboardMediaInfo latestInfo{};
  if (onboardStorageReady && onboardLatestMediaId != 0 && readOnboardMediaInfo(onboardLatestMediaId, latestInfo)) {
    latestLen = latestInfo.bytes;
  }
  const uint64_t storageTotal = onboardStorageReady ? persistentStorageTotalBytes() : 0;
  const uint64_t storageUsed = onboardStorageReady ? persistentStorageUsedBytes() : 0;

  const int localMinute = onboardLocalMinuteOfDay();
  String payload = "{";
  payload += "\"ready\":" + String(onboardCameraReady ? "true" : "false");
  payload += ",\"enabled\":" + String(onboardCaptureEnabled ? "true" : "false");
  payload += ",\"interval_ms\":" + String(onboardCaptureIntervalMs);
  payload += ",\"window_start\":\"" + minuteOfDayToString(onboardCaptureStartMinute) + "\"";
  payload += ",\"window_end\":\"" + minuteOfDayToString(onboardCaptureEndMinute) + "\"";
  payload += ",\"window_start_minute\":" + String(onboardCaptureStartMinute);
  payload += ",\"window_end_minute\":" + String(onboardCaptureEndMinute);
  payload += ",\"tz_offset_min\":" + String(onboardCaptureTzOffsetMin);
  payload += ",\"clock_valid\":" + String(onboardClockValid() ? "true" : "false");
  payload += ",\"schedule_mode\":\"" + String(onboardClockValid() ? "clock_window" : "uptime_fallback") + "\"";
  payload += ",\"local_minute\":" + String(localMinute);
  payload += ",\"window_active\":" + String(onboardCaptureWindowActive() ? "true" : "false");
  payload += ",\"framesize\":\"" + String(frameSizeName(onboardFrameSize)) + "\"";
  payload += ",\"jpeg_quality\":" + String(onboardJpegQuality);
  payload += ",\"psram_found\":" + String(psramFound() ? "true" : "false");
  payload += ",\"psram_size\":" + String(ESP.getPsramSize());
  payload += ",\"psram_free\":" + String(ESP.getFreePsram());
  payload += ",\"brightness\":" + String(onboardBrightness);
  payload += ",\"contrast\":" + String(onboardContrast);
  payload += ",\"saturation\":" + String(onboardSaturation);
  payload += ",\"sharpness\":" + String(onboardSharpness);
  payload += ",\"vflip\":" + String(onboardVflip ? "true" : "false");
  payload += ",\"hmirror\":" + String(onboardHmirror ? "true" : "false");
  payload += ",\"awb\":" + String(onboardAwb ? "true" : "false");
  payload += ",\"awb_gain\":" + String(onboardAwbGain ? "true" : "false");
  payload += ",\"wb_mode\":" + String(onboardWbMode);
  payload += ",\"aec\":" + String(onboardAec ? "true" : "false");
  payload += ",\"aec2\":" + String(onboardAec2 ? "true" : "false");
  payload += ",\"ae_level\":" + String(onboardAeLevel);
  payload += ",\"aec_value\":" + String(onboardAecValue);
  payload += ",\"agc\":" + String(onboardAgc ? "true" : "false");
  payload += ",\"agc_gain\":" + String(onboardAgcGain);
  payload += ",\"special_effect\":" + String(onboardSpecialEffect);
  payload += ",\"storage_ready\":" + String(onboardStorageReady ? "true" : "false");
  payload += ",\"storage_type\":\"" + String(persistentStorageType()) + "\"";
  payload += ",\"storage_total_bytes\":" + String(static_cast<unsigned long long>(storageTotal));
  payload += ",\"storage_used_bytes\":" + String(static_cast<unsigned long long>(storageUsed));
  payload += ",\"storage_free_bytes\":" + String(static_cast<unsigned long long>(storageTotal >= storageUsed ? storageTotal - storageUsed : 0));
  payload += ",\"sd\":" + buildSdStatusJson();
  payload += ",\"stored_photo_count\":" + String(onboardStoredPhotoCount);
  payload += ",\"latest_media_id\":" + (onboardLatestMediaId == 0 ? String("null") : "\"" + onboardMediaIdString(onboardLatestMediaId) + "\"");
  payload += ",\"latest_bytes\":" + String(static_cast<unsigned>(latestLen));
  payload += ",\"captures\":" + String(count);
  payload += ",\"failures\":" + String(failures);
  payload += ",\"schedule_skips\":" + String(scheduleSkips);
  payload += ",\"last_capture_age_ms\":" + String(msSince(lastMs));
  payload += ",\"timelapse\":{";
  payload += "\"active\":" + String(onboardTimelapseActive ? "true" : "false");
  payload += ",\"state\":\"" + jsonEscape(onboardTimelapseLastState) + "\"";
  payload += ",\"interval_ms\":" + String(onboardTimelapseIntervalMs);
  payload += ",\"duration_ms\":" + String(onboardTimelapseDurationMs);
  payload += ",\"elapsed_ms\":" + String(onboardTimelapseElapsedMs());
  payload += ",\"remaining_ms\":" + String(onboardTimelapseRemainingMs());
  payload += ",\"captures\":" + String(onboardTimelapseCaptureCount);
  payload += ",\"completed_count\":" + String(onboardTimelapseCompletedCount);
  payload += ",\"started_age_ms\":" + String(msSince(onboardTimelapseStartedMs));
  payload += ",\"last_capture_age_ms\":" + String(msSince(onboardTimelapseLastCaptureMs));
  payload += "}";
  payload += "}";
  return payload;
}

bool applyOnboardCameraSensorArgs(String &error, bool persist) {
  if (!serverHasOnboardSensorArgs()) {
    return true;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    error = "sensor_unavailable";
    return false;
  }

  if (server.hasArg("framesize")) {
    framesize_t requested;
    if (!parseFrameSize(server.arg("framesize"), requested)) {
      error = "invalid_framesize";
      return false;
    }
    if (!psramFound() && requested > FRAMESIZE_SVGA) {
      error = "framesize_requires_psram";
      return false;
    }
    if (s->set_framesize(s, requested) == 0) {
      onboardFrameSize = requested;
    }
  }
  if (server.hasArg("jpeg_quality") || server.hasArg("quality")) {
    const int requested = server.hasArg("jpeg_quality") ? server.arg("jpeg_quality").toInt() : server.arg("quality").toInt();
    if (requested < 4 || requested > 63) {
      error = "invalid_jpeg_quality";
      return false;
    }
    if (s->set_quality(s, requested) == 0) {
      onboardJpegQuality = requested;
    }
  }
  if (server.hasArg("brightness")) {
    onboardBrightness = server.arg("brightness").toInt();
    s->set_brightness(s, onboardBrightness);
  }
  if (server.hasArg("contrast")) {
    onboardContrast = server.arg("contrast").toInt();
    s->set_contrast(s, onboardContrast);
  }
  if (server.hasArg("saturation")) {
    onboardSaturation = server.arg("saturation").toInt();
    s->set_saturation(s, onboardSaturation);
  }
  if (server.hasArg("sharpness")) {
    onboardSharpness = server.arg("sharpness").toInt();
    s->set_sharpness(s, onboardSharpness);
  }
  if (server.hasArg("vflip")) {
    onboardVflip = boolLikeValue(server.arg("vflip"));
    s->set_vflip(s, onboardVflip);
  }
  if (server.hasArg("hmirror")) {
    onboardHmirror = boolLikeValue(server.arg("hmirror"));
    s->set_hmirror(s, onboardHmirror);
  }
  if (server.hasArg("awb")) {
    onboardAwb = boolLikeValue(server.arg("awb"));
    s->set_whitebal(s, onboardAwb);
  }
  if (server.hasArg("awb_gain")) {
    onboardAwbGain = boolLikeValue(server.arg("awb_gain"));
    s->set_awb_gain(s, onboardAwbGain);
  }
  if (server.hasArg("wb_mode")) {
    onboardWbMode = server.arg("wb_mode").toInt();
    s->set_wb_mode(s, onboardWbMode);
  }
  if (server.hasArg("aec")) {
    onboardAec = boolLikeValue(server.arg("aec"));
    s->set_exposure_ctrl(s, onboardAec);
  }
  if (server.hasArg("aec2")) {
    onboardAec2 = boolLikeValue(server.arg("aec2"));
    s->set_aec2(s, onboardAec2);
  }
  if (server.hasArg("ae_level")) {
    onboardAeLevel = server.arg("ae_level").toInt();
    s->set_ae_level(s, onboardAeLevel);
  }
  if (server.hasArg("aec_value")) {
    onboardAecValue = server.arg("aec_value").toInt();
    s->set_aec_value(s, onboardAecValue);
  }
  if (server.hasArg("agc")) {
    onboardAgc = boolLikeValue(server.arg("agc"));
    s->set_gain_ctrl(s, onboardAgc);
  }
  if (server.hasArg("agc_gain")) {
    onboardAgcGain = server.arg("agc_gain").toInt();
    s->set_agc_gain(s, onboardAgcGain);
  }
  if (server.hasArg("special_effect")) {
    onboardSpecialEffect = server.arg("special_effect").toInt();
    s->set_special_effect(s, onboardSpecialEffect);
  }
  if (persist) {
    saveOnboardConfig();
  }
  return true;
}

bool serialBoolValue(const String &value) {
  return boolLikeValue(value);
}

bool applyOnboardCameraSetting(const String &key, const String &value, String &error) {
  if (key == "enabled") {
    onboardCaptureEnabled = serialBoolValue(value);
    saveOnboardConfig();
    return true;
  }
  if (key == "interval_ms") {
    const unsigned long requested = value.toInt();
    if (requested < 5000UL) {
      error = "invalid_interval_ms";
      return false;
    }
    onboardCaptureIntervalMs = requested;
    saveOnboardConfig();
    return true;
  }
  if (key == "start" || key == "start_minute") {
    uint16_t requested = 0;
    if (!parseMinuteOfDay(value, requested)) {
      error = "invalid_start";
      return false;
    }
    onboardCaptureStartMinute = requested;
    saveOnboardConfig();
    return true;
  }
  if (key == "end" || key == "end_minute") {
    uint16_t requested = 0;
    if (!parseMinuteOfDay(value, requested)) {
      error = "invalid_end";
      return false;
    }
    onboardCaptureEndMinute = requested;
    saveOnboardConfig();
    return true;
  }
  if (key == "tz_offset_min") {
    onboardCaptureTzOffsetMin = static_cast<int16_t>(value.toInt());
    saveOnboardConfig();
    return true;
  }
  if (key == "epoch") {
    const time_t epoch = static_cast<time_t>(value.toInt());
    if (epoch < 1700000000) {
      error = "invalid_epoch";
      return false;
    }
    timeval tv{};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    backfillOnboardMediaTimestamps();
    return true;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    error = "sensor_unavailable";
    return false;
  }
  if (key == "framesize") {
    framesize_t requested;
    if (!parseFrameSize(value, requested)) {
      error = "invalid_framesize";
      return false;
    }
    if (!psramFound() && requested > FRAMESIZE_SVGA) {
      error = "framesize_requires_psram";
      return false;
    }
    if (s->set_framesize(s, requested) == 0) {
      onboardFrameSize = requested;
      saveOnboardConfig();
    }
    return true;
  }
  if (key == "jpeg_quality" || key == "quality") {
    const int requested = value.toInt();
    if (requested < 4 || requested > 63) {
      error = "invalid_jpeg_quality";
      return false;
    }
    if (s->set_quality(s, requested) == 0) {
      onboardJpegQuality = requested;
      saveOnboardConfig();
    }
    return true;
  }
  if (key == "brightness") {
    onboardBrightness = value.toInt();
    s->set_brightness(s, onboardBrightness);
  } else if (key == "contrast") {
    onboardContrast = value.toInt();
    s->set_contrast(s, onboardContrast);
  } else if (key == "saturation") {
    onboardSaturation = value.toInt();
    s->set_saturation(s, onboardSaturation);
  } else if (key == "sharpness") {
    onboardSharpness = value.toInt();
    s->set_sharpness(s, onboardSharpness);
  } else if (key == "vflip") {
    onboardVflip = serialBoolValue(value);
    s->set_vflip(s, onboardVflip);
  } else if (key == "hmirror") {
    onboardHmirror = serialBoolValue(value);
    s->set_hmirror(s, onboardHmirror);
  } else if (key == "awb") {
    onboardAwb = serialBoolValue(value);
    s->set_whitebal(s, onboardAwb);
  } else if (key == "awb_gain") {
    onboardAwbGain = serialBoolValue(value);
    s->set_awb_gain(s, onboardAwbGain);
  } else if (key == "wb_mode") {
    onboardWbMode = value.toInt();
    s->set_wb_mode(s, onboardWbMode);
  } else if (key == "aec") {
    onboardAec = serialBoolValue(value);
    s->set_exposure_ctrl(s, onboardAec);
  } else if (key == "aec2") {
    onboardAec2 = serialBoolValue(value);
    s->set_aec2(s, onboardAec2);
  } else if (key == "ae_level") {
    onboardAeLevel = value.toInt();
    s->set_ae_level(s, onboardAeLevel);
  } else if (key == "aec_value") {
    onboardAecValue = value.toInt();
    s->set_aec_value(s, onboardAecValue);
  } else if (key == "agc") {
    onboardAgc = serialBoolValue(value);
    s->set_gain_ctrl(s, onboardAgc);
  } else if (key == "agc_gain") {
    onboardAgcGain = value.toInt();
    s->set_agc_gain(s, onboardAgcGain);
  } else if (key == "special_effect") {
    onboardSpecialEffect = value.toInt();
    s->set_special_effect(s, onboardSpecialEffect);
  } else {
    error = "unknown_setting";
    return false;
  }
  saveOnboardConfig();
  return true;
}

bool applyOnboardTimelapseSerialArg(const String &key, const String &value, unsigned long &durationMs, unsigned long &intervalMs, String &error) {
  if (key == "interval_ms") {
    intervalMs = strtoul(value.c_str(), nullptr, 10);
    if (intervalMs < ONBOARD_TIMELAPSE_MIN_INTERVAL_MS) {
      error = "invalid_interval_ms";
      return false;
    }
    return true;
  }
  if (key == "duration_ms") {
    durationMs = strtoul(value.c_str(), nullptr, 10);
    return true;
  }
  if (key == "duration_minutes" || key == "minutes") {
    const float minutes = value.toFloat();
    if (minutes <= 0) {
      error = "invalid_duration";
      return false;
    }
    durationMs = static_cast<unsigned long>(minutes * 60.0f * 1000.0f);
    return true;
  }
  if (key == "duration_hours" || key == "hours") {
    const float hours = value.toFloat();
    if (hours <= 0) {
      error = "invalid_duration";
      return false;
    }
    durationMs = static_cast<unsigned long>(hours * 60.0f * 60.0f * 1000.0f);
    return true;
  }
  error = "unknown_timelapse_arg";
  return false;
}

void printBase64Bytes(const uint8_t *data, size_t len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t lineChars = 0;
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t octetA = data[i];
    const uint32_t octetB = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t octetC = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;
    char out[4];
    out[0] = alphabet[(triple >> 18) & 0x3F];
    out[1] = alphabet[(triple >> 12) & 0x3F];
    out[2] = (i + 1 < len) ? alphabet[(triple >> 6) & 0x3F] : '=';
    out[3] = (i + 2 < len) ? alphabet[triple & 0x3F] : '=';
    Serial.write(reinterpret_cast<const uint8_t *>(out), sizeof(out));
    lineChars += 4;
    if (lineChars >= 76) {
      Serial.println();
      lineChars = 0;
    }
  }
  if (lineChars > 0) {
    Serial.println();
  }
}

void dumpOnboardJpegBase64(bool freshCapture) {
  if (freshCapture) {
    if (!captureOnboardFrame()) {
      Serial.println("ONBOARD_JPEG_ERROR capture_failed");
      return;
    }
  }
  uint8_t *buf = nullptr;
  size_t len = 0;
  if (onboardFrameMutex != nullptr) {
    xSemaphoreTake(onboardFrameMutex, portMAX_DELAY);
  }
  buf = onboardLatestJpeg;
  len = onboardLatestJpegLen;
  if (buf == nullptr || len == 0) {
    if (onboardFrameMutex != nullptr) {
      xSemaphoreGive(onboardFrameMutex);
    }
    Serial.println("ONBOARD_JPEG_ERROR no_capture");
    return;
  }
  Serial.printf("BEGIN_ONBOARD_JPEG_BASE64 bytes=%u\n", static_cast<unsigned>(len));
  printBase64Bytes(buf, len);
  Serial.println("END_ONBOARD_JPEG_BASE64");
  if (onboardFrameMutex != nullptr) {
    xSemaphoreGive(onboardFrameMutex);
  }
}

void onboardCaptureTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    refreshOnboardTimelapseState();
    if (onboardCameraReady) {
      const unsigned long nowMs = millis();
      if (onboardTimelapseActive) {
        if (onboardTimelapseLastCaptureMs == 0 ||
            nowMs - onboardTimelapseLastCaptureMs >= onboardTimelapseIntervalMs) {
          if (captureOnboardFrame("timelapse")) {
            onboardTimelapseLastCaptureMs = millis();
            ++onboardTimelapseCaptureCount;
          }
        }
      } else if (onboardCaptureEnabled) {
        const unsigned long normalIntervalMs = onboardCaptureIntervalMs < 5000UL ? 5000UL : onboardCaptureIntervalMs;
        if (onboardLastScheduleAttemptMs == 0 ||
            nowMs - onboardLastScheduleAttemptMs >= normalIntervalMs) {
          if (!onboardClockValid()) {
            onboardLastScheduleAttemptMs = nowMs;
            Serial.println("[onboard-camera] scheduled capture using uptime fallback clock_valid=no");
            captureOnboardFrame("scheduled");
          } else if (onboardCaptureWindowActive()) {
            onboardLastScheduleAttemptMs = nowMs;
            captureOnboardFrame("scheduled");
          } else {
            onboardLastScheduleAttemptMs = nowMs;
            ++onboardCaptureScheduleSkips;
            Serial.printf("[onboard-camera] scheduled capture skipped clock_valid=%s local_minute=%d window=%s-%s\n",
                          onboardClockValid() ? "yes" : "no",
                          onboardLocalMinuteOfDay(),
                          minuteOfDayToString(onboardCaptureStartMinute).c_str(),
                          minuteOfDayToString(onboardCaptureEndMinute).c_str());
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(ONBOARD_SCHEDULER_TICK_MS));
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

int wifiChannelFrequencyMhz(int channel) {
  if (channel == 14) return 2484;
  if (channel >= 1 && channel <= 13) return 2407 + channel * 5;
  return 0;
}

String buildWifiObservationsJson(int scanCount) {
  String payload = "{";
  payload += "\"scanner_host\":\"" + jsonEscape(SCANNER_HOST) + "\"";
  payload += ",\"health\":{";
  payload += "\"mac\":\"" + WiFi.macAddress() + "\"";
  payload += ",\"free_heap\":" + String(ESP.getFreeHeap());
  payload += ",\"min_free_heap\":" + String(ESP.getMinFreeHeap());
  payload += ",\"uptime_ms\":" + String(millis());
  payload += ",\"temperature_c\":" + String(readChipTemperatureC(), 1);
  payload += "},\"observations\":[";
  for (int i = 0; i < scanCount; ++i) {
    if (i > 0) payload += ",";
    const int channel = WiFi.channel(i);
    payload += "{";
    payload += "\"mac\":\"" + WiFi.BSSIDstr(i) + "\"";
    payload += ",\"device_type\":\"AP\"";
    payload += ",\"interface\":\"esp32-wifi\"";
    payload += ",\"signal_dbm\":" + String(WiFi.RSSI(i));
    payload += ",\"channel\":" + String(channel);
    payload += ",\"freq_mhz\":" + String(wifiChannelFrequencyMhz(channel));
    payload += ",\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
    payload += ",\"ht\":false,\"vht\":false,\"he\":false";
    payload += ",\"probe_count\":1";
    payload += ",\"recorded_at\":" + buildRecordedAtJsonValue();
    payload += "}";
  }
  payload += "]}";
  return payload;
}

bool runIdleWifiScan(String &payload, String &error, int &scanCode) {
  scanCode = 0;
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
  WiFi.scanDelete();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  delay(200);
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    scanCode = count;
    error = "scan_failed";
    Serial.printf("[WiFi] idle scan failed code=%d\n", count);
    WiFi.scanDelete();
    WiFi.mode(previousMode);
    wifiScanBusy = false;
    return false;
  }
  payload = buildWifiScanJson(count);
  wifiObservationsLastJson = buildWifiObservationsJson(count);
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

String jsonNullableString(const String &value) {
  if (value.isEmpty()) {
    return "null";
  }
  return "\"" + jsonEscape(value) + "\"";
}

String buildRecordedAtJsonValue() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return "null";
  struct tm tmValue{};
  setenv("TZ", BOARD_TIMEZONE, 1);
  tzset();
  localtime_r(&now, &tmValue);
  char formatted[24];
  // air_scan passes this string directly to MySQL DATETIME, which rejects a
  // timezone suffix. Emit local wall time to match the database/server clock.
  strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%S", &tmValue);
  return "\"" + String(formatted) + "\"";
}

void setUploadResult(bool ok, int statusCode, const String &message) {
  uploadLastStatusCode = statusCode;
  uploadLastMessage = message;
  uploadLastAttemptMs = millis();
  if (ok) {
    ++uploadSuccessCount;
    uploadLastSuccessMs = millis();
  } else {
    ++uploadFailureCount;
  }
}

void enqueueUploadEvent(const String &type, const String &reason, const String &detailsJson, bool ok = true) {
  String event = "{";
  event += "\"type\":\"" + jsonEscape(type) + "\"";
  event += ",\"ok\":" + String(ok ? "true" : "false");
  if (!reason.isEmpty()) {
    event += ",\"reason\":\"" + jsonEscape(reason) + "\"";
  }
  event += ",\"uptime_ms\":" + String(millis());
  event += ",\"details\":" + (detailsJson.isEmpty() ? String("{}") : detailsJson);
  event += "}";

  const size_t slot = (uploadEventHead + uploadEventCount) % UPLOAD_EVENT_QUEUE_SIZE;
  uploadEventQueue[slot] = event;
  if (uploadEventCount < UPLOAD_EVENT_QUEUE_SIZE) {
    ++uploadEventCount;
  } else {
    uploadEventHead = (uploadEventHead + 1) % UPLOAD_EVENT_QUEUE_SIZE;
  }
}

String buildBoardTelemetryJson() {
  const uint32_t adcMv = readBatteryAdcMilliVolts();
  const float batteryV = (adcMv / 1000.0f) * 2.0f;
  const float chipTemperatureC = readChipTemperatureC();
  String payload = "{";
  payload += "\"scanner_host\":\"" + jsonEscape(SCANNER_HOST) + "\"";
  payload += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  payload += ",\"recorded_at\":" + buildRecordedAtJsonValue();
  payload += ",\"firmware\":{";
  payload += "\"name\":\"" + jsonEscape(FIRMWARE_NAME) + "\"";
  payload += ",\"version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"";
  payload += ",\"build\":\"" + jsonEscape(FIRMWARE_BUILD) + "\"";
  payload += "}";
  payload += ",\"board\":{";
  payload += "\"mac\":\"" + jsonEscape(WiFi.macAddress()) + "\"";
  payload += ",\"uptime_ms\":" + String(millis());
  payload += ",\"boot_count\":" + String(persistentBootCount);
  payload += ",\"boot_session_id\":" + String(bootSessionId);
  payload += ",\"free_heap\":" + String(ESP.getFreeHeap());
  payload += ",\"min_free_heap\":" + String(ESP.getMinFreeHeap());
  payload += ",\"psram_free\":" + String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  payload += ",\"chip_temperature_c\":" + String(chipTemperatureC, 1);
  payload += "}";
  payload += ",\"battery\":{";
  payload += "\"adc_mv\":" + String(adcMv);
  payload += ",\"battery_est_v\":" + String(batteryV, 3);
  payload += ",\"charging_gpio15\":" + String(digitalRead(BAT_CHRG_PIN));
  payload += ",\"done_gpio16\":" + String(digitalRead(BAT_DONE_PIN));
  payload += "}";
  payload += ",\"temperature\":{";
  payload += "\"probe_attached\":false";
  payload += ",\"sensor\":null";
  payload += ",\"temperature_c\":null";
  payload += ",\"chip_temperature_c\":" + String(chipTemperatureC, 1);
  payload += "}";
  payload += ",\"radio\":{";
  payload += "\"halow_connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"halow_rssi\":" + String(static_cast<int>(HaLow.RSSI()));
  payload += ",\"halow\":" + buildHaLowStatusJson();
  payload += ",\"trail_wifi_connected\":" + String(wifiConnected ? "true" : "false");
  payload += "}";
  payload += "}";
  return payload;
}

String buildEventsUploadJson() {
  String payload = "{";
  payload += "\"scanner_host\":\"" + jsonEscape(SCANNER_HOST) + "\"";
  payload += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  payload += ",\"recorded_at\":" + buildRecordedAtJsonValue();
  payload += ",\"events\":[";
  for (size_t i = 0; i < uploadEventCount; ++i) {
    if (i > 0) {
      payload += ",";
    }
    const size_t slot = (uploadEventHead + i) % UPLOAD_EVENT_QUEUE_SIZE;
    payload += uploadEventQueue[slot];
  }
  payload += "]}";
  return payload;
}

void clearUploadedEvents() {
  for (size_t i = 0; i < uploadEventCount; ++i) {
    const size_t slot = (uploadEventHead + i) % UPLOAD_EVENT_QUEUE_SIZE;
    uploadEventQueue[slot] = "";
  }
  uploadEventHead = 0;
  uploadEventCount = 0;
}

String buildUploadStatusJson() {
  String payload = "{";
  payload += "\"scanner_host\":\"" + jsonEscape(SCANNER_HOST) + "\"";
  payload += ",\"api_host\":\"" + jsonEscape(UPSTREAM_API_HOST) + "\"";
  payload += ",\"api_port\":" + String(UPSTREAM_API_PORT);
  payload += ",\"api_prefix\":\"" + jsonEscape(UPSTREAM_API_PREFIX) + "\"";
  payload += ",\"observations_api_host\":\"" + jsonEscape(AIR_SCAN_API_HOST) + "\"";
  payload += ",\"observations_api_port\":" + String(AIR_SCAN_API_PORT);
  payload += ",\"attempts\":" + String(uploadAttemptCount);
  payload += ",\"successes\":" + String(uploadSuccessCount);
  payload += ",\"failures\":" + String(uploadFailureCount);
  payload += ",\"last_status_code\":" + String(uploadLastStatusCode);
  payload += ",\"last_message\":\"" + jsonEscape(uploadLastMessage) + "\"";
  payload += ",\"last_attempt_age_ms\":" + String(msSince(uploadLastAttemptMs));
  payload += ",\"last_success_age_ms\":" + String(msSince(uploadLastSuccessMs));
  payload += ",\"queued_events\":" + String(static_cast<unsigned>(uploadEventCount));
  payload += ",\"observation_queue\":{";
  payload += "\"storage_type\":\"" + String(persistentStorageType()) + "\"";
  payload += ",\"queued_batches\":" + String(static_cast<unsigned>(observationQueueCachedCount));
  payload += ",\"queued_bytes\":" + String(static_cast<unsigned>(observationQueueCachedBytes));
  payload += ",\"enqueue_count\":" + String(observationQueueEnqueueCount);
  payload += ",\"drop_count\":" + String(observationQueueDropCount);
  payload += ",\"replay_successes\":" + String(observationQueueReplaySuccessCount);
  payload += ",\"replay_failures\":" + String(observationQueueReplayFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(observationQueueLastError) + "\"";
  payload += "}";
  payload += ",\"wifi_scanner\":{";
  payload += "\"runs\":" + String(wifiScannerRunCount);
  payload += ",\"upload_successes\":" + String(wifiScannerUploadSuccessCount);
  payload += ",\"upload_failures\":" + String(wifiScannerUploadFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(wifiScannerLastError) + "\"";
  payload += ",\"last_run_age_ms\":" + String(msSince(wifiScannerLastRunMs));
  payload += ",\"last_upload_age_ms\":" + String(msSince(wifiScannerLastUploadMs));
  payload += ",\"day_interval_ms\":" + String(WIFI_SCAN_DAY_INTERVAL_MS);
  payload += ",\"night_interval_ms\":" + String(WIFI_SCAN_NIGHT_INTERVAL_MS);
  payload += "}";
  payload += ",\"ble_scanner\":{";
  payload += "\"runs\":" + String(bleScannerRunCount);
  payload += ",\"last_count\":" + String(bleScannerLastCount);
  payload += ",\"upload_successes\":" + String(bleScannerUploadSuccessCount);
  payload += ",\"upload_failures\":" + String(bleScannerUploadFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(bleScannerLastError) + "\"";
  payload += ",\"last_run_age_ms\":" + String(msSince(bleScannerLastRunMs));
  payload += ",\"last_upload_age_ms\":" + String(msSince(bleScannerLastUploadMs));
  payload += ",\"last_manufacturer_count\":" + String(bleScannerLastManufacturerCount);
  payload += ",\"last_services_count\":" + String(bleScannerLastServicesCount);
  payload += ",\"last_service_data_count\":" + String(bleScannerLastServiceDataCount);
  payload += ",\"last_tx_power_count\":" + String(bleScannerLastTxPowerCount);
  payload += ",\"last_name_count\":" + String(bleScannerLastNameCount);
  payload += "}";
  payload += "}";
  return payload;
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
    if (controlState.activeAction == CONTROL_ACTION_BRINGUP &&
        action == CONTROL_ACTION_STREAM_START &&
        controlState.pendingAction == CONTROL_ACTION_NONE) {
      controlState.pendingAction = action;
      accepted = true;
      messageType = "queued_after";
      messageAction = controlState.activeAction;
    } else {
      messageType = "busy";
      messageAction = controlState.activeAction;
    }
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

  String details = "{";
  details += "\"action\":\"" + String(controlActionName(action)) + "\"";
  details += ",\"message\":\"" + jsonEscape(message == nullptr ? String("") : String(message)) + "\"";
  details += ",\"ble_stage\":\"" + jsonEscape(bleStage) + "\"";
  details += ",\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  details += ",\"stream_active\":" + String(streamSessionActive ? "true" : "false");
  details += "}";
  enqueueUploadEvent("control_action_finished", controlActionName(action), details, ok);
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
  bleDataChar4->writeValue(reinterpret_cast<const uint8_t *>(CAMERA_BLE_WAKE),
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
  const unsigned long wakeStartedMs = millis();
  if (bleClient == nullptr || !bleClient->isConnected() || bleDataChar4 == nullptr) {
    lastBleWakeElapsedMs = millis() - wakeStartedMs;
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
  lastBleWakeElapsedMs = millis() - wakeStartedMs;
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
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
  }
  bleNotifyChar3 = nullptr;
  bleDataChar4 = nullptr;
}

void refreshWifiState() {
  wifiConnected = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress((uint32_t)0));
}

static void bleNotifyCallback(NimBLERemoteCharacteristic *characteristic,
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

class BridgeBleClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *client) override {
    Serial.printf("[BLE] connected to %s\n", client->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient *client, int reason) override {
    Serial.printf("[BLE] disconnected from %s reason=%d\n", client->getPeerAddress().toString().c_str(), reason);
  }
};

class BridgeBleAdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    String mac = advertisedDevice->getAddress().toString().c_str();
    mac.toLowerCase();
    String name = "";
    if (advertisedDevice->haveName()) {
      name = advertisedDevice->getName().c_str();
    }
    String serviceUuid = "";
    if (advertisedDevice->haveServiceUUID()) {
      serviceUuid = advertisedDevice->getServiceUUID().toString().c_str();
      serviceUuid.toLowerCase();
    }
    const int rssi = advertisedDevice->getRSSI();
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
      bleTargetDevice = *advertisedDevice;
      bleTargetDeviceFound = true;
      NimBLEDevice::getScan()->stop();
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

String bytesToHex(const std::string &data) {
  static const char hex[] = "0123456789abcdef";
  String output;
  output.reserve(data.size() * 2);
  for (uint8_t value : data) {
    output += hex[(value >> 4) & 0x0f];
    output += hex[value & 0x0f];
  }
  return output;
}

String bytesToHex(const uint8_t *data, size_t length) {
  static const char hex[] = "0123456789abcdef";
  String output;
  output.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    const uint8_t value = data[i];
    output += hex[(value >> 4) & 0x0f];
    output += hex[value & 0x0f];
  }
  return output;
}

String normalizeBleUuid(String uuid) {
  uuid.toLowerCase();
  if (uuid.startsWith("0x")) {
    uuid = uuid.substring(2);
  }
  if (uuid.length() == 4) {
    return "0000" + uuid + "-0000-1000-8000-00805f9b34fb";
  }
  if (uuid.length() == 8) {
    return uuid + "-0000-1000-8000-00805f9b34fb";
  }
  return uuid;
}

String formatBleUuid16(uint16_t uuid) {
  char text[37];
  snprintf(text, sizeof(text), "0000%04x-0000-1000-8000-00805f9b34fb", uuid);
  return String(text);
}

String formatBleUuid32(uint32_t uuid) {
  char text[37];
  snprintf(text, sizeof(text), "%08lx-0000-1000-8000-00805f9b34fb", static_cast<unsigned long>(uuid));
  return String(text);
}

String formatBleUuid128(const uint8_t *data) {
  char text[37];
  snprintf(text, sizeof(text),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           data[15], data[14], data[13], data[12],
           data[11], data[10],
           data[9], data[8],
           data[7], data[6],
           data[5], data[4], data[3], data[2], data[1], data[0]);
  return String(text);
}

void appendCsvValue(String &target, const String &value) {
  if (value.isEmpty()) return;
  if (target.isEmpty()) {
    target = value;
    return;
  }
  int start = 0;
  while (start < target.length()) {
    int end = target.indexOf(',', start);
    if (end < 0) end = target.length();
    if (target.substring(start, end) == value) return;
    start = end + 1;
  }
  target += ",";
  target += value;
}

void truncateBleField(String &value) {
  if (value.length() > BLE_OBSERVATION_FIELD_LIMIT) {
    value.remove(BLE_OBSERVATION_FIELD_LIMIT);
  }
}

String formatBleManufacturerData(const NimBLEAdvertisedDevice *device) {
  String output;
  for (uint8_t i = 0; i < device->getManufacturerDataCount(); ++i) {
    const std::string data = device->getManufacturerData(i);
    if (data.size() < 2) continue;
    if (!output.isEmpty()) output += ",";
    const uint16_t companyId = static_cast<uint8_t>(data[0]) |
                               (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
    char company[6];
    snprintf(company, sizeof(company), "%04X:", companyId);
    output += company;
    output += bytesToHex(data.substr(2));
  }
  return output;
}

void mergeBlePayloadFields(BleObservationEntry &entry, const uint8_t *payload, size_t payloadLength) {
  size_t index = 0;
  while (index < payloadLength) {
    const uint8_t length = payload[index];
    if (length == 0) break;
    if (length < 1 || length > payloadLength - index - 1) break;
    const uint8_t type = payload[index + 1];
    const uint8_t *data = &payload[index + 2];
    const size_t dataLength = length - 1;

    if (type == 0xff && dataLength >= 2) {
      const uint16_t companyId = static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8);
      char company[6];
      snprintf(company, sizeof(company), "%04X:", companyId);
      String value = String(company) + bytesToHex(data + 2, dataLength - 2);
      truncateBleField(value);
      appendCsvValue(entry.manufacturerData, value);
    } else if ((type == 0x02 || type == 0x03) && dataLength >= 2) {
      for (size_t pos = 0; pos + 1 < dataLength; pos += 2) {
        appendCsvValue(entry.advServices,
                       formatBleUuid16(static_cast<uint16_t>(data[pos]) |
                                       (static_cast<uint16_t>(data[pos + 1]) << 8)));
      }
    } else if ((type == 0x04 || type == 0x05) && dataLength >= 4) {
      for (size_t pos = 0; pos + 3 < dataLength; pos += 4) {
        appendCsvValue(entry.advServices,
                       formatBleUuid32(static_cast<uint32_t>(data[pos]) |
                                       (static_cast<uint32_t>(data[pos + 1]) << 8) |
                                       (static_cast<uint32_t>(data[pos + 2]) << 16) |
                                       (static_cast<uint32_t>(data[pos + 3]) << 24)));
      }
    } else if ((type == 0x06 || type == 0x07) && dataLength >= 16) {
      for (size_t pos = 0; pos + 15 < dataLength; pos += 16) {
        appendCsvValue(entry.advServices, formatBleUuid128(data + pos));
      }
    } else if (type == 0x16 && dataLength >= 2) {
      String value = formatBleUuid16(static_cast<uint16_t>(data[0]) |
                                     (static_cast<uint16_t>(data[1]) << 8));
      value += ":";
      value += bytesToHex(data + 2, dataLength - 2);
      truncateBleField(value);
      appendCsvValue(entry.advServiceData, value);
    } else if (type == 0x20 && dataLength >= 4) {
      String value = formatBleUuid32(static_cast<uint32_t>(data[0]) |
                                     (static_cast<uint32_t>(data[1]) << 8) |
                                     (static_cast<uint32_t>(data[2]) << 16) |
                                     (static_cast<uint32_t>(data[3]) << 24));
      value += ":";
      value += bytesToHex(data + 4, dataLength - 4);
      truncateBleField(value);
      appendCsvValue(entry.advServiceData, value);
    } else if (type == 0x21 && dataLength >= 16) {
      String value = formatBleUuid128(data);
      value += ":";
      value += bytesToHex(data + 16, dataLength - 16);
      truncateBleField(value);
      appendCsvValue(entry.advServiceData, value);
    } else if ((type == 0x08 || type == 0x09) && dataLength > 0 && entry.localName.isEmpty()) {
      String value;
      value.reserve(dataLength);
      for (size_t pos = 0; pos < dataLength; ++pos) {
        const char c = static_cast<char>(data[pos]);
        if (c >= 32 && c <= 126) value += c;
      }
      truncateBleField(value);
      entry.localName = value;
    } else if (type == 0x0a && dataLength >= 1) {
      entry.hasTxPower = true;
      entry.txPower = static_cast<int8_t>(data[0]);
    }

    truncateBleField(entry.manufacturerData);
    truncateBleField(entry.advServices);
    truncateBleField(entry.advServiceData);
    index += length + 1;
  }
}

void resetBleObservationEntries() {
  for (size_t i = 0; i < bleObservationEntryCount; ++i) {
    bleObservationEntries[i] = BleObservationEntry{};
  }
  bleObservationEntryCount = 0;
}

BleObservationEntry *findOrCreateBleObservationEntry(const String &mac) {
  for (size_t i = 0; i < bleObservationEntryCount; ++i) {
    if (bleObservationEntries[i].active && bleObservationEntries[i].mac == mac) {
      return &bleObservationEntries[i];
    }
  }
  if (bleObservationEntryCount >= BLE_OBSERVATION_DEVICE_LIMIT) return nullptr;
  BleObservationEntry &entry = bleObservationEntries[bleObservationEntryCount++];
  entry = BleObservationEntry{};
  entry.active = true;
  entry.mac = mac;
  entry.rssi = -127;
  return &entry;
}

void mergeBleObservation(const NimBLEAdvertisedDevice *device) {
  String mac = device->getAddress().toString().c_str();
  mac.toLowerCase();
  BleObservationEntry *entry = findOrCreateBleObservationEntry(mac);
  if (entry == nullptr) return;

  const int rssi = device->getRSSI();
  if (rssi > entry->rssi) entry->rssi = rssi;
  entry->isRandomized = device->getAddressType() != 0;
  if (device->haveTXPower()) {
    entry->hasTxPower = true;
    entry->txPower = device->getTXPower();
  }

  const std::vector<uint8_t> &payload = device->getPayload();
  if (!payload.empty()) {
    mergeBlePayloadFields(*entry, payload.data(), payload.size());
  }

  const String name = device->haveName() ? String(device->getName().c_str()) : String("");
  if (!name.isEmpty() && entry->localName.isEmpty()) {
    entry->localName = name;
    truncateBleField(entry->localName);
  }

  String manufacturerData = formatBleManufacturerData(device);
  truncateBleField(manufacturerData);
  if (!manufacturerData.isEmpty() && entry->manufacturerData.isEmpty()) {
    entry->manufacturerData = manufacturerData;
  }

  String services;
  for (uint8_t i = 0; i < device->getServiceUUIDCount(); ++i) {
    if (i > 0) services += ",";
    services += normalizeBleUuid(device->getServiceUUID(i).toString().c_str());
  }
  truncateBleField(services);
  if (!services.isEmpty()) {
    if (entry->advServices.isEmpty()) {
      entry->advServices = services;
    } else if (entry->advServices.indexOf(services) < 0) {
      String merged = entry->advServices + "," + services;
      truncateBleField(merged);
      entry->advServices = merged;
    }
  }

  String serviceData;
  for (uint8_t i = 0; i < device->getServiceDataCount(); ++i) {
    if (i > 0) serviceData += ",";
    serviceData += normalizeBleUuid(device->getServiceDataUUID(i).toString().c_str());
    serviceData += ":";
    serviceData += bytesToHex(device->getServiceData(i));
  }
  truncateBleField(serviceData);
  if (!serviceData.isEmpty() && entry->advServiceData.isEmpty()) {
    entry->advServiceData = serviceData;
  }
}

void buildBleObservationsPayload() {
  bleObservationsLastJson = "{";
  bleObservationsLastJson += "\"scanner_host\":\"" + jsonEscape(SCANNER_HOST) + "\"";
  bleObservationsLastJson += ",\"health\":{";
  bleObservationsLastJson += "\"mac\":\"" + WiFi.macAddress() + "\"";
  bleObservationsLastJson += ",\"free_heap\":" + String(ESP.getFreeHeap());
  bleObservationsLastJson += ",\"min_free_heap\":" + String(ESP.getMinFreeHeap());
  bleObservationsLastJson += ",\"uptime_ms\":" + String(millis());
  bleObservationsLastJson += ",\"temperature_c\":" + String(readChipTemperatureC(), 1);
  bleObservationsLastJson += "},\"observations\":[";

  bleScannerLastCount = 0;
  bleScannerLastManufacturerCount = 0;
  bleScannerLastServicesCount = 0;
  bleScannerLastServiceDataCount = 0;
  bleScannerLastTxPowerCount = 0;
  bleScannerLastNameCount = 0;
  for (size_t i = 0; i < bleObservationEntryCount; ++i) {
    const BleObservationEntry &entry = bleObservationEntries[i];
    if (!entry.active || entry.mac.isEmpty()) continue;
    if (bleScannerLastCount > 0) bleObservationsLastJson += ",";
    bleObservationsLastJson += "{";
    bleObservationsLastJson += "\"mac\":\"" + jsonEscape(entry.mac) + "\"";
    bleObservationsLastJson += ",\"device_type\":\"BLE\"";
    bleObservationsLastJson += ",\"interface\":\"esp32-ble\"";
    bleObservationsLastJson += ",\"signal_dbm\":" + String(entry.rssi);
    bleObservationsLastJson += ",\"channel\":null,\"freq_mhz\":null,\"ssid\":null";
    bleObservationsLastJson += ",\"local_name\":" + jsonNullableString(entry.localName);
    bleObservationsLastJson += ",\"is_randomized\":" + String(entry.isRandomized ? "true" : "false");
    bleObservationsLastJson += ",\"ht\":false,\"vht\":false,\"he\":false";
    bleObservationsLastJson += ",\"probe_count\":1";
    bleObservationsLastJson += ",\"manufacturer_data\":" + jsonNullableString(entry.manufacturerData);
    bleObservationsLastJson += ",\"adv_services\":" + jsonNullableString(entry.advServices);
    bleObservationsLastJson += ",\"adv_service_data\":" + jsonNullableString(entry.advServiceData);
    bleObservationsLastJson += ",\"tx_power\":" + (entry.hasTxPower ? String(entry.txPower) : String("null"));
    bleObservationsLastJson += ",\"recorded_at\":" + buildRecordedAtJsonValue();
    bleObservationsLastJson += "}";
    ++bleScannerLastCount;
    if (!entry.manufacturerData.isEmpty()) ++bleScannerLastManufacturerCount;
    if (!entry.advServices.isEmpty()) ++bleScannerLastServicesCount;
    if (!entry.advServiceData.isEmpty()) ++bleScannerLastServiceDataCount;
    if (entry.hasTxPower) ++bleScannerLastTxPowerCount;
    if (!entry.localName.isEmpty()) ++bleScannerLastNameCount;
  }
  bleObservationsLastJson += "]}";
}

class ObservationBleScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    mergeBleObservation(device);
  }
};

static BridgeBleClientCallbacks bridgeBleClientCallbacks;
static BridgeBleAdvertisedDeviceCallbacks bridgeBleScanCallbacks;
static ObservationBleScanCallbacks observationBleScanCallbacks;

bool runBleObservationScan(String &error) {
  if (streamSessionActive || wifiConnected || isControlActionActive()) {
    error = "camera_wifi_active";
    return false;
  }
  bleScannerLastRunMs = millis();
  ++bleScannerRunCount;
  bleScannerLastCount = 0;
  resetBleObservationEntries();

  WiFi.mode(WIFI_OFF);
  cooperativeDelay(200);

  if (bleClient != nullptr) {
    if (bleClient->isConnected()) {
      bleClient->disconnect();
      cooperativeDelay(50);
    }
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
  }
  bleNotifyChar3 = nullptr;
  bleDataChar4 = nullptr;
  NimBLEDevice::deinit(true);
  cooperativeDelay(50);
  NimBLEDevice::init(BOARD_HOSTNAME);
  bleInitialized = true;

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->stop();
  scan->clearResults();
  scan->setScanCallbacks(&observationBleScanCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);
  scan->setMaxResults(0);
  const bool started = scan->start(BLE_OBSERVATION_SCAN_WINDOW_MS, false, true);
  const unsigned long startedMs = millis();
  while (scan->isScanning() && millis() - startedMs < BLE_OBSERVATION_SCAN_WINDOW_MS + 500UL) cooperativeDelay(50);
  scan->stop();
  scan->setScanCallbacks(&bridgeBleScanCallbacks, true);
  buildBleObservationsPayload();
  if (!started) {
    error = "ble_scan_failed";
    return false;
  }
  error = "";
  return true;
}

bool connectBleTargetWithRetries() {
  bleLastConnectError = 0;
  bleConnectAttempts = 0;
  bleStage = "connect";
  cooperativeDelay(BLE_SCAN_CONNECT_SETTLE_MS);

  for (uint8_t attempt = 1; attempt <= BLE_CONNECT_ATTEMPTS; ++attempt) {
    bleConnectAttempts = attempt;
    if (bleClient != nullptr) {
      NimBLEDevice::deleteClient(bleClient);
      bleClient = nullptr;
    }

    Serial.printf("[BLE] connect attempt %u/%u\n",
                  static_cast<unsigned>(attempt),
                  static_cast<unsigned>(BLE_CONNECT_ATTEMPTS));
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(&bridgeBleClientCallbacks, false);
    bleClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
    bleClient->setConnectionParams(24, 40, 0, 400, 160, 120);
    if (bleClient->connect(&bleTargetDevice, true, false, false)) {
      bleLastConnectError = 0;
      return true;
    }

    bleLastConnectError = bleClient->getLastError();
    Serial.printf("[BLE] connect attempt %u failed last_error=%d\n",
                  static_cast<unsigned>(attempt),
                  bleLastConnectError);
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    cooperativeDelay(BLE_CONNECT_RETRY_DELAY_MS);
  }

  bleStage = "connect_failed";
  return false;
}

bool runBleDiscoveryPass(NimBLEScan *scan,
                         bool activeScan,
                         uint8_t attempts,
                         uint16_t windowSec,
                         const char *modeLabel) {
  bleScanMode = modeLabel;
  scan->setActiveScan(activeScan);
  for (uint8_t attempt = 1; attempt <= attempts && !bleTargetDeviceFound; ++attempt) {
    ++bleScanAttemptCounter;
    Serial.printf("[BLE] %s scan attempt %u/%u window=%us results=%u best_mac=%s best_rssi=%d\n",
                  modeLabel,
                  attempt,
                  attempts,
                  static_cast<unsigned>(windowSec),
                  static_cast<unsigned>(bleScanResultCount),
                  bleBestSeenMac.c_str(),
                  bleBestSeenRssi);
    scan->start(static_cast<uint32_t>(windowSec) * 1000UL, false, true);
    const unsigned long startedMs = millis();
    while (!bleTargetDeviceFound && millis() - startedMs < static_cast<unsigned long>(windowSec) * 1000UL + 250UL) {
      cooperativeDelay(50);
    }
    scan->stop();
    if (!bleTargetDeviceFound) {
      Serial.printf("[BLE] target not found in %s scan window results=%u best_mac=%s best_rssi=%d\n",
                    modeLabel,
                    static_cast<unsigned>(bleScanResultCount),
                    bleBestSeenMac.c_str(),
                    bleBestSeenRssi);
      cooperativeDelay(BLE_SCAN_RETRY_DELAY_MS);
    }
  }
  return bleTargetDeviceFound;
}

bool runExactBleWake() {
  bleNotifyCount = 0;
  bleWakeSawOk = false;
  bleLastNotifyText = "";
  bleNotifyChar3 = nullptr;
  bleDataChar4 = nullptr;
  bleLastConnectError = 0;
  bleConnectAttempts = 0;
  bleStage = "scan";
  resetBleScanStats();

  Serial.printf("[BLE] scanning for target %s\n", CAMERA_BLE_MAC);
  const unsigned long warmupMs = bleInitialized ? BLE_REUSE_WARMUP_MS : BLE_INIT_WARMUP_MS;
  Serial.printf("[BLE] warmup before BLE init %lu ms initialized=%s\n",
                warmupMs,
                bleInitialized ? "yes" : "no");
  cooperativeDelay(warmupMs);
  if (!bleInitialized) {
    NimBLEDevice::init(BOARD_HOSTNAME);
    bleInitialized = true;
  }
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&bridgeBleScanCallbacks, true);
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);
  scan->setMaxResults(0);
  bleTargetDeviceFound = false;
  runBleDiscoveryPass(scan, true, BLE_SCAN_ATTEMPTS, BLE_SCAN_WINDOW_SEC, "active");
  if (!bleTargetDeviceFound) {
    runBleDiscoveryPass(scan, false, BLE_PASSIVE_SCAN_ATTEMPTS, BLE_PASSIVE_SCAN_WINDOW_SEC, "passive");
  }

  if (!bleTargetDeviceFound) {
    Serial.printf("[BLE] target advertisement not found results=%u best_mac=%s best_name=%s best_rssi=%d attempts=%u\n",
                  static_cast<unsigned>(bleScanResultCount),
                  bleBestSeenMac.c_str(),
                  bleBestSeenName.c_str(),
                  bleBestSeenRssi,
                  static_cast<unsigned>(bleScanAttemptCounter));
    bleStage = "scan_not_found";
    return false;
  }

  if (!connectBleTargetWithRetries()) {
    Serial.printf("[BLE] connect failed after %u attempts last_error=%d\n",
                  static_cast<unsigned>(bleConnectAttempts),
                  bleLastConnectError);
    return false;
  }

  bleStage = "enumerate";

  NimBLERemoteService *service = bleClient->getService(CAMERA_BLE_GATT_SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("[BLE] GATT service unavailable");
    bleStage = "service_missing";
    closeBleWakeSession();
    return false;
  }
  bleNotifyChar3 = service->getCharacteristic(CAMERA_BLE_NOTIFY_UUID);
  bleDataChar4 = service->getCharacteristic(CAMERA_BLE_DATA_UUID);

  if (bleNotifyChar3 != nullptr && bleNotifyChar3->canNotify()) {
    Serial.println("[BLE] register notify on 6e400003");
    bleNotifyChar3->subscribe(true, bleNotifyCallback);
  }
  if (bleDataChar4 != nullptr && (bleDataChar4->canNotify() || bleDataChar4->canIndicate())) {
    Serial.println("[BLE] register notify on 6e400004");
    bleDataChar4->subscribe(bleDataChar4->canNotify(), bleNotifyCallback);
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
    bleDataChar4->writeValue(reinterpret_cast<const uint8_t *>(CAMERA_BLE_WAKE),
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
  Serial.println("  halow_status");
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
  Serial.println("  onboard_delete_all");
  Serial.println("  onboard_config key=value [key=value...]");
  Serial.println("  onboard_timelapse hours=<value> [interval_ms=300000]");
  Serial.println("  onboard_timelapse_stop");
  Serial.println("  onboard_dump [fresh]");
  Serial.println("  sd_status");
  Serial.println("  sd_mount");
  Serial.println("  wifi_scan");
  Serial.println("  upload_status");
  Serial.println("  upload_telemetry");
  Serial.println("  upload_events");
  Serial.println("  upload_all");
  Serial.println("  rtsp <METHOD> <url>");
  Serial.println("  wake");
  Serial.println("  bleclose");
}

void connectCameraWifi() {
  const unsigned long connectStartedMs = millis();
  Serial.printf("Connecting camera WiFi SSID %s\n", CAMERA_WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_MODE_STA);
  WiFi.setHostname(BOARD_HOSTNAME);
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
  lastWifiJoinElapsedMs = millis() - connectStartedMs;
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
    const unsigned long elapsedMs = millis() - start;
    const int count = WiFi.scanNetworks();
    for (int i = 0; i < count; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid == CAMERA_WIFI_SSID) {
        lastHotspotWaitElapsedMs = millis() - start;
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
    const unsigned long pollMs = elapsedMs < BRINGUP_HOTSPOT_FAST_WINDOW_MS
                                   ? BRINGUP_HOTSPOT_FAST_POLL_MS
                                   : intervalMs;
    const unsigned long remainingMs = timeoutMs - (millis() - start);
    cooperativeDelay(pollMs < remainingMs ? pollMs : remainingMs);
  }
  lastHotspotWaitElapsedMs = millis() - start;
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
  Serial.println("HaLow using network-assigned IP");
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
    lastClockSyncAttemptMs = millis();
    const bool clockOk = syncBoardClockFromAirScan();
    Serial.printf("[clock] sync after HaLow connect=%s valid=%s\n",
                  clockOk ? "ok" : "failed",
                  onboardClockValid() ? "yes" : "no");
  } else {
    Serial.println("HaLow connect timed out");
  }
}

String normalizeApiPath(const String &path) {
  String prefix = UPSTREAM_API_PREFIX;
  if (!prefix.isEmpty() && !prefix.startsWith("/")) {
    prefix = "/" + prefix;
  }
  if (prefix.endsWith("/")) {
    prefix.remove(prefix.length() - 1);
  }
  if (path.startsWith("/")) {
    return prefix + path;
  }
  return prefix + "/" + path;
}

bool postJsonToApi(const char *apiHost,
                   uint16_t apiPort,
                   const String &requestPath,
                   const String &requestBody,
                   String &responseBody,
                   int &statusCode) {
  if (!halowConnected || HaLow.status() != WL_CONNECTED) {
    connectHaLow();
  }
  if (!halowConnected) {
    statusCode = 0;
    responseBody = "{\"error\":\"halow_down\"}";
    return false;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    statusCode = 0;
    responseBody = "{\"error\":\"socket_create_failed\"}";
    return false;
  }
  timeval receiveTimeout{};
  receiveTimeout.tv_sec = 10;
  receiveTimeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));

  sockaddr_in upstreamAddr{};
  upstreamAddr.sin_family = AF_INET;
  upstreamAddr.sin_addr.s_addr = inet_addr(apiHost);
  upstreamAddr.sin_port = htons(apiPort);

  if (connect(sock, reinterpret_cast<sockaddr *>(&upstreamAddr), sizeof(upstreamAddr)) != 0) {
    close(sock);
    statusCode = 0;
    responseBody = "{\"error\":\"api_connect_failed\"}";
    return false;
  }

  String request = "POST " + requestPath + " HTTP/1.1\r\n";
  request += "Host: " + String(apiHost) + ":" + String(apiPort) + "\r\n";
  request += "User-Agent: esp32-gardepro-unified/0.1\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Accept: application/json\r\n";
  if (strlen(UPSTREAM_API_TOKEN) > 0) {
    request += "Authorization: Bearer " + String(UPSTREAM_API_TOKEN) + "\r\n";
  }
  request += "Content-Length: " + String(requestBody.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += requestBody;

  if (!sendAll(sock,
               reinterpret_cast<const uint8_t *>(request.c_str()),
               request.length())) {
    close(sock);
    statusCode = 0;
    responseBody = "{\"error\":\"api_send_failed\"}";
    return false;
  }

  String raw;
  uint8_t buf[512];
  const unsigned long start = millis();
  while (millis() - start < 10000) {
    const int n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
      raw.concat(reinterpret_cast<const char *>(buf), n);
      continue;
    }
    if (n == 0) {
      break;
    }
    cooperativeDelay(5);
  }
  close(sock);

  if (raw.isEmpty()) {
    statusCode = 0;
    responseBody = "{\"error\":\"api_empty_response\"}";
    return false;
  }

  const int headerEnd = raw.indexOf("\r\n\r\n");
  const String headers = headerEnd >= 0 ? raw.substring(0, headerEnd) : raw;
  responseBody = headerEnd >= 0 ? raw.substring(headerEnd + 4) : "";
  const int firstLineEnd = headers.indexOf("\r\n");
  const String statusLine = firstLineEnd >= 0 ? headers.substring(0, firstLineEnd) : headers;
  const int firstSpace = statusLine.indexOf(' ');
  const int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  if (firstSpace > 0 && secondSpace > firstSpace) {
    statusCode = statusLine.substring(firstSpace + 1, secondSpace).toInt();
  } else {
    statusCode = 0;
  }

  Serial.printf("[upload] POST %s -> %d bytes=%u\n",
                requestPath.c_str(),
                statusCode,
                static_cast<unsigned>(responseBody.length()));
  return statusCode >= 200 && statusCode < 300;
}

bool postJsonToUpstream(const String &path,
                        const String &requestBody,
                        String &responseBody,
                        int &statusCode) {
  return postJsonToApi(UPSTREAM_API_HOST,
                       UPSTREAM_API_PORT,
                       normalizeApiPath(path),
                       requestBody,
                       responseBody,
                       statusCode);
}

String observationQueuePath(uint32_t id, const String &kind) {
  char name[40];
  snprintf(name, sizeof(name), "/%08lu_%s.json", static_cast<unsigned long>(id), kind.c_str());
  return String(OBSERVATION_QUEUE_DIR) + name;
}

bool observationQueueStats(size_t &count, size_t &bytes, std::vector<String> *paths = nullptr) {
  count = 0;
  bytes = 0;
  if (paths != nullptr) paths->clear();
  if (!onboardStorageReady) return false;
  fs::FS &fs = observationQueueFs();
  File dir = fs.open(OBSERVATION_QUEUE_DIR, FILE_READ);
  if (!dir || !dir.isDirectory()) return false;
  File file = dir.openNextFile(FILE_READ);
  while (file) {
    if (!file.isDirectory()) {
      const String name = file.name();
      if (name.endsWith(".json")) {
        ++count;
        bytes += file.size();
        if (paths != nullptr) {
          paths->push_back(name.startsWith("/") ? name : String(OBSERVATION_QUEUE_DIR) + "/" + name);
        }
      }
    }
    file.close();
    file = dir.openNextFile(FILE_READ);
  }
  dir.close();
  if (paths != nullptr) std::sort(paths->begin(), paths->end());
  return true;
}

void refreshObservationQueueCachedStats() {
  size_t count = 0;
  size_t bytes = 0;
  if (observationQueueStats(count, bytes)) {
    observationQueueCachedCount = count;
    observationQueueCachedBytes = bytes;
  }
}

bool enqueueObservationPayload(const String &kind, const String &payload) {
  if (!onboardStorageReady) {
    observationQueueLastError = "storage_unavailable";
    ++observationQueueDropCount;
    return false;
  }
  if (payload.isEmpty()) {
    observationQueueLastError = "empty_payload";
    ++observationQueueDropCount;
    return false;
  }
  size_t queuedCount = 0;
  size_t queuedBytes = 0;
  observationQueueStats(queuedCount, queuedBytes);
  const size_t maxFiles = sdReady ? 4096 : OBSERVATION_QUEUE_MAX_FILES;
  if (queuedCount >= maxFiles) {
    observationQueueLastError = "queue_full";
    ++observationQueueDropCount;
    return false;
  }
  const uint64_t freeBytes = persistentStorageFreeBytes();
  const size_t minFreeBytes = sdReady ? 1024 * 1024 : OBSERVATION_QUEUE_MIN_FREE_BYTES;
  if (freeBytes < payload.length() + minFreeBytes) {
    observationQueueLastError = "storage_full";
    ++observationQueueDropCount;
    return false;
  }

  fs::FS &fs = observationQueueFs();
  const uint32_t id = observationQueueNextId++;
  Preferences queuePrefs;
  if (queuePrefs.begin("obsq", false)) {
    queuePrefs.putULong("next_id", observationQueueNextId);
    queuePrefs.end();
  }

  const String finalPath = observationQueuePath(id, kind);
  const String tempPath = finalPath + ".tmp";
  fs.remove(tempPath);
  File file = fs.open(tempPath, FILE_WRITE);
  if (!file) {
    observationQueueLastError = "queue_write_failed";
    ++observationQueueDropCount;
    return false;
  }
  const size_t written = file.print(payload);
  file.close();
  if (written != payload.length() || !fs.rename(tempPath, finalPath)) {
    fs.remove(tempPath);
    fs.remove(finalPath);
    observationQueueLastError = "queue_commit_failed";
    ++observationQueueDropCount;
    return false;
  }
  ++observationQueueEnqueueCount;
  observationQueueCachedCount = queuedCount + 1;
  observationQueueCachedBytes = queuedBytes + payload.length();
  observationQueueLastError = "";
  Serial.printf("[observation-queue] saved kind=%s storage=%s path=%s bytes=%u\n",
                kind.c_str(),
                persistentStorageType(),
                finalPath.c_str(),
                static_cast<unsigned>(payload.length()));
  return true;
}

bool replayQueuedObservationBatches(uint8_t maxBatches, String &summary) {
  summary = "{\"attempted\":0,\"uploaded\":0,\"failed\":0}";
  if (!onboardStorageReady) {
    observationQueueLastError = "storage_unavailable";
    return true;
  }
  size_t queuedCount = 0;
  size_t queuedBytes = 0;
  std::vector<String> paths;
  if (!observationQueueStats(queuedCount, queuedBytes, &paths) || paths.empty()) {
    observationQueueLastError = "";
    return true;
  }

  uint8_t attempted = 0;
  uint8_t uploaded = 0;
  uint8_t failed = 0;
  fs::FS &fs = observationQueueFs();
  for (const String &path : paths) {
    if (attempted >= maxBatches) break;
    File file = fs.open(path, FILE_READ);
    if (!file) {
      fs.remove(path);
      continue;
    }
    const size_t queuedFileBytes = file.size();
    String payload = file.readString();
    file.close();
    if (payload.isEmpty()) {
      fs.remove(path);
      continue;
    }
    ++attempted;
    ++uploadAttemptCount;
    String responseBody;
    int statusCode = 0;
    const bool ok = postJsonToApi(AIR_SCAN_API_HOST,
                                  AIR_SCAN_API_PORT,
                                  "/api/observations/upload",
                                  payload,
                                  responseBody,
                                  statusCode);
    setUploadResult(ok, statusCode, ok ? "queued_observations_uploaded" : responseBody);
    if (ok) {
      fs.remove(path);
      if (observationQueueCachedCount > 0) --observationQueueCachedCount;
      if (observationQueueCachedBytes >= queuedFileBytes) observationQueueCachedBytes -= queuedFileBytes;
      ++uploaded;
      ++observationQueueReplaySuccessCount;
      Serial.printf("[observation-queue] uploaded path=%s status=%d\n", path.c_str(), statusCode);
    } else {
      ++failed;
      ++observationQueueReplayFailureCount;
      observationQueueLastError = "replay_failed";
      Serial.printf("[observation-queue] replay failed path=%s status=%d\n", path.c_str(), statusCode);
      break;
    }
  }

  summary = "{\"attempted\":" + String(attempted) +
            ",\"uploaded\":" + String(uploaded) +
            ",\"failed\":" + String(failed) + "}";
  if (failed == 0) observationQueueLastError = "";
  return failed == 0;
}

bool syncBoardClockFromAirScan() {
  if (onboardClockValid()) return true;
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return false;
  timeval timeout{};
  timeout.tv_sec = 5;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(AIR_SCAN_API_HOST);
  addr.sin_port = htons(AIR_SCAN_API_PORT);
  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return false;
  }
  const String request = "HEAD / HTTP/1.1\r\nHost: " + String(AIR_SCAN_API_HOST) +
                         ":" + String(AIR_SCAN_API_PORT) + "\r\nConnection: close\r\n\r\n";
  if (!sendAll(sock, reinterpret_cast<const uint8_t *>(request.c_str()), request.length())) {
    close(sock);
    return false;
  }
  char buffer[1025];
  const int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  close(sock);
  if (received <= 0) return false;
  buffer[received] = '\0';
  String headers(buffer);
  String lower = headers;
  lower.toLowerCase();
  const int dateStart = lower.indexOf("\r\ndate:");
  if (dateStart < 0) return false;
  const int valueStart = dateStart + 7;
  const int valueEnd = headers.indexOf("\r\n", valueStart);
  if (valueEnd < 0) return false;
  String dateValue = headers.substring(valueStart, valueEnd);
  dateValue.trim();
  char weekday[4] = {};
  char monthName[4] = {};
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(dateValue.c_str(), "%3[^,], %d %3s %d %d:%d:%d GMT",
             weekday, &day, monthName, &year, &hour, &minute, &second) != 7) return false;
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = -1;
  for (int i = 0; i < 12; ++i) {
    if (strcmp(monthName, months[i]) == 0) {
      month = i;
      break;
    }
  }
  if (month < 0) return false;
  struct tm tmValue{};
  tmValue.tm_year = year - 1900;
  tmValue.tm_mon = month;
  tmValue.tm_mday = day;
  tmValue.tm_hour = hour;
  tmValue.tm_min = minute;
  tmValue.tm_sec = second;
  setenv("TZ", "UTC0", 1);
  tzset();
  const time_t epoch = mktime(&tmValue);
  if (epoch < 1700000000) return false;
  timeval tv{};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
  Serial.printf("[clock] synchronized from air_scan HTTP date epoch=%ld\n", static_cast<long>(epoch));
  setenv("TZ", BOARD_TIMEZONE, 1);
  tzset();
  backfillOnboardMediaTimestamps();
  return true;
}

bool uploadTelemetryNow(String &responseBody, int &statusCode) {
  ++uploadAttemptCount;
  const String payload = buildBoardTelemetryJson();
  const bool ok = postJsonToUpstream("/api/board/telemetry", payload, responseBody, statusCode);
  setUploadResult(ok, statusCode, ok ? "telemetry_uploaded" : responseBody);
  enqueueUploadEvent("telemetry_upload", ok ? "manual_or_api" : "upload_failed", "{\"status_code\":" + String(statusCode) + "}", ok);
  return ok;
}

bool uploadWifiObservationsNow(String &responseBody, int &statusCode) {
  syncBoardClockFromAirScan();
  String scanPayload;
  String scanError;
  int scanCode = 0;
  wifiScannerLastRunMs = millis();
  ++wifiScannerRunCount;
  if (!runIdleWifiScan(scanPayload, scanError, scanCode)) {
    statusCode = 0;
    wifiScannerLastError = scanError;
    responseBody = "{\"error\":\"" + jsonEscape(scanError) + "\"";
    if (scanError == "scan_failed") responseBody += ",\"scan_code\":" + String(scanCode);
    responseBody += "}";
    ++wifiScannerUploadFailureCount;
    Serial.printf("[wifi-scanner] scan skipped/failed error=%s code=%d\n", scanError.c_str(), scanCode);
    return false;
  }

  ++uploadAttemptCount;
  const bool ok = postJsonToApi(AIR_SCAN_API_HOST,
                                AIR_SCAN_API_PORT,
                                "/api/observations/upload",
                                wifiObservationsLastJson,
                                responseBody,
                                statusCode);
  setUploadResult(ok, statusCode, ok ? "wifi_observations_uploaded" : responseBody);
  if (ok) {
    ++wifiScannerUploadSuccessCount;
    wifiScannerLastUploadMs = millis();
    wifiScannerLastError = "";
  } else {
    ++wifiScannerUploadFailureCount;
    wifiScannerLastError = "upload_failed";
    enqueueObservationPayload("wifi", wifiObservationsLastJson);
  }
  Serial.printf("[wifi-scanner] networks=%u upload=%s status=%d\n",
                wifiScanLastCount,
                ok ? "ok" : "failed",
                statusCode);
  return ok;
}

bool uploadBleObservationsNow(String &responseBody, int &statusCode) {
  String scanError;
  if (!runBleObservationScan(scanError)) {
    statusCode = 0;
    responseBody = "{\"error\":\"" + jsonEscape(scanError) + "\"}";
    bleScannerLastError = scanError;
    ++bleScannerUploadFailureCount;
    Serial.printf("[ble-scanner] scan skipped/failed error=%s\n", scanError.c_str());
    return false;
  }
  ++uploadAttemptCount;
  const bool ok = postJsonToApi(AIR_SCAN_API_HOST,
                                AIR_SCAN_API_PORT,
                                "/api/observations/upload",
                                bleObservationsLastJson,
                                responseBody,
                                statusCode);
  setUploadResult(ok, statusCode, ok ? "ble_observations_uploaded" : responseBody);
  if (ok) {
    ++bleScannerUploadSuccessCount;
    bleScannerLastUploadMs = millis();
    bleScannerLastError = "";
  } else {
    ++bleScannerUploadFailureCount;
    bleScannerLastError = "upload_failed";
    enqueueObservationPayload("ble", bleObservationsLastJson);
  }
  Serial.printf("[ble-scanner] devices=%u mfr=%u svc=%u svc_data=%u name=%u tx=%u upload=%s status=%d\n",
                bleScannerLastCount,
                bleScannerLastManufacturerCount,
                bleScannerLastServicesCount,
                bleScannerLastServiceDataCount,
                bleScannerLastNameCount,
                bleScannerLastTxPowerCount,
                ok ? "ok" : "failed",
                statusCode);
  return ok;
}

unsigned long currentWifiScannerIntervalMs() {
  if (!onboardClockValid()) return WIFI_SCAN_DAY_INTERVAL_MS;
  const int localMinute = onboardLocalMinuteOfDay();
  const bool daytime = localMinute >= WIFI_SCAN_DAY_START_MINUTE && localMinute < WIFI_SCAN_DAY_END_MINUTE;
  return daytime ? WIFI_SCAN_DAY_INTERVAL_MS : WIFI_SCAN_NIGHT_INTERVAL_MS;
}

void wifiScannerTask(void *pvParameters) {
  (void)pvParameters;
  vTaskDelay(pdMS_TO_TICKS(WIFI_SCAN_INITIAL_DELAY_MS));
  while (true) {
    String wifiResponse;
    int wifiStatus = 0;
    if (observationUploadMutex != nullptr) {
      xSemaphoreTake(observationUploadMutex, portMAX_DELAY);
    }
    String queueSummary;
    const bool queueOk = replayQueuedObservationBatches(4, queueSummary);
    const bool wifiOk = uploadWifiObservationsNow(wifiResponse, wifiStatus);
    String bleResponse;
    int bleStatus = 0;
    const bool bleOk = uploadBleObservationsNow(bleResponse, bleStatus);
    if (observationUploadMutex != nullptr) {
      xSemaphoreGive(observationUploadMutex);
    }
    const unsigned long waitMs = (queueOk && wifiOk && bleOk) ? currentWifiScannerIntervalMs() : 30000UL;
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
}

void startWifiScannerTask() {
  if (wifiScannerTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(wifiScannerTask,
                            "rf-scanner",
                            12288,
                            nullptr,
                            1,
                            &wifiScannerTaskHandle,
                            1);
  }
}

bool uploadQueuedEventsNow(String &responseBody, int &statusCode) {
  if (uploadEventCount == 0) {
    statusCode = 200;
    responseBody = "{\"ok\":true,\"inserted\":0}";
    setUploadResult(true, statusCode, "no_events_to_upload");
    return true;
  }

  ++uploadAttemptCount;
  const String payload = buildEventsUploadJson();
  const bool ok = postJsonToUpstream("/api/board/events", payload, responseBody, statusCode);
  setUploadResult(ok, statusCode, ok ? "events_uploaded" : responseBody);
  if (ok) {
    clearUploadedEvents();
  }
  return ok;
}

String buildUploadResultJson(bool ok, int statusCode, const String &responseBody) {
  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  payload += ",\"status_code\":" + String(statusCode);
  payload += ",\"response\":" + jsonNullableString(responseBody);
  payload += ",\"upload\":" + buildUploadStatusJson();
  payload += "}";
  return payload;
}

bool proxyCameraRequest(const String &method,
                       const String &path,
                       const String &requestBody,
                       const String &contentType,
                       String &responseBody,
                       int &statusCode) {
  refreshCameraSessionLease();
  if (path == "/cmd/standby/now" && cameraSessionLeaseActive) {
    statusCode = 200;
    responseBody = "{\"code\":0,\"desc\":\"standby_deferred_by_lease\"}";
    Serial.println("[session] deferred standby because lease is active");
    return true;
  }

  const unsigned long httpStartedMs = millis();
  WiFiClient client;
  if (!client.connect(CAMERA_IP, CAMERA_HTTP_PORT)) {
    lastCameraHttpElapsedMs = millis() - httpStartedMs;
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
      lastCameraHttpElapsedMs = millis() - httpStartedMs;
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
  lastCameraHttpElapsedMs = millis() - httpStartedMs;
  if (statusCode >= 200 && statusCode < 300) {
    if (path == "/cmd/standby/now") {
      standbyRequested = true;
      releaseCameraSessionLease();
    } else if (path == "/cmd/standby/reset") {
      standbyRequested = false;
      markCameraActivity();
    } else {
      markCameraActivity();
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

void processCameraSessionLeaseExpiry() {
  if (!cameraSessionLeaseActive || !cameraSessionLeaseExpired()) {
    return;
  }

  const bool sendStandby = cameraSessionLeaseStandbyOnExpire &&
                           wifiConnected &&
                           !streamSessionActive &&
                           !cameraSessionLeaseExpiredStandbySent;
  cameraSessionLeaseActive = false;
  cameraSessionLeaseExpiresMs = 0;
  cameraSessionLeaseDurationMs = 0;

  if (sendStandby) {
    String body;
    int statusCode = 500;
    cameraSessionLeaseExpiredStandbySent = true;
    Serial.println("[session] lease expired, requesting standby");
    proxyCameraRequest("GET", "/cmd/standby/now", body, statusCode);
  }
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
  lastStreamStartStage = "rtsp_init";
  lastStreamStartMessage = "";
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
    lastStreamStartStage = "camera_wifi_down";
    lastStreamStartMessage = "camera_wifi_down";
    return false;
  }

  closeRtspSession();
  if (!cameraRtspClient.connect(CAMERA_IP, CAMERA_RTSP_PORT)) {
    Serial.println("[rtsp-live] failed to connect RTSP socket");
    lastStreamStartStage = "rtsp_socket_connect_failed";
    lastStreamStartMessage = "rtsp_socket_connect_failed";
    return false;
  }

  lastStreamStartStage = "rtsp_describe";
  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "DESCRIBE",
                                 info.describeUrl,
                                 "",
                                 info.describeResponse,
                                 info.describeStatus)) {
    Serial.println("[rtsp-live] DESCRIBE failed");
    lastStreamDescribeStatus = info.describeStatus;
    lastStreamStartStage = "rtsp_describe_failed";
    lastStreamStartMessage = "rtsp_describe_failed";
    closeRtspSession();
    return false;
  }
  lastStreamDescribeStatus = info.describeStatus;
  printBodySnippet(info.describeResponse);
  printFullTextBlock("RTSP DESCRIBE", info.describeResponse);
  if (info.describeStatus != 200) {
    Serial.printf("[rtsp-live] DESCRIBE status=%d\n", info.describeStatus);
    lastStreamStartStage = "rtsp_describe_status";
    lastStreamStartMessage = "rtsp_describe_status_" + String(info.describeStatus);
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
  lastStreamStartStage = "rtsp_setup";
  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "SETUP",
                                 info.mediaControlUrl,
                                 setupHeaders,
                                 info.setupResponse,
                                 info.setupStatus)) {
    Serial.println("[rtsp-live] SETUP failed");
    lastStreamSetupStatus = info.setupStatus;
    lastStreamStartStage = "rtsp_setup_failed";
    lastStreamStartMessage = "rtsp_setup_failed";
    closeRtspSession();
    return false;
  }
  lastStreamSetupStatus = info.setupStatus;
  printBodySnippet(info.setupResponse);
  printFullTextBlock("RTSP SETUP", info.setupResponse);
  if (info.setupStatus != 200) {
    Serial.printf("[rtsp-live] SETUP status=%d\n", info.setupStatus);
    lastStreamStartStage = "rtsp_setup_status";
    lastStreamStartMessage = "rtsp_setup_status_" + String(info.setupStatus);
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
    lastStreamStartStage = "rtsp_setup_missing_session";
    lastStreamStartMessage = "rtsp_setup_missing_session";
    return false;
  }
  lastRtspSessionId = info.sessionHeader;
  Serial.printf("[rtsp-live] session=%s\n", info.sessionHeader.c_str());

  String playHeaders = "Session: " + info.sessionHeader + "\r\n";
  playHeaders += "Range: npt=0.000-\r\n";
  String playUrl = info.aggregateControlUrl;
  lastStreamStartStage = "rtsp_play";
  lastStreamPlayUrl = playUrl;
  if (!exchangeCameraRtspRequest(cameraRtspClient,
                                 "PLAY",
                                 playUrl,
                                 playHeaders,
                                 info.playResponse,
                                 info.playStatus)) {
    Serial.println("[rtsp-live] PLAY failed");
    lastStreamPlayStatus = info.playStatus;
    lastStreamStartStage = "rtsp_play_failed";
    lastStreamStartMessage = "rtsp_play_failed";
    closeRtspSession();
    return false;
  }
  lastStreamPlayStatus = info.playStatus;
  printBodySnippet(info.playResponse);
  printFullTextBlock("RTSP PLAY", info.playResponse);
  if (info.playStatus == 455 && info.mediaControlUrl != info.aggregateControlUrl) {
    Serial.printf("[rtsp-live] retry PLAY on media URL %s\n", info.mediaControlUrl.c_str());
    playUrl = info.mediaControlUrl;
    lastStreamPlayUrl = playUrl;
    info.playResponse = "";
    if (!exchangeCameraRtspRequest(cameraRtspClient,
                                   "PLAY",
                                   playUrl,
                                   playHeaders,
                                   info.playResponse,
                                   info.playStatus)) {
      Serial.println("[rtsp-live] PLAY retry failed");
      lastStreamPlayStatus = info.playStatus;
      lastStreamStartStage = "rtsp_play_retry_failed";
      lastStreamStartMessage = "rtsp_play_retry_failed";
      closeRtspSession();
      return false;
    }
    lastStreamPlayStatus = info.playStatus;
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
    lastStreamStartStage = "rtsp_play_ok";
    lastStreamStartMessage = "rtsp_play_ok";
  } else {
    lastStreamStartStage = "rtsp_play_status";
    lastStreamStartMessage = "rtsp_play_status_" + String(info.playStatus);
    closeRtspSession();
  }
  Serial.printf("[rtsp-live] PLAY status=%d url=%s\n", info.playStatus, playUrl.c_str());
  return info.playStatus == 200;
}

bool startStreamSession() {
  const unsigned long startedMs = millis();
  lastStreamStartMs = startedMs;
  lastStreamStartElapsedMs = 0;
  lastStreamStartStage = "start";
  lastStreamStartMessage = "";
  lastStreamDescribeStatus = 0;
  lastStreamSetupStatus = 0;
  lastStreamPlayStatus = 0;
  lastTunnelConnectError = 0;
  lastStreamPlayUrl = "";
  if (streamSessionActive) {
    Serial.println("[stream] session already active");
    lastStreamStartStage = "already_active";
    lastStreamStartMessage = "stream_active";
    lastStreamStartElapsedMs = millis() - startedMs;
    return true;
  }
  if (!halowConnected) {
    lastStreamStartStage = "halow_connect";
    Serial.println("[stream] HaLow is down, connecting now");
    connectHaLow();
  }
  if (!halowConnected) {
    Serial.println("[stream] HaLow connect failed");
    lastStreamStartStage = "halow_down";
    lastStreamStartMessage = "stream_halow_down";
    lastStreamStartElapsedMs = millis() - startedMs;
    return false;
  }
  if (!wifiConnected) {
    Serial.println("[stream] camera WiFi is down");
    lastStreamStartStage = "camera_wifi_down";
    lastStreamStartMessage = "stream_camera_wifi_down";
    lastStreamStartElapsedMs = millis() - startedMs;
    return false;
  }

  RtspSessionInfo info{};
  if (!runRtspLiveSequence(info)) {
    Serial.println("[stream] RTSP live sequence failed");
    if (lastStreamStartMessage.isEmpty()) {
      lastStreamStartMessage = "stream_rtsp_failed";
    }
    lastStreamStartElapsedMs = millis() - startedMs;
    return false;
  }

  lastStreamStartStage = "tunnel_connect";
  if (!connectTunnelSocket()) {
    Serial.println("[stream] tunnel connect failed");
    lastStreamStartStage = "tunnel_connect_failed";
    lastStreamStartMessage = "stream_tunnel_connect_failed";
    lastStreamStartElapsedMs = millis() - startedMs;
    closeRtspSession();
    return false;
  }

  startHttpServer();
  streamSessionActive = true;
  streamSessionStartedMs = millis();
  lastStreamStartStage = "stream_active";
  lastStreamStartMessage = "stream_active";
  lastStreamStartElapsedMs = millis() - startedMs;
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
  const unsigned long bringupStartedMs = millis();
  refreshWifiState();
  if (wifiConnected) {
    lastBringupElapsedMs = millis() - bringupStartedMs;
    Serial.println("[bringup] camera WiFi already connected");
    return true;
  }

  WiFi.disconnect(true, true);
  cooperativeDelay(250);
  refreshWifiState();
  bleWakeAttempted = true;
  bool hotspotVisible = false;

  if (tryExistingBleWakeSession()) {
    const unsigned long hotspotStartedMs = millis();
    hotspotVisible = waitForCameraWifiPresence(BRINGUP_HOTSPOT_WAIT_MS,
                                               BRINGUP_HOTSPOT_POLL_MS);
    lastHotspotWaitElapsedMs = millis() - hotspotStartedMs;
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
    const unsigned long bleStartedMs = millis();
    bleWakeConfirmed = runExactBleWake();
    lastBleWakeElapsedMs = millis() - bleStartedMs;
    Serial.printf("[BLE] exact wake result: %s stage=%s\n",
                  bleWakeConfirmed ? "success" : "no-confirmation",
                  bleStage.c_str());
    if (!bleWakeConfirmed) {
      Serial.println("[bringup] aborting after BLE wake failure");
      refreshWifiState();
      lastBringupElapsedMs = millis() - bringupStartedMs;
      return false;
    }
    const unsigned long hotspotStartedMs = millis();
    hotspotVisible = waitForCameraWifiPresence(BRINGUP_HOTSPOT_WAIT_MS,
                                               BRINGUP_HOTSPOT_POLL_MS);
    lastHotspotWaitElapsedMs = millis() - hotspotStartedMs;
    Serial.printf("[WiFi] hotspot visibility after BLE wake: %s\n", hotspotVisible ? "yes" : "no");
  }
  if (!hotspotVisible) {
    Serial.println("[bringup] aborting because camera hotspot did not appear");
    refreshWifiState();
    lastBringupElapsedMs = millis() - bringupStartedMs;
    return false;
  }
  connectCameraWifi();
  if (RUN_LOCAL_SERIAL_TEST) {
    Serial.println("[BLE] keeping wake session open in local serial mode");
  } else {
    closeBleWakeSession();
  }
  refreshWifiState();
  lastBringupElapsedMs = millis() - bringupStartedMs;
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
  String basic = "{";
  basic += "\"uptime_ms\":" + String(millis());
  basic += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  basic += ",\"boot_count\":" + String(persistentBootCount);
  basic += ",\"boot_session_id\":" + String(bootSessionId);
  basic += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  basic += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  basic += ",\"halow_rssi\":" + String(halowConnected ? HaLow.RSSI() : 0);
  basic += ",\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  basic += ",\"clock_valid\":" + String(onboardClockValid() ? "true" : "false");
  basic += ",\"storage_type\":\"" + String(persistentStorageType()) + "\"";
  basic += ",\"storage_ready\":" + String(onboardStorageReady ? "true" : "false");
  basic += ",\"sd_ready\":" + String(sdReady ? "true" : "false");
  basic += ",\"stored_photo_count\":" + String(onboardStoredPhotoCount);
  basic += ",\"latest_media_id\":" + (onboardLatestMediaId == 0 ? String("null") : "\"" + onboardMediaIdString(onboardLatestMediaId) + "\"");
  basic += ",\"onboard_captures\":" + String(onboardCaptureCount);
  basic += ",\"schedule_mode\":\"" + String(onboardClockValid() ? "clock_window" : "uptime_fallback") + "\"";
  basic += ",\"observation_queue\":{";
  basic += "\"storage_type\":\"" + String(persistentStorageType()) + "\"";
  basic += ",\"queued_batches\":" + String(static_cast<unsigned>(observationQueueCachedCount));
  basic += ",\"queued_bytes\":" + String(static_cast<unsigned>(observationQueueCachedBytes));
  basic += "}";
  basic += ",\"wifi_scanner\":{";
  basic += "\"runs\":" + String(wifiScannerRunCount);
  basic += ",\"last_count\":" + String(wifiScanLastCount);
  basic += ",\"upload_successes\":" + String(wifiScannerUploadSuccessCount);
  basic += ",\"upload_failures\":" + String(wifiScannerUploadFailureCount);
  basic += ",\"last_error\":\"" + jsonEscape(wifiScannerLastError) + "\"";
  basic += "}";
  basic += ",\"ble_scanner\":{";
  basic += "\"runs\":" + String(bleScannerRunCount);
  basic += ",\"last_count\":" + String(bleScannerLastCount);
  basic += ",\"manufacturer_count\":" + String(bleScannerLastManufacturerCount);
  basic += ",\"services_count\":" + String(bleScannerLastServicesCount);
  basic += ",\"name_count\":" + String(bleScannerLastNameCount);
  basic += ",\"tx_power_count\":" + String(bleScannerLastTxPowerCount);
  basic += ",\"upload_successes\":" + String(bleScannerUploadSuccessCount);
  basic += ",\"upload_failures\":" + String(bleScannerUploadFailureCount);
  basic += ",\"last_error\":\"" + jsonEscape(bleScannerLastError) + "\"";
  basic += "}";
  basic += "}";
  server.send(200, "application/json", basic);
  return;

  ControlState controlSnapshot{};
  snapshotControlState(controlSnapshot);
  String payload = "{";
  payload += "\"uptime_ms\":" + String(millis());
  payload += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  payload += ",\"boot_count\":" + String(persistentBootCount);
  payload += ",\"boot_session_id\":" + String(bootSessionId);
  payload += ",\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  payload += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
  payload += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"halow_mac\":\"" + HaLow.macAddress() + "\"";
  payload += ",\"halow\":" + buildHaLowStatusJson();
  payload += ",\"psram_found\":" + String(psramFound() ? "true" : "false");
  payload += ",\"psram_size\":" + String(ESP.getPsramSize());
  payload += ",\"psram_free\":" + String(ESP.getFreePsram());
  payload += ",\"chip_temperature_c\":" + String(readChipTemperatureC(), 1);
  payload += ",\"camera_ip\":\"" + CAMERA_IP.toString() + "\"";
  payload += ",\"camera_wifi_ever_connected\":" + String(cameraWifiEverConnected ? "true" : "false");
  payload += ",\"standby_requested\":" + String(standbyRequested ? "true" : "false");
  payload += ",\"camera_session\":" + buildCameraSessionJson();
  payload += ",\"timing\":" + buildTimingJson();
  payload += ",\"stream_status\":" + buildStreamStatusJson();
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
  payload += ",\"upload\":" + buildUploadStatusJson();
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
  payload += ",\"ble_connect_attempts\":" + String(bleConnectAttempts);
  payload += ",\"ble_last_connect_error\":" + String(bleLastConnectError);
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
  if (!onboardStorageReady) {
    server.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
    return;
  }

  if (server.hasArg("capture") && server.arg("capture") == "1") {
    String error;
    if (!captureOnboardFrame("manual", nullptr, &error)) {
      server.send(error == "storage_full" ? 507 : 500, "application/json", "{\"error\":\"" + jsonEscape(error) + "\"}");
      return;
    }
  }

  if (onboardLatestMediaId == 0) {
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
    return;
  }
  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  File file = onboardMediaFs().open(onboardMediaImagePath(onboardLatestMediaId), FILE_READ);
  if (!file) {
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(file, "image/jpeg");
  file.close();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
}

void handleOnboardCameraCapture() {
  if (!onboardCameraReady) {
    server.send(503, "application/json", "{\"error\":\"onboard_camera_not_ready\"}");
    return;
  }

  const bool hasOneShotSettings = serverHasOnboardSensorArgs();
  const OnboardCameraSettings scheduledSettings = snapshotOnboardCameraSettings();
  if (hasOneShotSettings) {
    String sensorError;
    if (!applyOnboardCameraSensorArgs(sensorError, false)) {
      server.send(400, "application/json", "{\"error\":\"" + jsonEscape(sensorError) + "\"}");
      return;
    }
    cooperativeDelay(150);
  }

  OnboardMediaInfo savedMedia{};
  String captureError;
  const bool ok = captureOnboardFrame("manual", &savedMedia, &captureError);
  bool settingsRestored = !hasOneShotSettings;
  if (hasOneShotSettings) {
    String restoreError;
    settingsRestored = applyOnboardCameraSettings(scheduledSettings, restoreError);
    if (!settingsRestored) {
      Serial.printf("[onboard-camera] failed to restore scheduled settings: %s\n", restoreError.c_str());
    }
  }

  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  if (!ok) payload += ",\"error\":\"" + jsonEscape(captureError.isEmpty() ? String("capture_failed") : captureError) + "\"";
  if (ok) payload += ",\"media\":" + buildOnboardMediaJson(savedMedia, false);
  payload += ",\"latest_updated\":" + String(ok ? "true" : "false");
  payload += ",\"one_shot_settings\":" + String(hasOneShotSettings ? "true" : "false");
  payload += ",\"settings_restored\":" + String(settingsRestored ? "true" : "false");
  payload += ",\"onboard_camera\":" + buildOnboardCameraStatusJson();
  payload += "}";
  const int status = ok ? 200 : (captureError == "storage_full" ? 507 : (captureError == "storage_unavailable" ? 503 : 500));
  server.send(status, "application/json", payload);
}

bool parseOnboardMediaId(const String &value, uint32_t &id) {
  if (value.isEmpty() || value.length() > 10) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(value[i])) return false;
  }
  id = strtoul(value.c_str(), nullptr, 10);
  return id > 0;
}

bool deleteAllOnboardMedia(unsigned &deleted, unsigned &failed) {
  deleted = 0;
  failed = 0;
  if (!onboardStorageReady) return false;
  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  fs::FS &fs = onboardMediaFs();
  std::vector<uint32_t> ids;
  File dir = fs.open(ONBOARD_MEDIA_DIR, FILE_READ);
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile(FILE_READ);
    while (file) {
      const String name = file.name();
      if (!file.isDirectory() && name.endsWith(".jpg")) {
        const int slash = name.lastIndexOf('/');
        uint32_t id = 0;
        if (parseOnboardMediaId(name.substring(slash + 1, name.length() - 4), id)) ids.push_back(id);
      }
      file.close();
      file = dir.openNextFile(FILE_READ);
    }
    dir.close();
  }
  for (uint32_t id : ids) {
    const bool imageDeleted = fs.remove(onboardMediaImagePath(id));
    const bool metaDeleted = !fs.exists(onboardMediaMetaPath(id)) || fs.remove(onboardMediaMetaPath(id));
    if (imageDeleted && metaDeleted) ++deleted; else ++failed;
  }
  refreshOnboardMediaState();
  if (onboardFrameMutex != nullptr) {
    xSemaphoreTake(onboardFrameMutex, portMAX_DELAY);
    if (onboardLatestJpeg != nullptr) {
      free(onboardLatestJpeg);
      onboardLatestJpeg = nullptr;
    }
    onboardLatestJpegLen = 0;
    onboardLastCaptureMs = 0;
    xSemaphoreGive(onboardFrameMutex);
  }
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  return true;
}

void handleOnboardMediaList() {
  if (!onboardStorageReady) {
    server.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
    return;
  }
  unsigned offset = server.hasArg("offset") ? server.arg("offset").toInt() : 0;
  unsigned limit = server.hasArg("limit") ? server.arg("limit").toInt() : 50;
  if (limit == 0 || limit > 100) {
    server.send(400, "application/json", "{\"error\":\"invalid_request\",\"detail\":\"limit must be 1..100\"}");
    return;
  }
  const bool oldestFirst = server.hasArg("sort") && server.arg("sort") == "oldest";
  uint32_t fromEpoch = server.hasArg("from") ? strtoul(server.arg("from").c_str(), nullptr, 10) : 0;
  uint32_t toEpoch = server.hasArg("to") ? strtoul(server.arg("to").c_str(), nullptr, 10) : UINT32_MAX;
  std::vector<OnboardMediaInfo> media;
  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  fs::FS &fs = onboardMediaFs();
  File dir = fs.open(ONBOARD_MEDIA_DIR, FILE_READ);
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile(FILE_READ);
    while (file) {
      const String name = file.name();
      if (!file.isDirectory() && name.endsWith(".jpg")) {
        const int slash = name.lastIndexOf('/');
        uint32_t id = 0;
        if (parseOnboardMediaId(name.substring(slash + 1, name.length() - 4), id)) {
          OnboardMediaInfo info{};
          if (readOnboardMediaInfo(id, info) &&
              (info.recordedAt == 0 || (info.recordedAt >= fromEpoch && info.recordedAt <= toEpoch))) {
            media.push_back(info);
          }
        }
      }
      file.close();
      file = dir.openNextFile(FILE_READ);
    }
    dir.close();
  }
  std::sort(media.begin(), media.end(), [oldestFirst](const OnboardMediaInfo &a, const OnboardMediaInfo &b) {
    return oldestFirst ? a.id < b.id : a.id > b.id;
  });
  String payload = "{";
  payload += "\"count\":" + String(media.size());
  payload += ",\"offset\":" + String(offset);
  payload += ",\"limit\":" + String(limit);
  payload += ",\"sort\":\"" + String(oldestFirst ? "oldest" : "newest") + "\"";
  payload += ",\"media\":[";
  unsigned emitted = 0;
  for (size_t i = offset; i < media.size() && emitted < limit; ++i, ++emitted) {
    if (emitted > 0) payload += ",";
    payload += buildOnboardMediaJson(media[i], true);
  }
  payload += "]}";
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  server.send(200, "application/json", payload);
}

void handleOnboardMediaFile() {
  if (!onboardStorageReady) {
    server.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
    return;
  }
  uint32_t id = 0;
  if (!parseOnboardMediaId(server.pathArg(0), id)) {
    server.send(400, "application/json", "{\"error\":\"invalid_request\"}");
    return;
  }
  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  OnboardMediaInfo info{};
  File file;
  if (readOnboardMediaInfo(id, info)) file = onboardMediaFs().open(onboardMediaImagePath(id), FILE_READ);
  if (!file) {
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
    return;
  }
  server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server.streamFile(file, "image/jpeg");
  file.close();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
}

void handleOnboardMediaThumb() {
  uint32_t id = 0;
  OnboardMediaInfo info{};
  if (!onboardStorageReady) {
    server.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
  } else if (!parseOnboardMediaId(server.pathArg(0), id)) {
    server.send(400, "application/json", "{\"error\":\"invalid_request\"}");
  } else if (!readOnboardMediaInfo(id, info)) {
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
  } else {
    server.send(404, "application/json", "{\"error\":\"thumbnail_not_available\"}");
  }
}

void handleOnboardMediaDelete() {
  if (!onboardStorageReady) {
    server.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
    return;
  }
  uint32_t id = 0;
  if (!parseOnboardMediaId(server.pathArg(0), id)) {
    server.send(400, "application/json", "{\"error\":\"invalid_request\"}");
    return;
  }
  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  OnboardMediaInfo info{};
  if (!readOnboardMediaInfo(id, info)) {
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
    return;
  }
  fs::FS &fs = onboardMediaFs();
  const bool imageDeleted = fs.remove(onboardMediaImagePath(id));
  const bool metaDeleted = fs.remove(onboardMediaMetaPath(id));
  if (imageDeleted && metaDeleted) refreshOnboardMediaState();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  if (!imageDeleted || !metaDeleted) {
    server.send(500, "application/json", "{\"error\":\"delete_failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"id\":\"" + onboardMediaIdString(id) + "\"}");
}

void handleOnboardMediaDeleteAll() {
  if (!onboardStorageReady) {
    server.send(503, "application/json", "{\"error\":\"storage_unavailable\"}");
    return;
  }
  unsigned deleted = 0;
  unsigned failed = 0;
  deleteAllOnboardMedia(deleted, failed);
  String payload = "{\"ok\":" + String(failed == 0 ? "true" : "false") +
                   ",\"deleted\":" + String(deleted) + ",\"failed\":" + String(failed) + "}";
  server.send(failed == 0 ? 200 : 500, "application/json", payload);
}

bool parseOnboardTimelapseArgs(unsigned long &durationMs, unsigned long &intervalMs, String &error) {
  durationMs = 0;
  intervalMs = ONBOARD_TIMELAPSE_DEFAULT_INTERVAL_MS;

  if (server.hasArg("interval_ms")) {
    intervalMs = strtoul(server.arg("interval_ms").c_str(), nullptr, 10);
    if (intervalMs < ONBOARD_TIMELAPSE_MIN_INTERVAL_MS) {
      error = "invalid_interval_ms";
      return false;
    }
  }

  if (server.hasArg("duration_ms")) {
    durationMs = strtoul(server.arg("duration_ms").c_str(), nullptr, 10);
  } else if (server.hasArg("duration_minutes")) {
    const float minutes = server.arg("duration_minutes").toFloat();
    if (minutes > 0) {
      durationMs = static_cast<unsigned long>(minutes * 60.0f * 1000.0f);
    }
  } else if (server.hasArg("minutes")) {
    const float minutes = server.arg("minutes").toFloat();
    if (minutes > 0) {
      durationMs = static_cast<unsigned long>(minutes * 60.0f * 1000.0f);
    }
  } else if (server.hasArg("duration_hours")) {
    const float hours = server.arg("duration_hours").toFloat();
    if (hours > 0) {
      durationMs = static_cast<unsigned long>(hours * 60.0f * 60.0f * 1000.0f);
    }
  } else if (server.hasArg("hours")) {
    const float hours = server.arg("hours").toFloat();
    if (hours > 0) {
      durationMs = static_cast<unsigned long>(hours * 60.0f * 60.0f * 1000.0f);
    }
  }

  if (durationMs < ONBOARD_TIMELAPSE_MIN_INTERVAL_MS) {
    error = "invalid_duration";
    return false;
  }
  if (durationMs > ONBOARD_TIMELAPSE_MAX_DURATION_MS) {
    error = "duration_too_long";
    return false;
  }
  return true;
}

void handleOnboardTimelapseStart() {
  if (!onboardCameraReady) {
    server.send(503, "application/json", "{\"error\":\"onboard_camera_not_ready\"}");
    return;
  }

  unsigned long durationMs = 0;
  unsigned long intervalMs = 0;
  String error;
  if (!parseOnboardTimelapseArgs(durationMs, intervalMs, error)) {
    String payload = "{\"error\":\"" + jsonEscape(error) + "\"";
    payload += ",\"hint\":\"send hours, duration_hours, duration_minutes, minutes, or duration_ms; optional interval_ms defaults to 300000\"";
    payload += "}";
    server.send(400, "application/json", payload);
    return;
  }

  startOnboardTimelapse(durationMs, intervalMs);
  server.send(200, "application/json", buildOnboardCameraStatusJson());
}

void handleOnboardTimelapseStop() {
  stopOnboardTimelapse("stopped");
  server.send(200, "application/json", buildOnboardCameraStatusJson());
}

void handleOnboardCameraConfig() {
  bool configChanged = false;
  if (server.hasArg("enabled")) {
    const String enabled = server.arg("enabled");
    onboardCaptureEnabled = enabled == "1" || enabled == "true" || enabled == "yes";
    configChanged = true;
  }
  if (server.hasArg("interval_ms")) {
    const unsigned long requested = server.arg("interval_ms").toInt();
    if (requested >= 5000) {
      onboardCaptureIntervalMs = requested;
      configChanged = true;
    } else {
      server.send(400, "application/json", "{\"error\":\"invalid_interval_ms\"}");
      return;
    }
  }
  if (server.hasArg("start") || server.hasArg("start_minute")) {
    uint16_t requested = 0;
    const String value = server.hasArg("start") ? server.arg("start") : server.arg("start_minute");
    if (!parseMinuteOfDay(value, requested)) {
      server.send(400, "application/json", "{\"error\":\"invalid_start\"}");
      return;
    }
    onboardCaptureStartMinute = requested;
    configChanged = true;
  }
  if (server.hasArg("end") || server.hasArg("end_minute")) {
    uint16_t requested = 0;
    const String value = server.hasArg("end") ? server.arg("end") : server.arg("end_minute");
    if (!parseMinuteOfDay(value, requested)) {
      server.send(400, "application/json", "{\"error\":\"invalid_end\"}");
      return;
    }
    onboardCaptureEndMinute = requested;
    configChanged = true;
  }
  if (server.hasArg("tz_offset_min")) {
    onboardCaptureTzOffsetMin = static_cast<int16_t>(server.arg("tz_offset_min").toInt());
    configChanged = true;
  }
  if (server.hasArg("epoch")) {
    const time_t epoch = static_cast<time_t>(server.arg("epoch").toInt());
    if (epoch < 1700000000) {
      server.send(400, "application/json", "{\"error\":\"invalid_epoch\"}");
      return;
    }
    timeval tv{};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    backfillOnboardMediaTimestamps();
    configChanged = true;
  }
  String sensorError;
  if (!applyOnboardCameraSensorArgs(sensorError, true)) {
    server.send(400, "application/json", "{\"error\":\"" + jsonEscape(sensorError) + "\"}");
    return;
  }
  if (configChanged) {
    clampOnboardConfig();
    saveOnboardConfig();
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
  int scanCode = 0;
  if (!runIdleWifiScan(payload, error, scanCode)) {
    String body = "{";
    body += "\"error\":\"" + jsonEscape(error) + "\"";
    if (error == "scan_failed") {
      body += ",\"scan_code\":" + String(scanCode);
      body += ",\"hint\":\"ESP32 WiFi driver rejected the scan after STA reset\"";
    } else if (error == "camera_wifi_active") {
      body += ",\"hint\":\"scan requires idle trail-camera WiFi\"";
    }
    body += "}";
    server.send(409, "application/json", body);
    return;
  }
  server.send(200, "application/json", payload);
}

void handleUploadStatus() {
  server.send(200, "application/json", buildUploadStatusJson());
}

void handleUploadBleLast() {
  if (bleObservationsLastJson.isEmpty()) {
    server.send(404, "application/json", "{\"error\":\"no_ble_observation_payload\"}");
    return;
  }
  server.send(200, "application/json", bleObservationsLastJson);
}

void handleSdStatus() {
  server.send(200, "application/json", buildSdStatusJson());
}

void handleSdMount() {
  const bool ok = mountSdCard();
  server.send(ok ? 200 : 503, "application/json", buildSdStatusJson());
}

void handleUploadTelemetry() {
  String body;
  int statusCode = 0;
  const bool ok = uploadTelemetryNow(body, statusCode);
  server.send(ok ? 200 : 502, "application/json", buildUploadResultJson(ok, statusCode, body));
}

void handleUploadEvents() {
  String body;
  int statusCode = 0;
  const bool ok = uploadQueuedEventsNow(body, statusCode);
  server.send(ok ? 200 : 502, "application/json", buildUploadResultJson(ok, statusCode, body));
}

void handleUploadObservations() {
  if (observationUploadMutex != nullptr && xSemaphoreTake(observationUploadMutex, pdMS_TO_TICKS(60000)) != pdTRUE) {
    server.send(409, "application/json", "{\"error\":\"observation_upload_busy\"}");
    return;
  }
  String queueBody;
  const bool queueOk = replayQueuedObservationBatches(4, queueBody);
  String wifiBody;
  int wifiStatus = 0;
  const bool wifiOk = uploadWifiObservationsNow(wifiBody, wifiStatus);
  String bleBody;
  int bleStatus = 0;
  const bool bleOk = uploadBleObservationsNow(bleBody, bleStatus);
  String payload = "{";
  payload += "\"ok\":" + String((queueOk && wifiOk && bleOk) ? "true" : "false");
  payload += ",\"queued_replay\":" + queueBody;
  payload += ",\"wifi\":" + buildUploadResultJson(wifiOk, wifiStatus, wifiBody);
  payload += ",\"ble\":" + buildUploadResultJson(bleOk, bleStatus, bleBody);
  payload += "}";
  if (observationUploadMutex != nullptr) {
    xSemaphoreGive(observationUploadMutex);
  }
  server.send((queueOk && wifiOk && bleOk) ? 200 : 502, "application/json", payload);
}

void handleUploadAll() {
  String telemetryBody;
  int telemetryStatus = 0;
  const bool telemetryOk = uploadTelemetryNow(telemetryBody, telemetryStatus);

  String eventsBody;
  int eventsStatus = 0;
  const bool eventsOk = uploadQueuedEventsNow(eventsBody, eventsStatus);

  String payload = "{";
  payload += "\"ok\":" + String((telemetryOk && eventsOk) ? "true" : "false");
  payload += ",\"telemetry\":";
  payload += buildUploadResultJson(telemetryOk, telemetryStatus, telemetryBody);
  payload += ",\"events\":";
  payload += buildUploadResultJson(eventsOk, eventsStatus, eventsBody);
  payload += "}";
  server.send((telemetryOk && eventsOk) ? 200 : 502, "application/json", payload);
}

void handleCameraRawGet() {
  const unsigned long httpStartedMs = millis();
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
    lastCameraHttpElapsedMs = millis() - httpStartedMs;
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
      lastCameraHttpElapsedMs = millis() - httpStartedMs;
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
  lastCameraHttpElapsedMs = millis() - httpStartedMs;
  if (statusCode >= 200 && statusCode < 300) {
    markCameraActivity();
  }
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

void handleSessionLease() {
  unsigned long ttlMs = CAMERA_SESSION_DEFAULT_LEASE_MS;
  if (server.hasArg("ttl_ms")) {
    ttlMs = strtoul(server.arg("ttl_ms").c_str(), nullptr, 10);
  } else if (server.hasArg("seconds")) {
    ttlMs = strtoul(server.arg("seconds").c_str(), nullptr, 10) * 1000UL;
  }
  const bool standbyOnExpire = !server.hasArg("standby_on_expire") ||
                               server.arg("standby_on_expire") != "0";
  startCameraSessionLease(ttlMs, standbyOnExpire);
  server.send(200, "application/json", buildCameraSessionJson());
}

void handleSessionRelease() {
  const bool requestStandby = !server.hasArg("standby") || server.arg("standby") != "0";
  releaseCameraSessionLease();
  if (requestStandby && wifiConnected) {
    String body;
    int statusCode = 500;
    proxyCameraRequest("GET", "/cmd/standby/now", body, statusCode);
  }
  server.send(200, "application/json", buildCameraSessionJson());
}

void handleSessionStatus() {
  server.send(200, "application/json", buildCameraSessionJson());
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

bool jsonObjectHasIntField(const String &objectJson, const char *fieldName, int expectedValue) {
  const String key = String("\"") + String(fieldName) + "\"";
  int index = objectJson.indexOf(key);
  while (index >= 0) {
    const int colon = objectJson.indexOf(':', index + key.length());
    if (colon < 0) {
      return false;
    }
    int valueStart = colon + 1;
    while (valueStart < static_cast<int>(objectJson.length()) && objectJson[valueStart] == ' ') {
      ++valueStart;
    }
    const int value = objectJson.substring(valueStart).toInt();
    if (value == expectedValue) {
      return true;
    }
    index = objectJson.indexOf(key, colon + 1);
  }
  return false;
}

String extractLatestMediaItemJson(const String &galleryJson, int typeFilter, bool &found) {
  found = false;
  const int dataKey = galleryJson.indexOf("\"data\"");
  if (dataKey < 0) {
    return "";
  }
  const int arrayStart = galleryJson.indexOf('[', dataKey);
  if (arrayStart < 0) {
    return "";
  }

  bool inString = false;
  bool escaped = false;
  int depth = 0;
  int objectStart = -1;
  for (int i = arrayStart + 1; i < static_cast<int>(galleryJson.length()); ++i) {
    const char c = galleryJson[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\' && inString) {
      escaped = true;
      continue;
    }
    if (c == '"') {
      inString = !inString;
      continue;
    }
    if (inString) {
      continue;
    }
    if (c == '{') {
      if (depth == 0) {
        objectStart = i;
      }
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && objectStart >= 0) {
        const String item = galleryJson.substring(objectStart, i + 1);
        if (typeFilter < 0 || jsonObjectHasIntField(item, "type", typeFilter)) {
          found = true;
          return item;
        }
        objectStart = -1;
      }
    } else if (c == ']' && depth == 0) {
      break;
    }
  }
  return "";
}

void handleCameraLatest() {
  if (!wifiConnected) {
    server.send(503, "application/json", "{\"error\":\"camera_wifi_down\"}");
    return;
  }

  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 6;
  if (limit < 1) {
    limit = 1;
  } else if (limit > 20) {
    limit = 20;
  }
  int typeFilter = -1;
  if (server.hasArg("type")) {
    typeFilter = server.arg("type").toInt();
  }

  String body;
  int statusCode = 500;
  const String path = "/list/detail/backward/900000/" + String(limit);
  proxyCameraRequest("GET", path, body, statusCode);
  if (statusCode < 200 || statusCode >= 300) {
    server.send(statusCode, "application/json", body);
    return;
  }

  bool found = false;
  const String item = extractLatestMediaItemJson(body, typeFilter, found);
  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"limit\":" + String(limit);
  payload += ",\"type_filter\":" + String(typeFilter);
  payload += ",\"data\":";
  payload += found ? item : String("null");
  payload += "}";
  server.send(200, "application/json", payload);
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
      message = ok ? "stream_active" : lastStreamStartMessage.c_str();
      if (!ok && (message == nullptr || message[0] == '\0')) {
        message = "stream_start_failed";
      }
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
    server.begin();
    return;
  }
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/camera/raw", HTTP_GET, handleCameraRawGet);
  server.on("/camera/request", HTTP_GET, handleCameraRequest);
  server.on("/camera/request", HTTP_POST, handleCameraRequest);
  server.on("/control/bringup", HTTP_POST, handleControlBringup);
  server.on("/control/stream_start", HTTP_POST, handleControlStreamStart);
  server.on("/control/stream_stop", HTTP_POST, handleControlStreamStop);
  server.on("/session/lease", HTTP_POST, handleSessionLease);
  server.on("/session/release", HTTP_POST, handleSessionRelease);
  server.on("/session/status", HTTP_GET, handleSessionStatus);
  server.on("/battery/status", HTTP_GET, handleBatteryStatus);
  server.on("/onboard/status", HTTP_GET, handleOnboardCameraStatus);
  server.on("/onboard/latest.jpg", HTTP_GET, handleOnboardCameraLatest);
  server.on("/onboard/capture", HTTP_POST, handleOnboardCameraCapture);
  server.on("/onboard/media", HTTP_GET, handleOnboardMediaList);
  server.on("/onboard/media/delete_all", HTTP_POST, handleOnboardMediaDeleteAll);
  server.on(UriRegex("^\\/onboard\\/media\\/([0-9]+)\\/thumb$"), HTTP_GET, handleOnboardMediaThumb);
  server.on(UriRegex("^\\/onboard\\/media\\/([0-9]+)$"), HTTP_GET, handleOnboardMediaFile);
  server.on(UriRegex("^\\/onboard\\/media\\/([0-9]+)$"), HTTP_DELETE, handleOnboardMediaDelete);
  server.on("/onboard/config", HTTP_POST, handleOnboardCameraConfig);
  server.on("/onboard/timelapse/start", HTTP_POST, handleOnboardTimelapseStart);
  server.on("/onboard/timelapse/stop", HTTP_POST, handleOnboardTimelapseStop);
  server.on("/scan/wifi", HTTP_GET, handleWifiScan);
  server.on("/upload/status", HTTP_GET, handleUploadStatus);
  server.on("/upload/ble/last", HTTP_GET, handleUploadBleLast);
  server.on("/sd/status", HTTP_GET, handleSdStatus);
  server.on("/sd/mount", HTTP_POST, handleSdMount);
  server.on("/upload/telemetry", HTTP_POST, handleUploadTelemetry);
  server.on("/upload/events", HTTP_POST, handleUploadEvents);
  server.on("/upload/observations", HTTP_POST, handleUploadObservations);
  server.on("/upload/all", HTTP_POST, handleUploadAll);
  server.on("/camera/info/1", HTTP_GET, handleCameraInfo1);
  server.on("/camera/info/2", HTTP_GET, handleCameraInfo2);
  server.on("/camera/info/3", HTTP_GET, handleCameraInfo3);
  server.on("/camera/info/4", HTTP_GET, handleCameraInfo4);
  server.on("/camera/info/5", HTTP_GET, handleCameraInfo5);
  server.on("/camera/info/6", HTTP_GET, handleCameraInfo6);
  server.on("/camera/getParaSetting", HTTP_GET, handleParaSettings);
  server.on("/camera/gallery", HTTP_GET, handleGalleryList);
  server.on("/camera/latest", HTTP_GET, handleCameraLatest);
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
    stats->lastSourceIp = IPAddress(srcAddr.sin_addr.s_addr);
    stats->lastSourcePort = ntohs(srcAddr.sin_port);
    stats->lastPacketLen = static_cast<size_t>(len);
    if (primary) {
      mediaPrimaryPackets++;
      mediaPrimaryBytes += len;
      lastPrimaryPacketMs = millis();
    } else {
      mediaSecondaryPackets++;
      mediaSecondaryBytes += len;
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
  if (cmd == "halow_status") {
    Serial.println(buildHaLowStatusJson());
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
  if (cmd == "onboard_delete_all") {
    unsigned deleted = 0;
    unsigned failed = 0;
    const bool ok = deleteAllOnboardMedia(deleted, failed);
    Serial.printf("[onboard-camera] delete_all ok=%s deleted=%u failed=%u %s\n",
                  ok && failed == 0 ? "true" : "false",
                  deleted,
                  failed,
                  buildOnboardCameraStatusJson().c_str());
    return;
  }
  if (cmd == "sd_status") {
    Serial.println(buildSdStatusJson());
    return;
  }
  if (cmd == "sd_mount") {
    mountSdCard();
    Serial.println(buildSdStatusJson());
    return;
  }
  if (cmd.startsWith("onboard_config")) {
    String args = cmd.substring(strlen("onboard_config"));
    args.trim();
    if (args.isEmpty()) {
      Serial.println(buildOnboardCameraStatusJson());
      return;
    }
    bool allOk = true;
    while (!args.isEmpty()) {
      int space = args.indexOf(' ');
      String token = space >= 0 ? args.substring(0, space) : args;
      args = space >= 0 ? args.substring(space + 1) : "";
      args.trim();
      token.trim();
      if (token.isEmpty()) {
        continue;
      }
      const int equals = token.indexOf('=');
      if (equals <= 0) {
        Serial.printf("[onboard-config] invalid token=%s\n", token.c_str());
        allOk = false;
        continue;
      }
      String key = token.substring(0, equals);
      String value = token.substring(equals + 1);
      key.toLowerCase();
      String error;
      const bool ok = applyOnboardCameraSetting(key, value, error);
      if (!ok) {
        Serial.printf("[onboard-config] %s=%s failed error=%s\n",
                      key.c_str(),
                      value.c_str(),
                      error.c_str());
        allOk = false;
      } else {
        Serial.printf("[onboard-config] %s=%s ok\n", key.c_str(), value.c_str());
      }
    }
    Serial.printf("[onboard-config] result=%s %s\n",
                  allOk ? "ok" : "partial_failure",
                  buildOnboardCameraStatusJson().c_str());
    return;
  }
  if (cmd.startsWith("onboard_timelapse")) {
    if (cmd == "onboard_timelapse_stop") {
      stopOnboardTimelapse("stopped");
      Serial.println(buildOnboardCameraStatusJson());
      return;
    }
    String args = cmd.substring(strlen("onboard_timelapse"));
    args.trim();
    unsigned long durationMs = 0;
    unsigned long intervalMs = ONBOARD_TIMELAPSE_DEFAULT_INTERVAL_MS;
    bool allOk = true;
    while (!args.isEmpty()) {
      int space = args.indexOf(' ');
      String token = space >= 0 ? args.substring(0, space) : args;
      args = space >= 0 ? args.substring(space + 1) : "";
      args.trim();
      token.trim();
      if (token.isEmpty()) {
        continue;
      }
      const int equals = token.indexOf('=');
      if (equals <= 0) {
        Serial.printf("[onboard-timelapse] invalid token=%s\n", token.c_str());
        allOk = false;
        continue;
      }
      String key = token.substring(0, equals);
      String value = token.substring(equals + 1);
      key.toLowerCase();
      String error;
      if (!applyOnboardTimelapseSerialArg(key, value, durationMs, intervalMs, error)) {
        Serial.printf("[onboard-timelapse] %s=%s failed error=%s\n",
                      key.c_str(),
                      value.c_str(),
                      error.c_str());
        allOk = false;
      }
    }
    if (!allOk || durationMs < ONBOARD_TIMELAPSE_MIN_INTERVAL_MS) {
      Serial.println("[onboard-timelapse] failed error=invalid_duration");
      return;
    }
    if (durationMs > ONBOARD_TIMELAPSE_MAX_DURATION_MS) {
      Serial.println("[onboard-timelapse] failed error=duration_too_long");
      return;
    }
    startOnboardTimelapse(durationMs, intervalMs);
    Serial.println(buildOnboardCameraStatusJson());
    return;
  }
  if (cmd == "onboard_dump" || cmd == "onboard_dump fresh") {
    dumpOnboardJpegBase64(cmd.endsWith("fresh"));
    return;
  }
  if (cmd == "wifi_scan") {
    String payload;
    String error;
    int scanCode = 0;
    if (runIdleWifiScan(payload, error, scanCode)) {
      Serial.println(payload);
    } else if (error == "scan_failed") {
      Serial.printf("[wifi-scan] failed error=%s code=%d\n", error.c_str(), scanCode);
    } else {
      Serial.printf("[wifi-scan] failed error=%s\n", error.c_str());
    }
    return;
  }
  if (cmd == "upload_status") {
    Serial.println(buildUploadStatusJson());
    return;
  }
  if (cmd == "upload_telemetry") {
    String body;
    int statusCode = 0;
    const bool ok = uploadTelemetryNow(body, statusCode);
    Serial.println(buildUploadResultJson(ok, statusCode, body));
    return;
  }
  if (cmd == "upload_events") {
    String body;
    int statusCode = 0;
    const bool ok = uploadQueuedEventsNow(body, statusCode);
    Serial.println(buildUploadResultJson(ok, statusCode, body));
    return;
  }
  if (cmd == "upload_all") {
    String telemetryBody;
    int telemetryStatus = 0;
    const bool telemetryOk = uploadTelemetryNow(telemetryBody, telemetryStatus);
    String eventsBody;
    int eventsStatus = 0;
    const bool eventsOk = uploadQueuedEventsNow(eventsBody, eventsStatus);
    Serial.printf("[upload-all] telemetry=%s status=%d events=%s status=%d\n",
                  telemetryOk ? "ok" : "failed",
                  telemetryStatus,
                  eventsOk ? "ok" : "failed",
                  eventsStatus);
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
  WiFi.setHostname(BOARD_HOSTNAME);
  Serial.printf("[network] hostname=%s\n", BOARD_HOSTNAME);
  initBootIdentity();
  enqueueUploadEvent("boot", "startup", "{\"firmware\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"}", true);

  pinMode(BAT_ADC_CTRL_PIN, OUTPUT);
  digitalWrite(BAT_ADC_CTRL_PIN, LOW);
  pinMode(BAT_CHRG_PIN, INPUT_PULLUP);
  pinMode(BAT_DONE_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  Serial.printf("[battery] %s\n", buildBatteryJson().c_str());

  onboardFrameMutex = xSemaphoreCreateMutex();
  onboardStorageMutex = xSemaphoreCreateMutex();
  observationUploadMutex = xSemaphoreCreateMutex();
  initOnboardStorage();
  mountSdCard();
  loadOnboardConfig();
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
    startHttpServer();
    if (!halowConnected) {
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
    startWifiScannerTask();
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
  startWifiScannerTask();

  xTaskCreatePinnedToCore(udpForwardTask, "udp-primary", 4096, reinterpret_cast<void *>(0), 1, nullptr, 0);
  xTaskCreatePinnedToCore(udpForwardTask, "udp-secondary", 4096, reinterpret_cast<void *>(1), 1, nullptr, 0);
}

void loop() {
  pollSerialConsole();

  if (RUN_LOCAL_SERIAL_TEST) {
    halowConnected = (HaLow.status() == WL_CONNECTED);
    if (!httpServerStarted) {
      startHttpServer();
    }
    if (httpServerStarted) {
      server.handleClient();
    }
    if (halowConnected && !onboardClockValid() &&
        (lastClockSyncAttemptMs == 0 || millis() - lastClockSyncAttemptMs > 60000UL)) {
      lastClockSyncAttemptMs = millis();
      syncBoardClockFromAirScan();
    }
    refreshWifiState();
    processCameraSessionLeaseExpiry();
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

  if (halowConnected && !onboardClockValid() &&
      (lastClockSyncAttemptMs == 0 || millis() - lastClockSyncAttemptMs > 60000UL)) {
    lastClockSyncAttemptMs = millis();
    syncBoardClockFromAirScan();
  }

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
