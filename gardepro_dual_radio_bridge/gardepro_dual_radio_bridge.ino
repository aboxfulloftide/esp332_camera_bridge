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
#include <Update.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

// Shared helpers (cooperative delays, SD/camera loops) run on tasks that are
// not all subscribed to the task watchdog. esp_task_wdt_reset() on such a task
// logs "task not found" thousands of times a minute, which stalls serial.
// (A macro, not a function: a function here would make the Arduino builder
// insert its generated prototypes above the remaining #includes.)
#define feedTaskWdt() \
  do { if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset(); } while (0)
#include <mbedtls/sha256.h>
#include <uri/UriRegex.h>
#include <algorithm>
#include <vector>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

extern "C" {
#include <lwip/sockets.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
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
#ifndef CAMERA_WIFI_SSID
#define CAMERA_WIFI_SSID "CAM8Z8_A4C138982635"
#endif
#ifndef CAMERA_WIFI_PASS
#define CAMERA_WIFI_PASS "1234567890"
#endif
#ifndef TRANSFER_WIFI_SSID
#define TRANSFER_WIFI_SSID ""
#endif
#ifndef TRANSFER_WIFI_PASS
#define TRANSFER_WIFI_PASS ""
#endif
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
#define AIR_SCAN_API_HOST "192.168.1.42"
#endif
#ifndef AIR_SCAN_API_PORT
#define AIR_SCAN_API_PORT 80
#endif
#ifndef SCANNER_SCHEDULE_ENABLED
#define SCANNER_SCHEDULE_ENABLED 0
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
static const char *FIRMWARE_VERSION = "0.2.77";
static const char *FIRMWARE_BUILD = __DATE__ " " __TIME__;
static const char *DIAGNOSTIC_DIR = "/diagnostics";
static const char *DIAGNOSTIC_LOG_PATH = "/diagnostics/health.jsonl";
static const char *DURABLE_JOB_DIR = "/jobs";
static const char *DURABLE_JOB_STATE_PATH = "/jobs/media_job.bin";
static const char *DURABLE_JOB_STATE_TEMP_PATH = "/jobs/media_job.tmp";
static const char *DURABLE_JOB_STATE_BACKUP_PATH = "/jobs/media_job.bak";
static const char *DURABLE_JOB_MANIFEST_PATH = "/jobs/media_manifest.bin";
static const char *TRAIL_MEDIA_UPLOAD_CURSOR_PATH = "/jobs/media_upload_cursor.bin";
static const char *TRAIL_MEDIA_DIR = "/trail_media";
static const unsigned long TRAIL_MEDIA_UPLOAD_RETRY_MS = 30000UL;
static const uint8_t TRAIL_MEDIA_MAX_UPLOAD_ATTEMPTS = 3;
static const unsigned long FAILSAFE_CHECK_INTERVAL_MS = 10000UL;
static const unsigned long FAILSAFE_HTTP_STALL_MS = 60000UL;
static const unsigned long FAILSAFE_UPLOAD_STALL_MS = 180000UL;
static const unsigned long FAILSAFE_LONG_HTTP_STALL_MS = 300000UL;
static const unsigned long HTTP_SELF_TEST_INTERVAL_MS = 60000UL;
static const uint16_t TRANSFER_WIFI_BATCH_FILES = 25;
static const unsigned long TRANSFER_WIFI_SESSION_MAX_MS = 600000UL;
// Automatic transfer Wi-Fi currently panics this dual-radio build during
// route handoff. Keep the explicit diagnostic endpoint available, but use
// HaLow for unattended uploads until interface routing is repaired.
static const bool TRANSFER_WIFI_UPLOAD_ENABLED = true;
static const unsigned long DIAGNOSTIC_SD_INTERVAL_MS = 300000UL;
static const unsigned long DIAGNOSTIC_NVS_INTERVAL_MS = 21600000UL;
#ifndef CAMERA_BLE_MAC
#define CAMERA_BLE_MAC "a4:c1:38:98:26:35"
#endif
#ifndef CAMERA_BLE_WAKE
#define CAMERA_BLE_WAKE "AT+WAKEPULSE=10\r\n"
#endif
#ifndef CAMERA_BLE_NAME
#define CAMERA_BLE_NAME "CAM8Z8_NoName_G_E6"
#endif
#ifndef CAMERA_BLE_NAME_PREFIX
#define CAMERA_BLE_NAME_PREFIX "CAM8Z8_"
#endif
#ifndef CAMERA_BLE_SERVICE_UUID
#define CAMERA_BLE_SERVICE_UUID "6e000100-b5a3-f393-e0a9-e50e24dcca9e"
#endif
#ifndef CAMERA_BLE_GATT_SERVICE_UUID
#define CAMERA_BLE_GATT_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#endif
#ifndef CAMERA_BLE_NOTIFY_UUID
#define CAMERA_BLE_NOTIFY_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#endif
#ifndef CAMERA_BLE_DATA_UUID
#define CAMERA_BLE_DATA_UUID "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#endif
#ifndef CAMERA_DISCOVER_ANY_GARDEPRO
#define CAMERA_DISCOVER_ANY_GARDEPRO 1
#endif
static const size_t BLE_RECENT_DEVICE_SLOTS = 16;

#ifndef ONBOARD_CAPTURE_INTERVAL_MS
#define ONBOARD_CAPTURE_INTERVAL_MS 86400000UL
#endif
#ifndef ONBOARD_CAPTURE_ENABLED
#define ONBOARD_CAPTURE_ENABLED 1
#endif
#ifndef ONBOARD_CAPTURE_START_MINUTE
#define ONBOARD_CAPTURE_START_MINUTE 720
#endif
#ifndef ONBOARD_CAPTURE_END_MINUTE
#define ONBOARD_CAPTURE_END_MINUTE 750
#endif

static const int BAT_ADC_PIN = 1;
static const int BAT_ADC_CTRL_PIN = 20;
static const int BAT_ADC_CTRL_ACTIVE_LEVEL = LOW;
static const int BAT_ADC_CTRL_IDLE_LEVEL = HIGH;
static const uint32_t BATTERY_BOOT_MIN_MV = 3400;
static const uint32_t BATTERY_BOOT_RECOVER_MV = 3600;
static const uint8_t BATTERY_BOOT_STABLE_SAMPLES = 3;
static const unsigned long BATTERY_LOW_POWER_RECHECK_MS = 60000UL;
static const unsigned long BATTERY_LOW_POWER_RESTART_MS = 30UL * 60UL * 1000UL;
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
// Join the camera AP as 802.11b-only first (see connectCameraWifi()).
bool cameraWifiPrefer11b = true;
bool cameraWifiLast11b = false;
portMUX_TYPE halowConnectMux = portMUX_INITIALIZER_UNLOCKED;
bool halowConnectInProgress = false;
unsigned long halowLastRecoveryAttemptMs = 0;
unsigned long halowRecoveryIntervalMs = 30000UL;
unsigned long halowDownSinceMs = 0;
uint32_t halowRecoveryAttempts = 0;
uint32_t halowRecoverySuccesses = 0;
unsigned long halowLastLivenessProbeMs = 0;
uint32_t halowLivenessProbeSuccesses = 0;
uint32_t halowLivenessProbeFailures = 0;
uint8_t halowConsecutiveLivenessFailures = 0;
uint32_t halowForcedReconnects = 0;
int halowLastLivenessError = 0;
unsigned long halowLastGratuitousArpMs = 0;
uint32_t halowGratuitousArpCount = 0;
uint32_t halowGratuitousArpFailures = 0;
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
String cameraTargetBleMac = CAMERA_BLE_MAC;
String cameraTargetBleName = CAMERA_BLE_NAME;
String cameraTargetWifiSsid = CAMERA_WIFI_SSID;
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
struct CameraProfile {
  const char *id;
  const char *label;
  const char *bleMac;
  const char *bleName;
  const char *wifiSsid;
};
static const CameraProfile CAMERA_PROFILES[] = {
  {"e6_original", "GardePro E6 replacement", "a4:c1:38:98:26:35", "CAM8Z8_NoName_G_E6", "CAM8Z8_A4C138982635"},
  {"e6_plus", "GardePro E6+", "a4:c1:38:98:81:48", "CAM8Z8_NoName_G_E6+", "CAM8Z8_A4C138988148"},
};
String serialCommandBuffer;
bool udpInspectorsStarted = false;
volatile bool udpPrimaryReady = false;
volatile bool udpSecondaryReady = false;
uint32_t rtpPacketsReceived = 0;
uint32_t rtpPacketsForwarded = 0;
uint32_t rtpSequenceGaps = 0;
uint32_t rtpPacketsMissing = 0;
uint32_t rtpOutOfOrder = 0;
uint32_t rtpForwardFailures = 0;
uint32_t udpReceiveOverruns = 0;
uint32_t rtpPacketsReceivedTotal = 0;
uint32_t rtpPacketsForwardedTotal = 0;
uint32_t rtpPacketsMissingTotal = 0;
uint32_t rtpForwardFailuresTotal = 0;
uint32_t udpReceiveOverrunsTotal = 0;
uint16_t lastRtpSequence = 0;
bool haveLastRtpSequence = false;
bool rtpKeyframeSeen = false;
uint32_t rtpKeyframesObserved = 0;
static const uint32_t RTP_MAX_HEALTH_LOSS_PCT = 20;
bool streamSessionActive = false;
volatile bool streamForwardingEnabled = false;
uint32_t streamSessionId = 0;
String lastStreamRecoveryReason;
bool lastStreamRecoverySucceeded = false;
unsigned long lastStreamRecoveryStartedMs = 0;
unsigned long lastStreamRecoveryFinishedMs = 0;
unsigned long lastStreamRecoveryDurationMs = 0;
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
unsigned long lastStreamStallMs = 0;
uint32_t streamStallCount = 0;
uint32_t rtspKeepaliveCount = 0;
uint32_t rtspKeepaliveFailures = 0;
int lastRtspKeepaliveStatus = 0;
String lastRtspKeepaliveMethod = "";
static const unsigned long RTSP_KEEPALIVE_INTERVAL_MS = 5000;
static const unsigned long TUNNEL_CONNECT_TIMEOUT_MS = 3000;
static const unsigned long BLE_KEEPALIVE_INTERVAL_MS = 20000;
static const unsigned long HTTP_KEEPALIVE_INTERVAL_MS = 15000;
static const unsigned long TUNNEL_RECONNECT_INTERVAL_MS = 2000;
static const unsigned long STREAM_STALL_TIMEOUT_MS = 4000;
static const unsigned long STREAM_STALL_FAIL_MS = 15000;
static const unsigned long STREAM_RECOVERY_COOLDOWN_MS = 10000;
static const unsigned long IDLE_WIFI_RECOVERY_COOLDOWN_MS = 30000;
static const unsigned long HALOW_RECOVERY_INTERVAL_MS = 30000;
static const unsigned long HALOW_RECOVERY_MAX_INTERVAL_MS = 600000;
static const unsigned long HALOW_CONNECT_WAIT_MS = 75000;
static const unsigned long HALOW_LIVENESS_INTERVAL_MS = 30000;
static const uint8_t HALOW_LIVENESS_FAILURE_THRESHOLD = 2;
static const unsigned long HALOW_GRATUITOUS_ARP_INTERVAL_MS = 5000;
static const unsigned long SD_BACKGROUND_RECOVERY_INTERVAL_MS = 15000;
static const unsigned long SD_BACKGROUND_RECOVERY_MAX_INTERVAL_MS = 600000;
static const uint32_t SD_RECOVERY_MIN_BATTERY_MV = 3500;
static const uint32_t MEDIA_UPLOAD_MIN_BATTERY_MV = 3600;
static const uint32_t CAMERA_BRINGUP_MIN_BATTERY_MV = 3600;
static const uint32_t DURABLE_MEDIA_MIN_BATTERY_MV = 3800;
static const uint32_t DURABLE_MEDIA_RECOVER_BATTERY_MV = 3900;
static const unsigned long DURABLE_MEDIA_LOW_BATTERY_RECHECK_MS = 30000UL;
static const unsigned long STORAGE_BATTERY_CHECK_INTERVAL_MS = 30000;
static const unsigned long CAMERA_SESSION_DEFAULT_LEASE_MS = 120000;
static const unsigned long CAMERA_SESSION_MAX_LEASE_MS = 600000;
static const unsigned long CAMERA_IDLE_HOLD_MS = 120000;
static const uint8_t HTTP_KEEPALIVE_FAILURE_THRESHOLD = 2;
#ifndef CAMERA_HTTP_KEEPALIVE_ENABLED
#define CAMERA_HTTP_KEEPALIVE_ENABLED 0
#endif
#ifndef CAMERA_AUTO_RECOVERY_ENABLED
#define CAMERA_AUTO_RECOVERY_ENABLED 0
#endif
#ifndef HTTP_SERVER_IDLE_RESTART_MS
#define HTTP_SERVER_IDLE_RESTART_MS 0UL
#endif
// Camera BLE/Wi-Fi bring-up previously ran beside the HaLow driver on core 0.
// Under repeated camera reconnects that combination starved IDLE0 and caused
// ESP_RST_TASK_WDT resets. Keep outbound staged uploads on core 0, but run the
// serialized camera control/durable-job worker on the application core.
// Camera BLE/Wi-Fi work must not share core 0 with the HaLow driver. BLE scan
// activity on core 0 starves the radio task and trips the task watchdog.
static const BaseType_t CONTROL_WORKER_CORE = 1;
static const BaseType_t TRAIL_UPLOAD_WORKER_CORE = 0;
static const BaseType_t HTTP_SERVICE_CORE = 1;
static const uint8_t BLE_SCAN_ATTEMPTS = 2;
static const uint16_t BLE_SCAN_WINDOW_SEC = 5;
static const unsigned long BLE_SCAN_RETRY_DELAY_MS = 1000;
static const uint8_t BLE_PASSIVE_SCAN_ATTEMPTS = 2;
static const uint16_t BLE_PASSIVE_SCAN_WINDOW_SEC = 8;
// The trail cameras can advertise only briefly after a cold boot.  A 50%
// scan duty cycle routinely missed the E6+ even though a nearby Linux BLE
// adapter saw one packet during a 30 second scan.  Use continuous scanning
// for camera bring-up so a sparse advertisement cannot land in a scan gap.
static const uint16_t BLE_SCAN_INTERVAL_MS = 100;
static const uint16_t BLE_SCAN_WINDOW_MS = 100;
static const unsigned long BLE_INIT_WARMUP_MS = 5000;
static const unsigned long BLE_REUSE_WARMUP_MS = 750;
static const unsigned long BLE_SCAN_CONNECT_SETTLE_MS = 500;
static const unsigned long BLE_CONNECT_RETRY_DELAY_MS = 1000;
static const uint8_t BLE_CONNECT_ATTEMPTS = 3;
static const uint16_t BLE_CONNECT_TIMEOUT_MS = 15000;
static const uint8_t BLE_DIRECT_CONNECT_ATTEMPTS = 1;
static const uint16_t BLE_DIRECT_CONNECT_TIMEOUT_MS = 5000;
static const uint8_t BLE_WAKE_PULSE_ATTEMPTS = 3;
static const unsigned long BLE_WAKE_PULSE_DELAY_MS = 350;
static const size_t BLE_OBSERVATION_DEVICE_LIMIT = 96;
static const size_t BLE_OBSERVATION_FIELD_LIMIT = 512;
static const uint32_t BLE_OBSERVATION_SCAN_WINDOW_MS = 10000UL;
static const unsigned long BRINGUP_HOTSPOT_WAIT_MS = 20000;
static const unsigned long BRINGUP_FAILED_WAKE_HOTSPOT_WAIT_MS = 5000;
static const unsigned long BRINGUP_HOTSPOT_POLL_MS = 3000;
static const unsigned long BRINGUP_HOTSPOT_FAST_WINDOW_MS = 10000;
static const unsigned long BRINGUP_HOTSPOT_FAST_POLL_MS = 1000;
static const uint32_t UDP_LOG_FIRST_PACKETS = 8;
static const uint32_t UDP_LOG_EVERY_N_PACKETS = 120;
uint32_t streamRecoveryAttempts = 0;
uint32_t idleRecoveryAttempts = 0;

unsigned long msSince(unsigned long timestampMs);
void serviceBoardHttp();
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
unsigned long lastHttpServiceMs = 0;
unsigned long httpServiceGapMaxMs = 0;
unsigned long httpServiceLastGapMs = 0;
unsigned long lastHttpServerRestartMs = 0;
uint32_t httpServiceCount = 0;
uint32_t httpServerRestartCount = 0;
String httpServerLastRestartReason = "boot";
SemaphoreHandle_t httpServiceMutex = nullptr;
SemaphoreHandle_t tunnelWriteMutex = nullptr;
TaskHandle_t controlWorkerTaskHandle = nullptr;
TaskHandle_t wifiScannerTaskHandle = nullptr;
TaskHandle_t httpServiceTaskHandle = nullptr;
TaskHandle_t trailMediaUploadTaskHandle = nullptr;
TaskHandle_t failsafeTaskHandle = nullptr;
volatile unsigned long loopHeartbeatMs = 0;
volatile bool cameraBringupInProgress = false;
unsigned long setupStartedMs = 0;
SemaphoreHandle_t observationUploadMutex = nullptr;

SemaphoreHandle_t onboardFrameMutex = nullptr;
SemaphoreHandle_t onboardStorageMutex = nullptr;
// Serializes SD.end()/SD.begin() and the follow-up job reload. The durable
// job worker and loop()'s background recovery could otherwise remount the
// card concurrently. Recursive so a retry sequence can hold it across mounts.
SemaphoreHandle_t sdMountMutex = nullptr;
SemaphoreHandle_t onboardCameraMutex = nullptr;
SemaphoreHandle_t wifiRadioMutex = nullptr;
TaskHandle_t onboardCaptureTaskHandle = nullptr;
SemaphoreHandle_t onboardJobMutex = nullptr;
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
// True while a camera response is being written through an open FAT file.
// No background task may open another SD file during this interval. FatFS is
// nominally re-entrant, but concurrent directory/journal activity has caused
// zero-byte writes on the field card.
volatile bool cameraSdTransferActive = false;
String sdLastMessage = "not_started";
uint32_t sdMountAttempts = 0;
uint32_t sdMountSuccesses = 0;
uint32_t sdLastMountFrequencyHz = 0;
unsigned long sdLastBackgroundRecoveryMs = 0;
uint32_t sdBackgroundRecoveryAttempts = 0;
uint32_t sdBackgroundRecoverySuccesses = 0;
unsigned long sdBackgroundRecoveryIntervalMs = SD_BACKGROUND_RECOVERY_INTERVAL_MS;
uint32_t storageBatteryMv = 0;
unsigned long storageBatteryLastCheckMs = 0;
portMUX_TYPE sdCapacityCacheMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t sdCachedCardType = CARD_NONE;
uint64_t sdCachedCardSizeBytes = 0;
uint64_t sdCachedTotalBytes = 0;
uint64_t sdCachedUsedBytes = 0;
bool onboardCaptureEnabled = ONBOARD_CAPTURE_ENABLED != 0;
unsigned long onboardCaptureIntervalMs = ONBOARD_CAPTURE_INTERVAL_MS;
uint16_t onboardCaptureStartMinute = ONBOARD_CAPTURE_START_MINUTE;
uint16_t onboardCaptureEndMinute = ONBOARD_CAPTURE_END_MINUTE;
int16_t onboardCaptureTzOffsetMin = 0;
bool onboardCameraPoweredDownBySchedule = false;
String onboardCameraPowerState = "boot";
unsigned long onboardCameraLastPowerUpMs = 0;
unsigned long onboardCameraLastPowerDownMs = 0;
uint32_t onboardCameraPowerUpCount = 0;
uint32_t onboardCameraPowerDownCount = 0;
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
uint32_t onboardCameraReinitCount = 0;
uint32_t onboardCaptureRecoveryCount = 0;
String onboardLastCaptureError = "";
String onboardLastRecoveryReason = "";
unsigned long onboardLastCaptureMs = 0;
unsigned long onboardLastScheduleAttemptMs = 0;
unsigned long onboardLastClockInvalidSkipLogMs = 0;
framesize_t onboardFrameSize = FRAMESIZE_UXGA;
int onboardJpegQuality = 5;
int onboardBrightness = -1;
int onboardContrast = 1;
int onboardSaturation = 0;
int onboardSharpness = 3;
bool onboardVflip = true;
bool onboardHmirror = false;
bool onboardAwb = true;
bool onboardAwbGain = true;
int onboardWbMode = 0;
bool onboardAec = true;
bool onboardAec2 = true;
int onboardAeLevel = -2;
int onboardAecValue = 1200;
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

enum OnboardJobType : uint8_t {
  ONBOARD_JOB_NONE = 0,
  ONBOARD_JOB_CAPTURE = 1,
  ONBOARD_JOB_REINIT = 2,
};

struct OnboardJobState {
  OnboardJobType type;
  bool pending;
  bool running;
  bool done;
  bool ok;
  bool oneShotSettings;
  bool settingsRestored;
  char captureKind[16];
  char error[48];
  OnboardCameraSettings settings;
  OnboardMediaInfo media;
};

OnboardJobState onboardJob{};

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
bool scannerScheduleEnabled = SCANNER_SCHEDULE_ENABLED != 0;
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
esp_reset_reason_t currentResetReason = ESP_RST_UNKNOWN;
String currentResetReasonName = "unknown";
String firmwareSketchMd5;
bool previousBootOperational = false;
uint32_t previousBootUptimeMs = 0;
uint32_t previousBootBatteryMv = 0;
int16_t previousBootTemperatureDeciC = 0;
uint32_t previousBootFreeHeap = 0;
bool previousBootHaLowConnected = false;
bool bootMarkedOperational = false;
bool lowBatteryBootHold = false;
uint32_t lowBatteryBootHoldEntryMv = 0;
uint32_t lowBatteryLastMv = 0;
uint8_t lowBatteryStableSampleCount = 0;
unsigned long lowBatteryBootHoldStartedMs = 0;
unsigned long lowBatteryLastCheckMs = 0;
unsigned long lastDiagnosticSdMs = 0;
unsigned long lastDiagnosticNvsMs = 0;
uint32_t diagnosticSdWriteCount = 0;
uint32_t diagnosticSdWriteFailures = 0;
uint32_t diagnosticNvsWriteCount = 0;

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
bool trailMediaUploadActive = false;
volatile bool trailMediaUploadAbortRequested = false;
volatile bool core1TransferWdtGuardActive = false;
TaskHandle_t core1TransferWdtGuardHandle = nullptr;
volatile bool trailMediaRequeueActive = false;
bool transferWifiConnected = false;
uint32_t transferWifiConnectAttempts = 0;
String transferWifiLastError = "";
uint16_t transferWifiSessionFiles = 0;
unsigned long transferWifiSessionStartedMs = 0;
String trailMediaUploadFilename = "";
uint32_t trailMediaUploadBytesSent = 0;
uint32_t trailMediaUploadFileBytes = 0;
uint32_t trailMediaUploadBytesHashed = 0;
uint32_t trailMediaUploadSuccessCount = 0;
uint32_t trailMediaUploadFailureCount = 0;
uint32_t trailMediaUploadSkippedCount = 0;
String trailMediaUploadLastSkipped = "";
unsigned long trailMediaUploadLastAttemptMs = 0;
unsigned long trailMediaUploadLastSuccessMs = 0;
String trailMediaUploadLastError = "";
String trailMediaUploadBlockedReason = "";
String trailMediaUploadPreferredTransport = "halow";
volatile unsigned long trailMediaUploadPauseUntilMs = 0;
bool trailMediaUploadCursorLoaded = false;
uint32_t trailMediaUploadCursorJobId = 0;
uint32_t trailMediaUploadCursorPosition = 0;
uint8_t trailMediaUploadCursorAttempts = 0;

struct TrailMediaUploadCursorDisk {
  uint32_t magic;
  uint32_t jobId;
  uint32_t position;
  uint32_t check;
};

static const uint32_t TRAIL_MEDIA_UPLOAD_CURSOR_MAGIC = 0x55434D31UL;
enum UpstreamTransport : uint8_t {
  UPSTREAM_TRANSPORT_AUTO = 0,
  UPSTREAM_TRANSPORT_HALOW,
  UPSTREAM_TRANSPORT_TRANSFER_WIFI,
};
String failsafeLastReason = "";
uint32_t failsafeRestartCount = 0;
uint32_t failsafeConsecutiveSameReason = 0;
bool automaticRecoverySuspended = false;
uint32_t httpSelfTestSuccessCount = 0;
uint32_t httpSelfTestFailureCount = 0;
uint8_t httpSelfTestConsecutiveFailures = 0;
unsigned long lastClockSyncAttemptMs = 0;
uint32_t observationQueueNextId = 1;
uint32_t observationQueueEnqueueCount = 0;
uint32_t observationQueueDropCount = 0;
uint32_t observationQueueReplaySuccessCount = 0;
uint32_t observationQueueReplayFailureCount = 0;
String observationQueueLastError = "";
size_t observationQueueCachedCount = 0;
size_t observationQueueCachedBytes = 0;
bool otaInProgress = false;
volatile bool httpLongOperationActive = false;
volatile unsigned long httpLongOperationLastProgressMs = 0;
bool otaSucceeded = false;
bool otaRestartPending = false;
unsigned long otaStartedMs = 0;
unsigned long otaFinishedMs = 0;
unsigned long otaRestartAtMs = 0;
size_t otaBytesWritten = 0;
String otaLastError = "";

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

static const size_t STREAM_PACKET_MAX_BYTES = 1600;
static const UBaseType_t STREAM_PACKET_QUEUE_DEPTH = 16;

struct StreamPacket {
  uint32_t sessionId;
  uint16_t length;
  uint8_t streamId;
  uint8_t flags;
  uint8_t payload[STREAM_PACKET_MAX_BYTES];
};

QueueHandle_t streamPacketQueue = nullptr;
TaskHandle_t streamForwardTaskHandle = nullptr;
volatile uint32_t streamQueueHighWater = 0;
volatile uint32_t streamQueueEnqueued = 0;
volatile uint32_t streamQueueDequeued = 0;
volatile uint32_t streamQueueDropped = 0;
volatile uint32_t tunnelWriteLastMs = 0;
volatile uint32_t tunnelWriteMaxMs = 0;
volatile uint32_t tunnelWriteBlockedCount = 0;
int udpPrimaryReceiveBufferBytes = 0;
int udpSecondaryReceiveBufferBytes = 0;

enum ControlAction : uint8_t {
  CONTROL_ACTION_NONE = 0,
  CONTROL_ACTION_BRINGUP = 1,
  CONTROL_ACTION_STREAM_START = 2,
  CONTROL_ACTION_STREAM_STOP = 3,
  CONTROL_ACTION_TRIGGER_PHOTO = 4,
};

enum DurableMediaJobAction : uint8_t {
  DURABLE_MEDIA_ACTION_NONE = 0,
  DURABLE_MEDIA_ACTION_DOWNLOAD_ALL = 1,
  DURABLE_MEDIA_ACTION_DOWNLOAD_AND_DELETE = 2,
};

enum DurableMediaJobStatus : uint8_t {
  DURABLE_MEDIA_IDLE = 0,
  DURABLE_MEDIA_QUEUED = 1,
  DURABLE_MEDIA_RUNNING = 2,
  DURABLE_MEDIA_WAITING_RETRY = 3,
  DURABLE_MEDIA_SUCCEEDED = 4,
  DURABLE_MEDIA_FAILED = 5,
  DURABLE_MEDIA_CANCELLED = 6,
};

struct DurableMediaJobState {
  uint32_t magic;
  uint16_t version;
  uint8_t action;
  uint8_t status;
  uint32_t jobId;
  uint32_t createdBoot;
  uint32_t updatedBoot;
  uint32_t manifestCount;
  uint32_t currentIndex;
  uint32_t downloadedCount;
  uint32_t skippedCount;
  uint32_t deletedCount;
  uint32_t retryCount;
  uint32_t bytesDownloaded;
  uint32_t currentMediaId;
  char stage[32];
  char lastError[96];
  uint32_t checksum;
} __attribute__((packed));

struct DurableMediaRecord {
  uint32_t id;
  uint8_t type;
  uint8_t reserved[3];
  uint32_t expectedBytes;
  char uid[24];
  char date[24];
} __attribute__((packed));

static const uint32_t DURABLE_MEDIA_JOB_MAGIC = 0x47504A42UL;
static const uint16_t DURABLE_MEDIA_JOB_VERSION = 1;
static const unsigned long DURABLE_MEDIA_RETRY_MS = 10000UL;
static const uint8_t DURABLE_MEDIA_ITEM_MAX_ATTEMPTS = 3;
static const uint8_t DURABLE_MEDIA_CAMERA_BRINGUP_MAX_ATTEMPTS = 3;
// After MAX_ATTEMPTS quick retries, retry every 15 minutes; give up only
// after roughly three hours of consecutive bring-up failures.
static const uint8_t DURABLE_MEDIA_CAMERA_BRINGUP_GIVE_UP_ATTEMPTS = 15;
static const unsigned long DURABLE_MEDIA_CAMERA_BACKOFF_MS = 15UL * 60UL * 1000UL;
// Consecutive camera-link drops with no stored progress before the current
// item is charged a normal failure attempt.
static const uint8_t DURABLE_MEDIA_MAX_LINK_FAILURES = 8;
uint8_t durableMediaLinkFailures = 0;
DurableMediaJobState durableMediaJob{};
bool durableMediaJobLoaded = false;
bool durableMediaJobFromMirror = false;
uint32_t durableMirrorLastJobId = 0;
uint32_t durableMirrorLastIndex = 0;
uint8_t durableMirrorLastStatus = DURABLE_MEDIA_IDLE;
unsigned long durableMirrorLastWriteMs = 0;
unsigned long durableMediaNextRetryMs = 0;
SemaphoreHandle_t durableMediaJobMutex = nullptr;
volatile uint32_t durableTransferBytes = 0;
volatile uint32_t durableTransferExpectedBytes = 0;
volatile unsigned long durableTransferStartedMs = 0;
volatile unsigned long durableTransferLastProgressMs = 0;

String buildDurableMediaJobJson();
bool loadDurableMediaJob();

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
  char lastError[64];
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
  "",
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

  int selected = 0;
  const unsigned long connectStartedMs = millis();
  while (millis() - connectStartedMs < timeoutMs) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    timeval slice{};
    slice.tv_sec = 0;
    slice.tv_usec = 250000;
    selected = select(sock + 1, nullptr, &writeSet, nullptr, &slice);
    if (selected > 0) break;
    if (selected < 0 && errno != EINTR) break;
    feedTaskWdt();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
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
  streamForwardingEnabled = false;
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
  unsigned long lastProgressMs = millis();
  while (sent < len) {
    const int written = send(sock, data + sent, len - sent, MSG_DONTWAIT);
    if (written > 0) {
      sent += static_cast<size_t>(written);
      lastProgressMs = millis();
      continue;
    }
    const int sendError = errno;
    if (written < 0 && (sendError == EAGAIN || sendError == EWOULDBLOCK ||
                        sendError == EINTR) &&
        millis() - lastProgressMs < 15000UL) {
      vTaskDelay(pdMS_TO_TICKS(10));
      feedTaskWdt();
      continue;
    }
    return false;
  }
  return true;
}

bool sendMediaChunkWithRetry(int sock, const uint8_t *data, size_t len, String &error) {
  size_t sent = 0;
  unsigned long lastProgressMs = millis();
  while (sent < len) {
    if (trailMediaUploadAbortRequested || durableMediaJobIsActive()) {
      error = "camera_job_preempted";
      return false;
    }
    const int written = send(sock, data + sent, len - sent, 0);
    if (written > 0) {
      sent += static_cast<size_t>(written);
      lastProgressMs = millis();
      feedTaskWdt();
      continue;
    }
    const int sendError = errno;
    if (written < 0 && (sendError == EAGAIN || sendError == EWOULDBLOCK || sendError == EINTR)) {
      if (millis() - lastProgressMs < 90000UL) {
        vTaskDelay(pdMS_TO_TICKS(20));
        feedTaskWdt();
        continue;
      }
      error = "blob_send_no_progress_timeout_" + String(sendError);
      return false;
    }
    error = "blob_send_socket_error_" + String(sendError);
    return false;
  }
  return true;
}

bool bindSocketToTransferWifi(int sock) {
  esp_netif_t *wifiNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (wifiNetif == nullptr) return false;
  char interfaceName[8] = {};
  if (esp_netif_get_netif_impl_name(wifiNetif, interfaceName) != ESP_OK || interfaceName[0] == '\0') {
    return false;
  }
  const int result = setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE,
                                interfaceName, strlen(interfaceName) + 1);
  if (result != 0) {
    Serial.printf("[network] bind WiFi device=%s failed errno=%d\n", interfaceName, errno);
  }
  return result == 0;
}

bool bindSocketToHaLow(int sock) {
  struct netif *interface = HaLow.netif();
  if (interface == nullptr) return false;
  char interfaceName[NETIF_NAMESIZE] = {};
  if (netif_index_to_name(netif_get_index(interface), interfaceName) == nullptr) return false;
  const int result = setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE,
                                interfaceName, strlen(interfaceName) + 1);
  if (result != 0) {
    Serial.printf("[network] bind HaLow device=%s failed errno=%d\n", interfaceName, errno);
  }
  return result == 0;
}

bool bindSocketToLocalAddress(int sock, const IPAddress &localIp) {
  if (localIp == IPAddress((uint32_t)0)) return false;
  sockaddr_in localAddress{};
  localAddress.sin_family = AF_INET;
  localAddress.sin_addr.s_addr = static_cast<uint32_t>(localIp);
  localAddress.sin_port = 0;
  return bind(sock, reinterpret_cast<sockaddr *>(&localAddress), sizeof(localAddress)) == 0;
}

bool bindSocketToTransport(int sock, UpstreamTransport transport) {
  if (transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI) {
    // ESP-IDF's lwIP build for this board rejects SO_BINDTODEVICE with EINVAL.
    // Binding the source address is sufficient to make source-aware routing
    // choose the matching netif, including when WiFi and HaLow share a subnet.
    return transferWifiConnected &&
           bindSocketToLocalAddress(sock, WiFi.localIP());
  }
  if (transport == UPSTREAM_TRANSPORT_HALOW) {
    return halowConnected && HaLow.status() == WL_CONNECTED &&
           bindSocketToLocalAddress(sock, HaLow.localIP());
  }
  return false;
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
    feedTaskWdt();
    delay(1);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void serviceBoardHttp() {
  if (!httpServerStarted) {
    return;
  }
  if (httpServiceMutex != nullptr &&
      xSemaphoreTake(httpServiceMutex, 0) != pdTRUE) {
    return;
  }
  const unsigned long now = millis();
  if (lastHttpServiceMs != 0) {
    httpServiceLastGapMs = now - lastHttpServiceMs;
    if (httpServiceLastGapMs > httpServiceGapMaxMs) {
      httpServiceGapMaxMs = httpServiceLastGapMs;
    }
  }
  server.handleClient();
  lastHttpServiceMs = millis();
  ++httpServiceCount;
  if (httpServiceMutex != nullptr) xSemaphoreGive(httpServiceMutex);
}

void httpServiceTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    serviceBoardHttp();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void startHttpServiceTask() {
  if (httpServiceTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(httpServiceTask,
                          "http-service",
                          8192,
                          nullptr,
                          2,
                          &httpServiceTaskHandle,
                          HTTP_SERVICE_CORE);
}

void restartHttpServer(const char *reason) {
  Serial.printf("[http] restarting server reason=%s count=%u\n",
                reason == nullptr ? "" : reason,
                static_cast<unsigned>(httpServerRestartCount + 1));
  server.close();
  cooperativeDelay(25);
  server.begin();
  lastHttpServerRestartMs = millis();
  ++httpServerRestartCount;
  httpServerLastRestartReason = reason == nullptr ? "" : String(reason);
}

void maybeRestartHttpServerIdle() {
  if (!httpServerStarted) return;
  if (HTTP_SERVER_IDLE_RESTART_MS == 0) return;
  if (otaInProgress || otaRestartPending) return;
  if (isControlActionActive() || streamSessionActive) return;
  if (lastHttpServerRestartMs == 0) {
    lastHttpServerRestartMs = millis();
    return;
  }
  if (millis() - lastHttpServerRestartMs >= HTTP_SERVER_IDLE_RESTART_MS) {
    restartHttpServer("idle_periodic");
  }
}

String buildHttpServiceJson() {
  String payload = "{";
  payload += "\"service_count\":" + String(httpServiceCount);
  payload += ",\"last_service_age_ms\":" + String(msSince(lastHttpServiceMs));
  payload += ",\"last_gap_ms\":" + String(httpServiceLastGapMs);
  payload += ",\"max_gap_ms\":" + String(httpServiceGapMaxMs);
  payload += ",\"server_restart_count\":" + String(httpServerRestartCount);
  payload += ",\"server_last_restart_age_ms\":" + String(msSince(lastHttpServerRestartMs));
  payload += ",\"server_last_restart_reason\":\"" + jsonEscape(httpServerLastRestartReason) + "\"";
  payload += ",\"server_idle_restart_ms\":" + String(HTTP_SERVER_IDLE_RESTART_MS);
  payload += ",\"camera_http_keepalive_enabled\":" + String((CAMERA_HTTP_KEEPALIVE_ENABLED != 0) ? "true" : "false");
  payload += ",\"camera_auto_recovery_enabled\":" + String((CAMERA_AUTO_RECOVERY_ENABLED != 0) ? "true" : "false");
  payload += "}";
  return payload;
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
  if (streamPacketQueue != nullptr) xQueueReset(streamPacketQueue);
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

  timeval sendTimeout{};
  sendTimeout.tv_sec = 1;
  sendTimeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

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
  streamForwardingEnabled = true;
  return true;
}

void closeRtspSession() {
  // Explicitly tear down an active RTSP session before closing the TCP
  // connection.  Some trail-camera firmware keeps the previous RTP session
  // alive when the client simply drops TCP, causing subsequent PLAY requests
  // to return 200 but emit no UDP media.
  if (cameraRtspClient.connected() &&
      rtspSessionOpen &&
      !rtspSessionHeader.isEmpty() &&
      !rtspPlayUrl.isEmpty()) {
    String teardownResponse;
    int teardownStatus = 0;
    String teardownHeaders = "Session: " + rtspSessionHeader + "\r\n";
    exchangeCameraRtspRequest(cameraRtspClient,
                              "TEARDOWN",
                              rtspPlayUrl,
                              teardownHeaders,
                              teardownResponse,
                              teardownStatus);
    Serial.printf("[rtsp-live] TEARDOWN status=%d\n", teardownStatus);
  }
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
  streamForwardingEnabled = false;
  streamSessionActive = false;
  const int sock = getTunnelSocketSnapshot();
  if (sock >= 0) {
    String metadata = "{\"type\":\"stop\",\"reason\":\"";
    metadata += reason;
    metadata += "\"}";
    sendTunnelControlFrame(sock, CONTROL_TYPE_STOP, metadata);
  }
  closeTunnelSocket();
  if (streamPacketQueue != nullptr) xQueueReset(streamPacketQueue);
  closeRtspSession();
  lastStreamStopMs = millis();
  streamSessionStartedMs = 0;
}

void serviceStreamHealth() {
  if (!streamSessionActive) return;
  const unsigned long gapMs = lastPrimaryPacketMs == 0
                                ? msSince(streamSessionStartedMs)
                                : millis() - lastPrimaryPacketMs;
  if (gapMs <= STREAM_STALL_FAIL_MS) return;
#if CAMERA_AUTO_RECOVERY_ENABLED
  // The main loop's recovery path owns stalled-session restart when enabled.
  // Leave the session state intact long enough for recoverActiveStream() to
  // tear down and establish a fresh RTSP/tunnel session.
  return;
#endif
  ++streamStallCount;
  lastStreamStallMs = millis();
  lastStreamStartStage = "rtp_stalled";
  lastStreamStartMessage = "stream_rtp_stalled";
  Serial.printf("[stream] RTP stalled gap_ms=%lu count=%u; stopping stale session\n",
                gapMs,
                static_cast<unsigned>(streamStallCount));
  stopTunnelSession("rtp_stalled");
  finishControlAction(CONTROL_ACTION_STREAM_START, false, "stream_rtp_stalled");
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
  const unsigned long writeStartedMs = millis();
  if (!sendAll(sock, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) ||
      !sendAll(sock, payload, payloadLen)) {
    const uint32_t writeMs = millis() - writeStartedMs;
    tunnelWriteLastMs = writeMs;
    if (writeMs > tunnelWriteMaxMs) tunnelWriteMaxMs = writeMs;
    if (writeMs >= 20) ++tunnelWriteBlockedCount;
    unlockTunnelWrite();
    ++tunnelSendFailures;
    Serial.println("[tunnel] media send failed, closing socket");
    closeTunnelSocket();
    return false;
  }
  unlockTunnelWrite();

  const uint32_t writeMs = millis() - writeStartedMs;
  tunnelWriteLastMs = writeMs;
  if (writeMs > tunnelWriteMaxMs) tunnelWriteMaxMs = writeMs;
  if (writeMs >= 20) ++tunnelWriteBlockedCount;

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
  const unsigned long primaryAgeMs = msSince(lastPrimaryPacketMs);
  const uint32_t lossDenominator = rtpPacketsReceived + rtpPacketsMissing;
  const uint32_t lossPct = lossDenominator == 0 ? 100 :
                           static_cast<uint32_t>((static_cast<uint64_t>(rtpPacketsMissing) * 100ULL) / lossDenominator);
  const bool primaryReceiving = streamSessionActive && lastPrimaryPacketMs > 0 &&
                                primaryAgeMs < 5000UL && rtpPacketsReceived > 0;
  const bool primaryQualityOk = primaryReceiving && lossPct <= RTP_MAX_HEALTH_LOSS_PCT;
  const bool primaryFlowing = primaryQualityOk && rtpPacketsForwarded > 0;
  const bool primaryStalled = streamSessionActive &&
                              ((lastPrimaryPacketMs > 0 && primaryAgeMs > STREAM_STALL_FAIL_MS) ||
                               (lastPrimaryPacketMs == 0 && msSince(streamSessionStartedMs) > STREAM_STALL_FAIL_MS));
  const UBaseType_t queueDepth = streamPacketQueue == nullptr ? 0 : uxQueueMessagesWaiting(streamPacketQueue);
  String payload = "{";
  payload += "\"last_stage\":\"" + jsonEscape(lastStreamStartStage) + "\"";
  payload += ",\"last_message\":\"" + jsonEscape(lastStreamStartMessage) + "\"";
  payload += ",\"rtp_flowing\":" + String(primaryFlowing ? "true" : "false");
  payload += ",\"rtp_receiving\":" + String(primaryReceiving ? "true" : "false");
  payload += ",\"rtp_quality_ok\":" + String(primaryQualityOk ? "true" : "false");
  payload += ",\"rtp_quality_limit_pct\":" + String(RTP_MAX_HEALTH_LOSS_PCT);
  payload += ",\"rtp_stalled\":" + String(primaryStalled ? "true" : "false");
  payload += ",\"stream_session_id\":" + String(streamSessionId);
  payload += ",\"stall_fail_ms\":" + String(STREAM_STALL_FAIL_MS);
  payload += ",\"stall_count\":" + String(streamStallCount);
  payload += ",\"last_stall_age_ms\":" + String(msSince(lastStreamStallMs));
  payload += ",\"last_elapsed_ms\":" + String(lastStreamStartElapsedMs);
  payload += ",\"last_started_age_ms\":" + String(msSince(lastStreamStartMs));
  payload += ",\"describe_status\":" + String(lastStreamDescribeStatus);
  payload += ",\"setup_status\":" + String(lastStreamSetupStatus);
  payload += ",\"play_status\":" + String(lastStreamPlayStatus);
  payload += ",\"play_url\":\"" + jsonEscape(lastStreamPlayUrl) + "\"";
  payload += ",\"rtsp_keepalive_count\":" + String(rtspKeepaliveCount);
  payload += ",\"rtsp_keepalive_failures\":" + String(rtspKeepaliveFailures);
  payload += ",\"last_rtsp_keepalive_status\":" + String(lastRtspKeepaliveStatus);
  payload += ",\"last_rtsp_keepalive_method\":\"" + jsonEscape(lastRtspKeepaliveMethod) + "\"";
  payload += ",\"rtp_packets_received\":" + String(rtpPacketsReceived);
  payload += ",\"rtp_packets_forwarded\":" + String(rtpPacketsForwarded);
  payload += ",\"rtp_sequence_gaps\":" + String(rtpSequenceGaps);
  payload += ",\"rtp_packets_missing\":" + String(rtpPacketsMissing);
  payload += ",\"rtp_out_of_order\":" + String(rtpOutOfOrder);
  payload += ",\"rtp_forward_failures\":" + String(rtpForwardFailures);
  payload += ",\"udp_receive_overruns\":" + String(udpReceiveOverruns);
  payload += ",\"rtp_packets_received_total\":" + String(rtpPacketsReceivedTotal);
  payload += ",\"rtp_packets_forwarded_total\":" + String(rtpPacketsForwardedTotal);
  payload += ",\"rtp_packets_missing_total\":" + String(rtpPacketsMissingTotal);
  payload += ",\"rtp_forward_failures_total\":" + String(rtpForwardFailuresTotal);
  payload += ",\"udp_receive_overruns_total\":" + String(udpReceiveOverrunsTotal);
  payload += ",\"last_rtp_sequence\":" + String(lastRtpSequence);
  payload += ",\"rtp_loss_pct\":" + String(lossPct);
  payload += ",\"rtp_keyframe_seen\":" + String(rtpKeyframeSeen ? "true" : "false");
  payload += ",\"rtp_keyframes_observed\":" + String(rtpKeyframesObserved);
  payload += ",\"tunnel_target\":\"" + jsonEscape(String(UPSTREAM_TUNNEL_HOST) + ":" + String(UPSTREAM_TUNNEL_PORT)) + "\"";
  payload += ",\"tunnel_connect_error\":" + String(lastTunnelConnectError);
  payload += ",\"tunnel_packets_sent\":" + String(tunnelPacketsSent);
  payload += ",\"tunnel_bytes_sent\":" + String(tunnelBytesSent);
  payload += ",\"tunnel_send_failures\":" + String(tunnelSendFailures);
  payload += ",\"forward_queue\":{";
  payload += "\"capacity\":" + String(STREAM_PACKET_QUEUE_DEPTH);
  payload += ",\"depth\":" + String(queueDepth);
  payload += ",\"high_water\":" + String(streamQueueHighWater);
  payload += ",\"enqueued\":" + String(streamQueueEnqueued);
  payload += ",\"dequeued\":" + String(streamQueueDequeued);
  payload += ",\"dropped\":" + String(streamQueueDropped);
  payload += ",\"tunnel_write_last_ms\":" + String(tunnelWriteLastMs);
  payload += ",\"tunnel_write_max_ms\":" + String(tunnelWriteMaxMs);
  payload += ",\"tunnel_write_blocked_count\":" + String(tunnelWriteBlockedCount);
  payload += "}";
  payload += ",\"recovery\":{";
  payload += "\"reason\":\"" + jsonEscape(lastStreamRecoveryReason) + "\"";
  payload += ",\"succeeded\":" + String(lastStreamRecoverySucceeded ? "true" : "false");
  payload += ",\"started_age_ms\":" + String(msSince(lastStreamRecoveryStartedMs));
  payload += ",\"finished_age_ms\":" + String(msSince(lastStreamRecoveryFinishedMs));
  payload += ",\"duration_ms\":" + String(lastStreamRecoveryDurationMs);
  payload += "}";
  payload += ",\"udp_primary\":{";
  payload += "\"ready\":" + String(udpPrimaryReady ? "true" : "false");
  payload += ",\"port\":" + String(udpPrimaryStats.localPort);
  payload += ",\"packets\":" + String(udpPrimaryStats.packets);
  payload += ",\"bytes\":" + String(udpPrimaryStats.bytes);
  payload += ",\"last_source\":\"" + udpPrimaryStats.lastSourceIp.toString() + ":" + String(udpPrimaryStats.lastSourcePort) + "\"";
  payload += ",\"last_packet_len\":" + String(static_cast<unsigned>(udpPrimaryStats.lastPacketLen));
  payload += ",\"last_packet_age_ms\":" + String(primaryAgeMs);
  payload += ",\"receive_buffer_bytes\":" + String(udpPrimaryReceiveBufferBytes);
  payload += "}";
  payload += ",\"udp_secondary\":{";
  payload += "\"ready\":" + String(udpSecondaryReady ? "true" : "false");
  payload += ",\"port\":" + String(udpSecondaryStats.localPort);
  payload += ",\"packets\":" + String(udpSecondaryStats.packets);
  payload += ",\"bytes\":" + String(udpSecondaryStats.bytes);
  payload += ",\"last_source\":\"" + udpSecondaryStats.lastSourceIp.toString() + ":" + String(udpSecondaryStats.lastSourcePort) + "\"";
  payload += ",\"last_packet_len\":" + String(static_cast<unsigned>(udpSecondaryStats.lastPacketLen));
  payload += ",\"last_packet_age_ms\":" + String(msSince(lastSecondaryPacketMs));
  payload += ",\"receive_buffer_bytes\":" + String(udpSecondaryReceiveBufferBytes);
  payload += "}";
  payload += "}";
  return payload;
}

uint32_t readBatteryAdcMilliVolts(int samples = 64) {
  digitalWrite(BAT_ADC_CTRL_PIN, BAT_ADC_CTRL_ACTIVE_LEVEL);
  delay(20);
  uint64_t total = 0;
  for (int i = 0; i < samples; ++i) {
    total += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  digitalWrite(BAT_ADC_CTRL_PIN, BAT_ADC_CTRL_IDLE_LEVEL);
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
  payload += ",\"adc_ctrl_active_level\":" + String(BAT_ADC_CTRL_ACTIVE_LEVEL);
  payload += ",\"adc_ctrl_idle_level\":" + String(BAT_ADC_CTRL_IDLE_LEVEL);
  payload += ",\"adc_ctrl_idle_after_read\":" + String(digitalRead(BAT_ADC_CTRL_PIN));
  payload += ",\"adc_mv\":" + String(adcMv);
  payload += ",\"battery_est_v\":" + String(batteryV, 3);
  payload += ",\"charging_gpio15\":" + String(digitalRead(BAT_CHRG_PIN));
  payload += ",\"done_gpio16\":" + String(digitalRead(BAT_DONE_PIN));
  payload += "}";
  return payload;
}

uint32_t readBatteryMilliVolts(int samples = 16) {
  return readBatteryAdcMilliVolts(samples) * 2;
}

bool previousBootSuggestsLowVoltageWedge() {
  return !previousBootOperational &&
         previousBootUptimeMs > 0 &&
         previousBootUptimeMs < 60000UL &&
         previousBootBatteryMv > 0 &&
         previousBootBatteryMv < BATTERY_BOOT_MIN_MV;
}

bool batteryVoltageStableForBoot(uint32_t batteryMv) {
  if (batteryMv >= BATTERY_BOOT_RECOVER_MV) {
    ++lowBatteryStableSampleCount;
  } else {
    lowBatteryStableSampleCount = 0;
  }
  return lowBatteryStableSampleCount >= BATTERY_BOOT_STABLE_SAMPLES;
}

String buildLowBatteryBootHoldJson() {
  String payload = "{";
  payload += "\"active\":" + String(lowBatteryBootHold ? "true" : "false");
  payload += ",\"entry_battery_mv\":" + String(lowBatteryBootHoldEntryMv);
  payload += ",\"last_battery_mv\":" + String(lowBatteryLastMv);
  payload += ",\"boot_min_mv\":" + String(BATTERY_BOOT_MIN_MV);
  payload += ",\"boot_recover_mv\":" + String(BATTERY_BOOT_RECOVER_MV);
  payload += ",\"stable_samples\":" + String(lowBatteryStableSampleCount);
  payload += ",\"required_stable_samples\":" + String(BATTERY_BOOT_STABLE_SAMPLES);
  payload += ",\"hold_age_ms\":" + String(msSince(lowBatteryBootHoldStartedMs));
  payload += ",\"previous_boot_low_voltage_wedge\":" + String(previousBootSuggestsLowVoltageWedge() ? "true" : "false");
  payload += "}";
  return payload;
}

bool batteryBootHoldRequired(uint32_t batteryMv) {
  if (batteryMv < BATTERY_BOOT_MIN_MV) {
    return true;
  }
  if (previousBootSuggestsLowVoltageWedge() && batteryMv < BATTERY_BOOT_RECOVER_MV) {
    return true;
  }
  return false;
}

void enterLowBatteryBootHold(uint32_t entryBatteryMv) {
  lowBatteryBootHold = true;
  lowBatteryBootHoldEntryMv = entryBatteryMv;
  lowBatteryLastMv = entryBatteryMv;
  lowBatteryStableSampleCount = 0;
  lowBatteryBootHoldStartedMs = millis();
  lowBatteryLastCheckMs = 0;
  Serial.printf("[power] low battery boot hold entry_mv=%u min_mv=%u recover_mv=%u previous_low_voltage_wedge=%s\n",
                entryBatteryMv,
                BATTERY_BOOT_MIN_MV,
                BATTERY_BOOT_RECOVER_MV,
                previousBootSuggestsLowVoltageWedge() ? "yes" : "no");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  closeBleWakeSession();
}

void serviceLowBatteryBootHold() {
  if (!lowBatteryBootHold) {
    return;
  }
  if (lowBatteryLastCheckMs != 0 &&
      millis() - lowBatteryLastCheckMs < BATTERY_LOW_POWER_RECHECK_MS) {
    delay(250);
    return;
  }
  lowBatteryLastCheckMs = millis();
  lowBatteryLastMv = readBatteryMilliVolts(16);
  const bool stable = batteryVoltageStableForBoot(lowBatteryLastMv);
  Serial.printf("[power] low battery hold mv=%u stable=%u/%u age_ms=%lu\n",
                lowBatteryLastMv,
                lowBatteryStableSampleCount,
                BATTERY_BOOT_STABLE_SAMPLES,
                msSince(lowBatteryBootHoldStartedMs));
  if (stable) {
    Serial.println("[power] battery recovered; restarting for clean boot");
    delay(100);
    ESP.restart();
  }
  if (BATTERY_LOW_POWER_RESTART_MS > 0 &&
      msSince(lowBatteryBootHoldStartedMs) >= BATTERY_LOW_POWER_RESTART_MS) {
    Serial.println("[power] low battery hold timed restart");
    delay(100);
    ESP.restart();
  }
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
  payload += ",\"reconnect_in_progress\":" + String(halowConnectInProgress ? "true" : "false");
  payload += ",\"recovery_attempts\":" + String(halowRecoveryAttempts);
  payload += ",\"recovery_successes\":" + String(halowRecoverySuccesses);
  payload += ",\"last_recovery_attempt_age_ms\":" + String(msSince(halowLastRecoveryAttemptMs));
  payload += ",\"liveness_probe_successes\":" + String(halowLivenessProbeSuccesses);
  payload += ",\"liveness_probe_failures\":" + String(halowLivenessProbeFailures);
  payload += ",\"consecutive_liveness_failures\":" + String(halowConsecutiveLivenessFailures);
  payload += ",\"last_liveness_error\":" + String(halowLastLivenessError);
  payload += ",\"last_liveness_probe_age_ms\":" + String(msSince(halowLastLivenessProbeMs));
  payload += ",\"forced_reconnects\":" + String(halowForcedReconnects);
  payload += ",\"gratuitous_arp_count\":" + String(halowGratuitousArpCount);
  payload += ",\"gratuitous_arp_failures\":" + String(halowGratuitousArpFailures);
  payload += ",\"last_gratuitous_arp_age_ms\":" + String(msSince(halowLastGratuitousArpMs));
  payload += ",\"snr_db\":null";
  payload += ",\"noise_dbm\":null";
  payload += ",\"snr_note\":\"not_exposed_by_current_halow_wrapper\"";
  payload += ",\"rate_control\":null";
  payload += ",\"rate_control_note\":\"not_exposed_by_current_halow_wrapper\"";
  payload += "}";
  return payload;
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void initBootIdentity() {
  currentResetReason = esp_reset_reason();
  currentResetReasonName = resetReasonName(currentResetReason);
  firmwareSketchMd5 = ESP.getSketchMD5();
  if (!runtimePrefs.begin("runtime", false)) {
    Serial.println("[boot] failed to open runtime preferences");
    persistentBootCount = 0;
    bootSessionId = millis();
    return;
  }
  previousBootOperational = runtimePrefs.getBool("boot_ok", false);
  previousBootUptimeMs = runtimePrefs.getUInt("last_up_ms", 0);
  previousBootBatteryMv = runtimePrefs.getUInt("last_bat_mv", 0);
  previousBootTemperatureDeciC = runtimePrefs.getShort("last_temp", 0);
  previousBootFreeHeap = runtimePrefs.getUInt("last_heap", 0);
  previousBootHaLowConnected = runtimePrefs.getBool("last_halow", false);
  failsafeLastReason = runtimePrefs.getString("failsafe", "");
  failsafeRestartCount = runtimePrefs.getUInt("failsafe_n", 0);
  failsafeConsecutiveSameReason = runtimePrefs.getUInt("failsafe_same", 0);
  automaticRecoverySuspended = failsafeConsecutiveSameReason >= 3;
  persistentBootCount = runtimePrefs.getUInt("boot_count", 0) + 1;
  runtimePrefs.putUInt("boot_count", persistentBootCount);
  runtimePrefs.putBool("boot_ok", false);
  runtimePrefs.end();
  bootSessionId = persistentBootCount;
  Serial.printf("[boot] boot_count=%u boot_session_id=%u reset=%s(%d) previous_operational=%s previous_uptime_ms=%u firmware_md5=%s\n",
                persistentBootCount,
                bootSessionId,
                currentResetReasonName.c_str(),
                static_cast<int>(currentResetReason),
                previousBootOperational ? "yes" : "no",
                previousBootUptimeMs,
                firmwareSketchMd5.c_str());
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

int onboardActualLocalMinuteOfDay() {
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

int onboardLocalMinuteOfDay() {
  const int actualMinute = onboardActualLocalMinuteOfDay();
  if (actualMinute >= 0) {
    return actualMinute;
  }
  return static_cast<int>((millis() / 60000UL) % 1440UL);
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

bool parseOnboardCameraSettingsFromServer(OnboardCameraSettings &settings, String &error) {
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
    settings.frameSize = requested;
  }
  if (server.hasArg("jpeg_quality") || server.hasArg("quality")) {
    const int requested = server.hasArg("jpeg_quality") ? server.arg("jpeg_quality").toInt() : server.arg("quality").toInt();
    if (requested < 4 || requested > 63) {
      error = "invalid_jpeg_quality";
      return false;
    }
    settings.jpegQuality = requested;
  }
  if (server.hasArg("brightness")) settings.brightness = server.arg("brightness").toInt();
  if (server.hasArg("contrast")) settings.contrast = server.arg("contrast").toInt();
  if (server.hasArg("saturation")) settings.saturation = server.arg("saturation").toInt();
  if (server.hasArg("sharpness")) settings.sharpness = server.arg("sharpness").toInt();
  if (server.hasArg("vflip")) settings.vflip = boolLikeValue(server.arg("vflip"));
  if (server.hasArg("hmirror")) settings.hmirror = boolLikeValue(server.arg("hmirror"));
  if (server.hasArg("awb")) settings.awb = boolLikeValue(server.arg("awb"));
  if (server.hasArg("awb_gain")) settings.awbGain = boolLikeValue(server.arg("awb_gain"));
  if (server.hasArg("wb_mode")) settings.wbMode = server.arg("wb_mode").toInt();
  if (server.hasArg("aec")) settings.aec = boolLikeValue(server.arg("aec"));
  if (server.hasArg("aec2")) settings.aec2 = boolLikeValue(server.arg("aec2"));
  if (server.hasArg("ae_level")) settings.aeLevel = server.arg("ae_level").toInt();
  if (server.hasArg("aec_value")) settings.aecValue = server.arg("aec_value").toInt();
  if (server.hasArg("agc")) settings.agc = boolLikeValue(server.arg("agc"));
  if (server.hasArg("agc_gain")) settings.agcGain = server.arg("agc_gain").toInt();
  if (server.hasArg("special_effect")) settings.specialEffect = server.arg("special_effect").toInt();
  error = "";
  return true;
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
    onboardJpegQuality = psramFound() ? 5 : 10;
    clampOnboardConfig();
    return;
  }
  onboardCaptureEnabled = runtimePrefs.getBool("enabled", onboardCaptureEnabled);
  onboardCaptureIntervalMs = runtimePrefs.getULong("interval", onboardCaptureIntervalMs);
  onboardCaptureStartMinute = runtimePrefs.getUShort("start", onboardCaptureStartMinute);
  onboardCaptureEndMinute = runtimePrefs.getUShort("end", onboardCaptureEndMinute);
  onboardCaptureTzOffsetMin = runtimePrefs.getShort("tz", onboardCaptureTzOffsetMin);
  onboardFrameSize = static_cast<framesize_t>(runtimePrefs.getUChar("framesize", psramFound() ? FRAMESIZE_UXGA : FRAMESIZE_SVGA));
  onboardJpegQuality = runtimePrefs.getUChar("quality", psramFound() ? 5 : 10);
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

void loadScannerConfig() {
  if (!runtimePrefs.begin("scanner", true)) {
    Serial.println("[scanner-config] failed to open preferences for read");
    return;
  }
  scannerScheduleEnabled = runtimePrefs.getBool("enabled", scannerScheduleEnabled);
  runtimePrefs.end();
  Serial.printf("[scanner-config] scheduled_scans=%s upload_host=%s:%u\n",
                scannerScheduleEnabled ? "enabled" : "disabled",
                AIR_SCAN_API_HOST,
                AIR_SCAN_API_PORT);
}

void saveScannerConfig() {
  if (!runtimePrefs.begin("scanner", false)) {
    Serial.println("[scanner-config] failed to open preferences for write");
    return;
  }
  runtimePrefs.putBool("enabled", scannerScheduleEnabled);
  runtimePrefs.end();
}

bool initOnboardCamera() {
  if (onboardCameraReady) {
    sensor_t *existing = esp_camera_sensor_get();
    if (existing != nullptr) {
      onboardCameraPowerState = "ready";
      return true;
    }
    Serial.println("[onboard-camera] ready flag set but sensor unavailable; forcing reinit");
    onboardCameraReady = false;
    onboardCameraPowerState = "sensor_unavailable";
  }

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
    onboardCameraPowerState = "init_failed";
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
  onboardCameraPoweredDownBySchedule = false;
  onboardCameraPowerState = "ready";
  onboardCameraLastPowerUpMs = millis();
  ++onboardCameraPowerUpCount;
  Serial.printf("[onboard-camera] ready psram=%s interval_ms=%lu enabled=%s\n",
                psramFound() ? "yes" : "no",
                onboardCaptureIntervalMs,
                onboardCaptureEnabled ? "yes" : "no");
  return true;
}

void shutdownOnboardCamera(const char *reason) {
  if (!onboardCameraReady) {
    onboardCameraPowerState = reason != nullptr ? reason : "off";
    return;
  }
  esp_camera_deinit();
  onboardCameraReady = false;
  onboardCameraPowerState = reason != nullptr ? reason : "off";
  onboardCameraLastPowerDownMs = millis();
  ++onboardCameraPowerDownCount;
  Serial.printf("[onboard-camera] powered down reason=%s\n", onboardCameraPowerState.c_str());
}

bool reinitOnboardCamera(const char *reason) {
  if (onboardCameraMutex != nullptr &&
      xSemaphoreTake(onboardCameraMutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
    onboardLastCaptureError = "onboard_camera_busy";
    onboardLastRecoveryReason = reason != nullptr ? reason : "reinit_busy";
    return false;
  }
  onboardLastRecoveryReason = reason != nullptr ? reason : "reinit";
  Serial.printf("[onboard-camera] reinit reason=%s\n", onboardLastRecoveryReason.c_str());
  esp_camera_deinit();
  onboardCameraReady = false;
  onboardCameraPowerState = "reinitializing";
  cooperativeDelay(1000);
  const bool ok = initOnboardCamera();
  ++onboardCameraReinitCount;
  if (!ok) {
    onboardLastCaptureError = "onboard_camera_not_ready";
  }
  if (onboardCameraMutex != nullptr) xSemaphoreGive(onboardCameraMutex);
  return ok;
}

bool ensureOnboardCameraReady(String *error = nullptr) {
  if (onboardCameraReady) {
    if (esp_camera_sensor_get() != nullptr) {
      return true;
    }
    if (!reinitOnboardCamera("sensor_unavailable")) {
      if (error != nullptr) *error = "onboard_camera_not_ready";
      return false;
    }
    return true;
  }
  if (!initOnboardCamera()) {
    if (error != nullptr) *error = "onboard_camera_not_ready";
    return false;
  }
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
  if (!sdReady) return LittleFS.totalBytes();
  uint64_t totalBytes = 0;
  portENTER_CRITICAL(&sdCapacityCacheMux);
  totalBytes = sdCachedTotalBytes;
  portEXIT_CRITICAL(&sdCapacityCacheMux);
  return totalBytes;
}

uint64_t persistentStorageUsedBytes() {
  if (!sdReady) return LittleFS.usedBytes();
  uint64_t usedBytes = 0;
  portENTER_CRITICAL(&sdCapacityCacheMux);
  usedBytes = sdCachedUsedBytes;
  portEXIT_CRITICAL(&sdCapacityCacheMux);
  return usedBytes;
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

void refreshSdCapacityCache() {
  if (!sdReady || cameraSdTransferActive) return;
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;
  const uint8_t cardType = static_cast<uint8_t>(SD.cardType());
  const uint64_t cardSize = SD.cardSize();
  const uint64_t totalBytes = SD.totalBytes();
  const uint64_t usedBytes = SD.usedBytes();
  portENTER_CRITICAL(&sdCapacityCacheMux);
  sdCachedCardType = cardType;
  sdCachedCardSizeBytes = cardSize;
  sdCachedTotalBytes = totalBytes;
  sdCachedUsedBytes = usedBytes;
  portEXIT_CRITICAL(&sdCapacityCacheMux);
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
}

bool mountSdCard(uint32_t frequency = 4000000UL) {
  ++sdMountAttempts;
  sdLastMountFrequencyHz = frequency;
  SD.end();
  sdSpi.end();
  delay(50);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SD_MOSI_PIN, OUTPUT);
  digitalWrite(SD_MOSI_PIN, HIGH);
  pinMode(SD_MISO_PIN, INPUT_PULLUP);
  sdSpi.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  // A processor reset can leave an SPI-mode SD card in the middle of a
  // command or data response. With CS high, provide well over the required
  // 74 startup clocks at the initialization rate so the card returns to an
  // idle command boundary before SD.begin() sends CMD0.
  sdSpi.beginTransaction(SPISettings(400000UL, MSBFIRST, SPI_MODE0));
  digitalWrite(SD_CS_PIN, HIGH);
  for (uint8_t i = 0; i < 16; ++i) sdSpi.transfer(0xFF);
  sdSpi.endTransaction();
  delay(10);
  if (!SD.begin(SD_CS_PIN, sdSpi, frequency, "/sd", 5, false)) {
    SD.end();
    sdSpi.end();
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
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
  refreshSdCapacityCache();
  Serial.printf("[sd] mounted type=%u size_mb=%llu total_mb=%llu used_mb=%llu\n",
                static_cast<unsigned>(SD.cardType()),
                SD.cardSize() / (1024ULL * 1024ULL),
                SD.totalBytes() / (1024ULL * 1024ULL),
                SD.usedBytes() / (1024ULL * 1024ULL));
  return true;
}

bool mountSdCardWithRetryLocked();

bool mountSdCardWithRetry() {
  if (sdMountMutex != nullptr) xSemaphoreTakeRecursive(sdMountMutex, portMAX_DELAY);
  const bool mounted = mountSdCardWithRetryLocked();
  if (sdMountMutex != nullptr) xSemaphoreGiveRecursive(sdMountMutex);
  return mounted;
}

bool mountSdCardWithRetryLocked() {
  // This bridge is network-bound, so 1 MHz provides ample throughput while
  // leaving substantially more signal margin for long writes and field wiring.
  static const uint32_t frequencies[] = {1000000UL, 1000000UL, 400000UL, 400000UL, 400000UL};
  static const uint16_t settleDelaysMs[] = {250, 500, 1000, 1500, 2000};
  for (size_t attempt = 0; attempt < sizeof(frequencies) / sizeof(frequencies[0]); ++attempt) {
    delay(settleDelaysMs[attempt]);
    Serial.printf("[sd] boot/recovery mount attempt=%u/%u settle_ms=%u freq=%lu\n",
                  static_cast<unsigned>(attempt + 1),
                  static_cast<unsigned>(sizeof(frequencies) / sizeof(frequencies[0])),
                  static_cast<unsigned>(settleDelaysMs[attempt]),
                  static_cast<unsigned long>(frequencies[attempt]));
    if (mountSdCard(frequencies[attempt])) return true;
  }
  Serial.println("[sd] all boot/recovery mount attempts failed");
  return false;
}

void serviceSdRecoveryLocked();

uint32_t cachedStorageBatteryMv() {
  const unsigned long now = millis();
  if (storageBatteryMv == 0 || storageBatteryLastCheckMs == 0 ||
      now - storageBatteryLastCheckMs >= STORAGE_BATTERY_CHECK_INTERVAL_MS) {
    storageBatteryMv = readBatteryMilliVolts(8);
    storageBatteryLastCheckMs = now;
  }
  return storageBatteryMv;
}

void serviceSdRecovery() {
  // A durable job may be available from the NVS mirror while the SD card is
  // still absent. That is exactly when recovery must continue so the manifest
  // and full state can be reloaded from SD.
  if (sdReady || otaInProgress || otaRestartPending) return;
  const uint32_t batteryMv = cachedStorageBatteryMv();
  if (batteryMv < SD_RECOVERY_MIN_BATTERY_MV) {
    sdLastMessage = "recovery_waiting_for_battery";
    return;
  }
  if (sdLastBackgroundRecoveryMs != 0 &&
      millis() - sdLastBackgroundRecoveryMs < sdBackgroundRecoveryIntervalMs) return;
  // Another task (normally the durable job worker) is already remounting.
  if (sdMountMutex != nullptr && xSemaphoreTakeRecursive(sdMountMutex, 0) != pdTRUE) return;
  if (!sdReady) serviceSdRecoveryLocked();
  if (sdMountMutex != nullptr) xSemaphoreGiveRecursive(sdMountMutex);
}

void serviceSdRecoveryLocked() {
  // Match mountSdCardWithRetry(): the field card has needed 400 kHz, and the
  // old 4 MHz-first rotation could take over half an hour to reach it at the
  // maximum backoff interval.
  static const uint32_t frequencies[] = {1000000UL, 400000UL};
  const uint32_t frequency = frequencies[sdBackgroundRecoveryAttempts %
                                         (sizeof(frequencies) / sizeof(frequencies[0]))];
  sdLastBackgroundRecoveryMs = millis();
  ++sdBackgroundRecoveryAttempts;
  Serial.printf("[sd] background recovery attempt=%u freq=%lu uptime_ms=%lu\n",
                sdBackgroundRecoveryAttempts,
                static_cast<unsigned long>(frequency),
                millis());
  if (!mountSdCard(frequency)) {
    sdBackgroundRecoveryIntervalMs = std::min(
        sdBackgroundRecoveryIntervalMs * 2UL, SD_BACKGROUND_RECOVERY_MAX_INTERVAL_MS);
    return;
  }
  if (!loadDurableMediaJob()) {
    sdLastMessage = "durable_job_load_failed";
    sdBackgroundRecoveryIntervalMs = std::min(
        sdBackgroundRecoveryIntervalMs * 2UL, SD_BACKGROUND_RECOVERY_MAX_INTERVAL_MS);
    return;
  }
  sdBackgroundRecoveryIntervalMs = SD_BACKGROUND_RECOVERY_INTERVAL_MS;
  ++sdBackgroundRecoverySuccesses;
  Serial.printf("[sd] background recovery succeeded count=%u durable_job=%s\n",
                sdBackgroundRecoverySuccesses,
                durableMediaJobIsActive() ? "active" : "idle");
}

String buildDiagnosticStatusJson() {
  const uint32_t adcMv = readBatteryAdcMilliVolts(16);
  const uint32_t batteryMv = adcMv * 2;
  const int16_t temperatureDeciC = static_cast<int16_t>(readChipTemperatureC() * 10.0f);
  String payload = "{";
  payload += "\"boot_count\":" + String(persistentBootCount);
  payload += ",\"boot_session_id\":" + String(bootSessionId);
  payload += ",\"uptime_ms\":" + String(millis());
  payload += ",\"reset_reason_code\":" + String(static_cast<int>(currentResetReason));
  payload += ",\"reset_reason\":\"" + jsonEscape(currentResetReasonName) + "\"";
  payload += ",\"boot_operational\":" + String(bootMarkedOperational ? "true" : "false");
  payload += ",\"firmware_version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"";
  payload += ",\"firmware_build\":\"" + jsonEscape(FIRMWARE_BUILD) + "\"";
  payload += ",\"firmware_md5\":\"" + jsonEscape(firmwareSketchMd5) + "\"";
  payload += ",\"battery_mv\":" + String(batteryMv);
  payload += ",\"chip_temperature_c\":" + String(temperatureDeciC / 10.0f, 1);
  payload += ",\"free_heap\":" + String(ESP.getFreeHeap());
  payload += ",\"minimum_free_heap\":" + String(ESP.getMinFreeHeap());
  payload += ",\"largest_free_heap_block\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  payload += ",\"http_task_stack_high_water\":" + String(httpServiceTaskHandle == nullptr ? 0 : uxTaskGetStackHighWaterMark(httpServiceTaskHandle));
  payload += ",\"upload_task_stack_high_water\":" + String(trailMediaUploadTaskHandle == nullptr ? 0 : uxTaskGetStackHighWaterMark(trailMediaUploadTaskHandle));
  payload += ",\"low_battery_boot_hold\":" + buildLowBatteryBootHoldJson();
  payload += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"sd_ready\":" + String(sdReady ? "true" : "false");
  payload += ",\"sd_write_count\":" + String(diagnosticSdWriteCount);
  payload += ",\"sd_write_failures\":" + String(diagnosticSdWriteFailures);
  payload += ",\"nvs_write_count\":" + String(diagnosticNvsWriteCount);
  payload += ",\"failsafe\":{";
  payload += "\"last_reason\":\"" + jsonEscape(failsafeLastReason) + "\"";
  payload += ",\"restart_count\":" + String(failsafeRestartCount);
  payload += ",\"consecutive_same_reason\":" + String(failsafeConsecutiveSameReason);
  payload += ",\"automatic_recovery_suspended\":" + String(automaticRecoverySuspended ? "true" : "false");
  payload += ",\"http_stall_ms\":" + String(FAILSAFE_HTTP_STALL_MS);
  payload += ",\"upload_stall_ms\":" + String(FAILSAFE_UPLOAD_STALL_MS);
  payload += ",\"http_self_test_successes\":" + String(httpSelfTestSuccessCount);
  payload += ",\"http_self_test_failures\":" + String(httpSelfTestFailureCount);
  payload += ",\"http_self_test_consecutive_failures\":" + String(httpSelfTestConsecutiveFailures);
  payload += "}";
  payload += ",\"previous_boot\":{";
  payload += "\"operational\":" + String(previousBootOperational ? "true" : "false");
  payload += ",\"last_checkpoint_uptime_ms\":" + String(previousBootUptimeMs);
  payload += ",\"battery_mv\":" + String(previousBootBatteryMv);
  payload += ",\"chip_temperature_c\":" + String(previousBootTemperatureDeciC / 10.0f, 1);
  payload += ",\"free_heap\":" + String(previousBootFreeHeap);
  payload += ",\"halow_connected\":" + String(previousBootHaLowConnected ? "true" : "false");
  payload += "}}";
  return payload;
}

bool writeDiagnosticCheckpointNvs(bool operational) {
  const uint32_t adcMv = readBatteryAdcMilliVolts(16);
  if (!runtimePrefs.begin("runtime", false)) {
    Serial.println("[diagnostics] failed to open runtime preferences");
    return false;
  }
  runtimePrefs.putBool("boot_ok", operational);
  runtimePrefs.putUInt("last_up_ms", millis());
  runtimePrefs.putUInt("last_bat_mv", adcMv * 2);
  runtimePrefs.putShort("last_temp", static_cast<int16_t>(readChipTemperatureC() * 10.0f));
  runtimePrefs.putUInt("last_heap", ESP.getFreeHeap());
  runtimePrefs.putBool("last_halow", halowConnected);
  runtimePrefs.end();
  ++diagnosticNvsWriteCount;
  lastDiagnosticNvsMs = millis();
  return true;
}

bool appendDiagnosticJournal(const char *eventName) {
  if (!sdReady || cameraSdTransferActive) {
    ++diagnosticSdWriteFailures;
    return false;
  }
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    ++diagnosticSdWriteFailures;
    return false;
  }
  if (!ensureFsDir(SD, DIAGNOSTIC_DIR)) {
    ++diagnosticSdWriteFailures;
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }
  File existing = SD.open(DIAGNOSTIC_LOG_PATH, FILE_READ);
  const size_t existingSize = existing ? existing.size() : 0;
  if (existing) existing.close();
  if (existingSize > 2UL * 1024UL * 1024UL) {
    SD.remove("/diagnostics/health.previous.jsonl");
    SD.rename(DIAGNOSTIC_LOG_PATH, "/diagnostics/health.previous.jsonl");
  }
  File log = SD.open(DIAGNOSTIC_LOG_PATH, FILE_APPEND);
  if (!log) {
    ++diagnosticSdWriteFailures;
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }
  String record = "{";
  record += "\"event\":\"" + jsonEscape(String(eventName)) + "\"";
  record += ",\"epoch\":" + String(onboardClockValid() ? static_cast<unsigned long>(time(nullptr)) : 0UL);
  record += ",\"diagnostics\":" + buildDiagnosticStatusJson();
  record += "}\n";
  const bool ok = log.print(record) == record.length();
  log.flush();
  log.close();
  if (ok) {
    ++diagnosticSdWriteCount;
    lastDiagnosticSdMs = millis();
  } else {
    ++diagnosticSdWriteFailures;
  }
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  return ok;
}

void markBootOperational() {
  if (bootMarkedOperational) return;
  bootMarkedOperational = true;
  writeDiagnosticCheckpointNvs(true);
  appendDiagnosticJournal("boot_operational");
  Serial.printf("[diagnostics] boot operational reset=%s previous_operational=%s\n",
                currentResetReasonName.c_str(),
                previousBootOperational ? "yes" : "no");
}

void servicePersistentDiagnostics() {
  if (sdReady && (lastDiagnosticSdMs == 0 || millis() - lastDiagnosticSdMs >= DIAGNOSTIC_SD_INTERVAL_MS)) {
    appendDiagnosticJournal("health");
  }
  if (bootMarkedOperational &&
      (lastDiagnosticNvsMs == 0 || millis() - lastDiagnosticNvsMs >= DIAGNOSTIC_NVS_INTERVAL_MS)) {
    writeDiagnosticCheckpointNvs(true);
  }
}

String buildSdStatusJson() {
  String payload = "{";
  payload += "\"ready\":" + String(sdReady ? "true" : "false");
  payload += ",\"durable_media_job_loaded\":" + String(durableMediaJobLoaded ? "true" : "false");
  payload += ",\"last_message\":\"" + jsonEscape(sdLastMessage) + "\"";
  payload += ",\"mount_attempts\":" + String(sdMountAttempts);
  payload += ",\"mount_successes\":" + String(sdMountSuccesses);
  payload += ",\"last_mount_frequency_hz\":" + String(sdLastMountFrequencyHz);
  payload += ",\"background_recovery_attempts\":" + String(sdBackgroundRecoveryAttempts);
  payload += ",\"background_recovery_successes\":" + String(sdBackgroundRecoverySuccesses);
  payload += ",\"last_background_recovery_age_ms\":" + String(msSince(sdLastBackgroundRecoveryMs));
  payload += ",\"background_recovery_interval_ms\":" + String(sdBackgroundRecoveryIntervalMs);
  payload += ",\"recovery_continues_in_safe_mode\":true";
  payload += ",\"battery_mv\":" + String(cachedStorageBatteryMv());
  payload += ",\"recovery_min_battery_mv\":" + String(SD_RECOVERY_MIN_BATTERY_MV);
  payload += ",\"pins\":{";
  payload += "\"mosi\":" + String(SD_MOSI_PIN);
  payload += ",\"clk\":" + String(SD_CLK_PIN);
  payload += ",\"miso\":" + String(SD_MISO_PIN);
  payload += ",\"cs\":" + String(SD_CS_PIN);
  payload += "}";
  if (sdReady) {
    uint8_t cardType = CARD_NONE;
    uint64_t cardSize = 0;
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    portENTER_CRITICAL(&sdCapacityCacheMux);
    cardType = sdCachedCardType;
    cardSize = sdCachedCardSizeBytes;
    totalBytes = sdCachedTotalBytes;
    usedBytes = sdCachedUsedBytes;
    portEXIT_CRITICAL(&sdCapacityCacheMux);
    payload += ",\"card_type\":" + String(static_cast<unsigned>(cardType));
    payload += ",\"card_size_bytes\":" + String(static_cast<unsigned long long>(cardSize));
    payload += ",\"storage_total_bytes\":" + String(static_cast<unsigned long long>(totalBytes));
    payload += ",\"storage_used_bytes\":" + String(static_cast<unsigned long long>(usedBytes));
    const uint64_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
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
  if (!ensureOnboardCameraReady(captureError)) {
    ++onboardCaptureFailures;
    return false;
  }
  if (!onboardStorageReady) {
    ++onboardCaptureFailures;
    onboardLastCaptureError = "storage_unavailable";
    if (captureError != nullptr) *captureError = "storage_unavailable";
    return false;
  }

  camera_fb_t *fb = nullptr;
  bool cameraLocked = false;
  if (onboardCameraMutex != nullptr) {
    if (xSemaphoreTake(onboardCameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
      ++onboardCaptureFailures;
      onboardLastCaptureError = "onboard_camera_busy";
      if (captureError != nullptr) *captureError = onboardLastCaptureError;
      return false;
    }
    cameraLocked = true;
  }

  fb = esp_camera_fb_get();
  if (fb == nullptr || fb->len == 0) {
    if (fb != nullptr) esp_camera_fb_return(fb);
    if (cameraLocked) {
      xSemaphoreGive(onboardCameraMutex);
      cameraLocked = false;
    }
    ++onboardCaptureRecoveryCount;
    onboardLastCaptureError = "capture_failed";
    if (reinitOnboardCamera("capture_fb_failed")) {
      cooperativeDelay(250);
      if (onboardCameraMutex != nullptr) {
        if (xSemaphoreTake(onboardCameraMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
          ++onboardCaptureFailures;
          onboardLastCaptureError = "onboard_camera_busy_after_reinit";
          if (captureError != nullptr) *captureError = onboardLastCaptureError;
          return false;
        }
        cameraLocked = true;
      }
      fb = esp_camera_fb_get();
    }
  }

  if (fb == nullptr || fb->len == 0) {
    if (fb != nullptr) esp_camera_fb_return(fb);
    if (cameraLocked) xSemaphoreGive(onboardCameraMutex);
    ++onboardCaptureFailures;
    onboardLastCaptureError = "capture_failed_after_reinit";
    if (captureError != nullptr) *captureError = onboardLastCaptureError;
    return false;
  }

  const size_t copyLen = fb->len;
  uint8_t *copy = static_cast<uint8_t *>(psramFound() ? ps_malloc(copyLen) : malloc(copyLen));
  if (copy == nullptr) {
    esp_camera_fb_return(fb);
    if (cameraLocked) xSemaphoreGive(onboardCameraMutex);
    ++onboardCaptureFailures;
    onboardLastCaptureError = "capture_alloc_failed";
    if (captureError != nullptr) *captureError = "capture_failed";
    return false;
  }
  memcpy(copy, fb->buf, copyLen);
  const uint16_t width = fb->width;
  const uint16_t height = fb->height;
  esp_camera_fb_return(fb);
  if (cameraLocked) xSemaphoreGive(onboardCameraMutex);

  if (onboardStorageMutex != nullptr) xSemaphoreTake(onboardStorageMutex, portMAX_DELAY);
  fs::FS &fs = onboardMediaFs();

  const uint64_t freeBytes = persistentStorageFreeBytes();
  if (freeBytes < copyLen + sizeof(OnboardMediaMetaDisk) + 4096) {
    free(copy);
    ++onboardCaptureFailures;
    onboardStorageError = "storage_full";
    onboardLastCaptureError = "storage_full";
    if (captureError != nullptr) *captureError = "storage_full";
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }

  const uint32_t id = onboardNextMediaId;
  const uint32_t recordedAt = onboardClockValid() ? static_cast<uint32_t>(time(nullptr)) : 0;
  const String imagePath = onboardMediaImagePath(id);
  const String metaPath = onboardMediaMetaPath(id);
  const String tempImagePath = String(ONBOARD_MEDIA_DIR) + "/.capture.jpg.tmp";
  const String tempMetaPath = String(ONBOARD_MEDIA_DIR) + "/.capture.meta.tmp";
  fs.remove(tempImagePath);
  fs.remove(tempMetaPath);
  File imageFile = fs.open(tempImagePath, FILE_WRITE);
  const size_t imageWritten = imageFile ? imageFile.write(copy, copyLen) : 0;
  if (imageFile) imageFile.close();

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
    onboardLastCaptureError = "storage_unavailable";
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
  onboardLastCaptureError = "";

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
  payload += ",\"power_state\":\"" + jsonEscape(onboardCameraPowerState) + "\"";
  payload += ",\"powered_down_by_schedule\":" + String(onboardCameraPoweredDownBySchedule ? "true" : "false");
  payload += ",\"power_up_count\":" + String(onboardCameraPowerUpCount);
  payload += ",\"power_down_count\":" + String(onboardCameraPowerDownCount);
  payload += ",\"last_power_up_age_ms\":" + String(msSince(onboardCameraLastPowerUpMs));
  payload += ",\"last_power_down_age_ms\":" + String(msSince(onboardCameraLastPowerDownMs));
  payload += ",\"enabled\":" + String(onboardCaptureEnabled ? "true" : "false");
  payload += ",\"interval_ms\":" + String(onboardCaptureIntervalMs);
  payload += ",\"window_start\":\"" + minuteOfDayToString(onboardCaptureStartMinute) + "\"";
  payload += ",\"window_end\":\"" + minuteOfDayToString(onboardCaptureEndMinute) + "\"";
  payload += ",\"window_start_minute\":" + String(onboardCaptureStartMinute);
  payload += ",\"window_end_minute\":" + String(onboardCaptureEndMinute);
  payload += ",\"tz_offset_min\":" + String(onboardCaptureTzOffsetMin);
  payload += ",\"clock_valid\":" + String(onboardClockValid() ? "true" : "false");
  payload += ",\"schedule_mode\":\"" + String(onboardClockValid() ? "clock_window" : "uptime_window_fallback") + "\"";
  payload += ",\"local_minute\":" + String(localMinute);
  payload += ",\"actual_local_minute\":" + String(onboardActualLocalMinuteOfDay());
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
  payload += ",\"camera_reinit_count\":" + String(onboardCameraReinitCount);
  payload += ",\"capture_recovery_count\":" + String(onboardCaptureRecoveryCount);
  payload += ",\"last_capture_error\":\"" + jsonEscape(onboardLastCaptureError) + "\"";
  payload += ",\"last_recovery_reason\":\"" + jsonEscape(onboardLastRecoveryReason) + "\"";
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

bool queueOnboardJob(OnboardJobType type,
                     const char *captureKind,
                     bool oneShotSettings,
                     const OnboardCameraSettings *settings,
                     String &error) {
  if (onboardJobMutex != nullptr) xSemaphoreTake(onboardJobMutex, portMAX_DELAY);
  if (onboardJob.pending || onboardJob.running) {
    if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);
    error = "onboard_camera_busy";
    return false;
  }
  onboardJob = {};
  onboardJob.type = type;
  onboardJob.pending = true;
  onboardJob.oneShotSettings = oneShotSettings;
  onboardJob.settingsRestored = !oneShotSettings;
  snprintf(onboardJob.captureKind, sizeof(onboardJob.captureKind), "%s", captureKind != nullptr ? captureKind : "manual");
  if (settings != nullptr) {
    onboardJob.settings = *settings;
  }
  if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);
  error = "";
  return true;
}

bool waitOnboardJob(unsigned long timeoutMs, OnboardJobState &result, String &error) {
  const unsigned long startedMs = millis();
  while (millis() - startedMs < timeoutMs) {
    if (onboardJobMutex != nullptr) xSemaphoreTake(onboardJobMutex, portMAX_DELAY);
    const bool done = onboardJob.done;
    if (done) {
      result = onboardJob;
      onboardJob = {};
      if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);
      error = result.error;
      return true;
    }
    if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);
    cooperativeDelay(25);
  }
  error = "onboard_job_timeout";
  return false;
}

bool runQueuedOnboardJob() {
  OnboardJobState job{};
  if (onboardJobMutex != nullptr) xSemaphoreTake(onboardJobMutex, portMAX_DELAY);
  if (!onboardJob.pending || onboardJob.running) {
    if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);
    return false;
  }
  onboardJob.pending = false;
  onboardJob.running = true;
  job = onboardJob;
  if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);

  String jobError;
  if (job.type == ONBOARD_JOB_REINIT) {
    job.ok = reinitOnboardCamera("worker_api_request");
    if (!job.ok) jobError = "onboard_camera_not_ready";
  } else if (job.type == ONBOARD_JOB_CAPTURE) {
    OnboardCameraSettings restoreSettings = snapshotOnboardCameraSettings();
    if (job.oneShotSettings) {
      job.ok = applyOnboardCameraSettings(job.settings, jobError);
      if (job.ok) cooperativeDelay(150);
    } else {
      job.ok = true;
    }
    if (job.ok) {
      job.ok = captureOnboardFrame(job.captureKind, &job.media, &jobError);
    }
    if (job.oneShotSettings) {
      String restoreError;
      job.settingsRestored = applyOnboardCameraSettings(restoreSettings, restoreError);
      if (!job.settingsRestored) {
        Serial.printf("[onboard-camera] worker failed to restore settings: %s\n", restoreError.c_str());
      }
    }
  } else {
    job.ok = false;
    jobError = "invalid_onboard_job";
  }

  snprintf(job.error, sizeof(job.error), "%s", jobError.c_str());
  job.pending = false;
  job.running = false;
  job.done = true;
  if (onboardJobMutex != nullptr) xSemaphoreTake(onboardJobMutex, portMAX_DELAY);
  onboardJob = job;
  if (onboardJobMutex != nullptr) xSemaphoreGive(onboardJobMutex);
  return true;
}

void onboardCaptureTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    if (runQueuedOnboardJob()) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    refreshOnboardTimelapseState();
    const unsigned long nowMs = millis();
    if (onboardTimelapseActive) {
      String cameraError;
      if (!ensureOnboardCameraReady(&cameraError)) {
        ++onboardCaptureFailures;
        Serial.printf("[onboard-timelapse] camera unavailable error=%s\n", cameraError.c_str());
      } else {
        if (onboardTimelapseLastCaptureMs == 0 ||
            nowMs - onboardTimelapseLastCaptureMs >= onboardTimelapseIntervalMs) {
          if (captureOnboardFrame("timelapse")) {
            onboardTimelapseLastCaptureMs = millis();
            ++onboardTimelapseCaptureCount;
          }
        }
      }
    } else if (onboardCaptureEnabled) {
      const bool windowActive = onboardCaptureWindowActive();
      if (!windowActive) {
        if (onboardCameraReady) {
          shutdownOnboardCamera("scheduled_window_inactive");
          onboardCameraPoweredDownBySchedule = true;
        }
        if (onboardLastScheduleAttemptMs == 0 ||
            nowMs - onboardLastScheduleAttemptMs >= onboardCaptureIntervalMs) {
          ++onboardCaptureScheduleSkips;
          Serial.printf("[onboard-camera] scheduled capture skipped/powered_down clock_valid=%s local_minute=%d window=%s-%s\n",
                        onboardClockValid() ? "yes" : "no",
                        onboardLocalMinuteOfDay(),
                        minuteOfDayToString(onboardCaptureStartMinute).c_str(),
                        minuteOfDayToString(onboardCaptureEndMinute).c_str());
        }
      } else {
        String cameraError;
        if (!ensureOnboardCameraReady(&cameraError)) {
          ++onboardCaptureFailures;
          Serial.printf("[onboard-camera] scheduled capture camera unavailable error=%s\n", cameraError.c_str());
        } else {
          onboardCameraPoweredDownBySchedule = false;
          const unsigned long normalIntervalMs = onboardCaptureIntervalMs < 5000UL ? 5000UL : onboardCaptureIntervalMs;
          if (onboardLastScheduleAttemptMs == 0 ||
              nowMs - onboardLastScheduleAttemptMs >= normalIntervalMs) {
            onboardLastScheduleAttemptMs = nowMs;
            captureOnboardFrame("scheduled");
          }
        }
      }
    } else if (onboardCameraPoweredDownBySchedule && !onboardCaptureEnabled) {
      onboardCameraPoweredDownBySchedule = false;
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
  if (streamSessionActive || wifiConnected || transferWifiConnected ||
      trailMediaUploadActive || isControlActionActive()) {
    error = "camera_wifi_active";
    return false;
  }

  if (wifiRadioMutex != nullptr &&
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    error = "wifi_radio_busy";
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
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
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
  if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
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
  payload += ",\"scanner_schedule_enabled\":" + String(scannerScheduleEnabled ? "true" : "false");
  payload += ",\"attempts\":" + String(uploadAttemptCount);
  payload += ",\"successes\":" + String(uploadSuccessCount);
  payload += ",\"failures\":" + String(uploadFailureCount);
  payload += ",\"last_status_code\":" + String(uploadLastStatusCode);
  payload += ",\"last_message\":\"" + jsonEscape(uploadLastMessage) + "\"";
  payload += ",\"last_attempt_age_ms\":" + String(msSince(uploadLastAttemptMs));
  payload += ",\"last_success_age_ms\":" + String(msSince(uploadLastSuccessMs));
  payload += ",\"queued_events\":" + String(static_cast<unsigned>(uploadEventCount));
  payload += ",\"trail_media\":{";
  payload += "\"active\":" + String(trailMediaUploadActive ? "true" : "false");
  payload += ",\"filename\":\"" + jsonEscape(trailMediaUploadFilename) + "\"";
  payload += ",\"bytes_sent\":" + String(trailMediaUploadBytesSent);
  payload += ",\"file_bytes\":" + String(trailMediaUploadFileBytes);
  payload += ",\"bytes_hashed\":" + String(trailMediaUploadBytesHashed);
  payload += ",\"successes\":" + String(trailMediaUploadSuccessCount);
  payload += ",\"failures\":" + String(trailMediaUploadFailureCount);
  payload += ",\"skipped_after_max_attempts\":" + String(trailMediaUploadSkippedCount);
  payload += ",\"max_attempts_per_file\":" + String(TRAIL_MEDIA_MAX_UPLOAD_ATTEMPTS);
  payload += ",\"last_skipped_file\":\"" + jsonEscape(trailMediaUploadLastSkipped) + "\"";
  payload += ",\"last_attempt_age_ms\":" + String(msSince(trailMediaUploadLastAttemptMs));
  payload += ",\"last_success_age_ms\":" + String(msSince(trailMediaUploadLastSuccessMs));
  payload += ",\"last_error\":\"" + jsonEscape(trailMediaUploadLastError) + "\"";
  payload += ",\"blocked_reason\":\"" + jsonEscape(trailMediaUploadBlockedReason) + "\"";
  payload += ",\"queue_job_id\":" + String(trailMediaUploadCursorJobId);
  payload += ",\"queue_position\":" + String(trailMediaUploadCursorPosition);
  payload += ",\"preferred_transport\":\"" + trailMediaUploadPreferredTransport + "\"";
  payload += ",\"fallback_transport\":\"halow\"";
  payload += ",\"battery_mv\":" + String(cachedStorageBatteryMv());
  payload += ",\"minimum_battery_mv\":" + String(MEDIA_UPLOAD_MIN_BATTERY_MV);
  payload += "}";
  payload += ",\"transfer_wifi\":{";
  payload += "\"configured\":" + String(strlen(TRANSFER_WIFI_SSID) > 0 ? "true" : "false");
  payload += ",\"preferred\":" + String(TRANSFER_WIFI_UPLOAD_ENABLED ? "true" : "false");
  payload += ",\"connected\":" + String(transferWifiConnected ? "true" : "false");
  payload += ",\"ip\":\"" + jsonEscape(transferWifiConnected ? WiFi.localIP().toString() : String("0.0.0.0")) + "\"";
  payload += ",\"connect_attempts\":" + String(transferWifiConnectAttempts);
  payload += ",\"session_files\":" + String(transferWifiSessionFiles);
  payload += ",\"batch_file_limit\":" + String(TRANSFER_WIFI_BATCH_FILES);
  payload += ",\"session_age_ms\":" + String(msSince(transferWifiSessionStartedMs));
  payload += ",\"session_max_ms\":" + String(TRANSFER_WIFI_SESSION_MAX_MS);
  payload += ",\"last_error\":\"" + jsonEscape(transferWifiLastError) + "\"";
  payload += "}";
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

String buildScannerConfigJson() {
  String payload = "{";
  payload += "\"feature_enabled\":false";
  payload += ",\"error\":\"scanner_disabled\"";
  payload += ",\"detail\":\"WiFi/BLE scanning is disabled in camera-priority firmware\"";
  payload += ",\"scheduled_scans_enabled\":" + String(scannerScheduleEnabled ? "true" : "false");
  payload += ",\"default_enabled\":" + String((SCANNER_SCHEDULE_ENABLED != 0) ? "true" : "false");
  payload += ",\"manual_upload_endpoint\":\"/upload/observations\"";
  payload += ",\"config_endpoint\":\"/scanner/config\"";
  payload += ",\"observations_api_host\":\"" + jsonEscape(AIR_SCAN_API_HOST) + "\"";
  payload += ",\"observations_api_port\":" + String(AIR_SCAN_API_PORT);
  payload += ",\"observations_api_path\":\"/api/observations/upload\"";
  payload += ",\"day_interval_ms\":" + String(WIFI_SCAN_DAY_INTERVAL_MS);
  payload += ",\"night_interval_ms\":" + String(WIFI_SCAN_NIGHT_INTERVAL_MS);
  payload += ",\"wifi_scanner\":{";
  payload += "\"runs\":" + String(wifiScannerRunCount);
  payload += ",\"upload_successes\":" + String(wifiScannerUploadSuccessCount);
  payload += ",\"upload_failures\":" + String(wifiScannerUploadFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(wifiScannerLastError) + "\"";
  payload += "}";
  payload += ",\"ble_scanner\":{";
  payload += "\"runs\":" + String(bleScannerRunCount);
  payload += ",\"upload_successes\":" + String(bleScannerUploadSuccessCount);
  payload += ",\"upload_failures\":" + String(bleScannerUploadFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(bleScannerLastError) + "\"";
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
    case CONTROL_ACTION_TRIGGER_PHOTO:
      return "trigger_photo";
    case CONTROL_ACTION_NONE:
    default:
      return "none";
  }
}

String controlActionJson(ControlAction action) {
  if (action == CONTROL_ACTION_NONE) {
    return "null";
  }
  return "\"" + String(controlActionName(action)) + "\"";
}

uint16_t controlActionEtaSec(ControlAction action) {
  switch (action) {
    case CONTROL_ACTION_BRINGUP:
      return 45;
    case CONTROL_ACTION_STREAM_START:
      return 60;
    case CONTROL_ACTION_STREAM_STOP:
      return 10;
    case CONTROL_ACTION_TRIGGER_PHOTO:
      return 45;
    case CONTROL_ACTION_NONE:
    default:
      return 0;
  }
}

String controlErrorFromMessage(ControlAction action, const String &message) {
  if (message.isEmpty()) {
    return "control_action_failed";
  }
  if (message.indexOf("scan_not_found") >= 0) return "ble_scan_not_found";
  if (message.indexOf("connect failed") >= 0 || message.indexOf("connect_failed") >= 0) return "ble_connect_failed";
  if (message.indexOf("wake") >= 0 || message.indexOf("bringup_failed") >= 0) {
    if (action == CONTROL_ACTION_BRINGUP) return "ble_wake_failed";
    if (action == CONTROL_ACTION_TRIGGER_PHOTO) return "trigger_photo_bringup_failed";
  }
  if (message.indexOf("hotspot") >= 0) return "camera_hotspot_not_found";
  if (message.indexOf("wifi") >= 0) return "camera_wifi_join_failed";
  if (message.indexOf("rtsp_describe") >= 0) return "rtsp_describe_failed";
  if (message.indexOf("rtsp_setup") >= 0) return "rtsp_setup_failed";
  if (message.indexOf("rtsp_play") >= 0) return "rtsp_play_failed";
  if (message.indexOf("rtp_stalled") >= 0) return "stream_rtp_stalled";
  if (message.indexOf("tunnel") >= 0) return "tunnel_connect_failed";
  if (message.indexOf("trigger_photo_take") >= 0) return "trigger_photo_take_failed";
  if (message.indexOf("trigger_photo_result") >= 0) return "trigger_photo_result_timeout";
  if (message.indexOf("camera_http") >= 0) return "camera_http_failed";
  switch (action) {
    case CONTROL_ACTION_BRINGUP:
      return "bringup_failed";
    case CONTROL_ACTION_STREAM_START:
      return "stream_start_failed";
    case CONTROL_ACTION_STREAM_STOP:
      return "stream_stop_failed";
    case CONTROL_ACTION_TRIGGER_PHOTO:
      return "trigger_photo_failed";
    case CONTROL_ACTION_NONE:
    default:
      return "control_action_failed";
  }
}

String controlStateFromSnapshot(const ControlState &snapshot) {
  if (snapshot.busy) {
    switch (snapshot.activeAction) {
      case CONTROL_ACTION_BRINGUP:
        return "bringing_up";
      case CONTROL_ACTION_STREAM_START:
        return "stream_starting";
      case CONTROL_ACTION_STREAM_STOP:
        return "going_standby";
      case CONTROL_ACTION_TRIGGER_PHOTO:
        return "triggering_photo";
      case CONTROL_ACTION_NONE:
      default:
        return "idle";
    }
  }
  if (snapshot.pendingAction != CONTROL_ACTION_NONE) {
    switch (snapshot.pendingAction) {
      case CONTROL_ACTION_BRINGUP:
        return "bringing_up";
      case CONTROL_ACTION_STREAM_START:
        return "stream_starting";
      case CONTROL_ACTION_STREAM_STOP:
        return "going_standby";
      case CONTROL_ACTION_TRIGGER_PHOTO:
        return "triggering_photo";
      case CONTROL_ACTION_NONE:
      default:
        break;
    }
  }
  if (!snapshot.lastOk && snapshot.lastAction != CONTROL_ACTION_NONE) {
    return "error";
  }
  if (streamSessionActive) {
    return "streaming";
  }
  if (wifiConnected) {
    return "connected";
  }
  return "idle";
}

String controlProgressFromSnapshot(const ControlState &snapshot) {
  if (!snapshot.busy) {
    if (snapshot.pendingAction != CONTROL_ACTION_NONE) {
      return "queued";
    }
    if (!snapshot.lastOk && snapshot.lastAction != CONTROL_ACTION_NONE) {
      return "failed";
    }
    if (streamSessionActive) return "streaming";
    if (wifiConnected) return "camera_wifi_connected";
    return "idle";
  }
  if (snapshot.activeAction == CONTROL_ACTION_BRINGUP) {
    if (bleStage == "scan") return "ble_scanning";
    if (bleStage == "reuse_wake" || bleStage == "wake") return "ble_waking";
    if (bleStage == "connect") return "ble_connecting";
    if (bleStage == "wake_ok") return "waiting_for_camera_hotspot";
    return "bringing_up";
  }
  if (snapshot.activeAction == CONTROL_ACTION_STREAM_START) {
    if (!lastStreamStartStage.isEmpty() && lastStreamStartStage != "idle") {
      return lastStreamStartStage;
    }
    return "starting_rtsp";
  }
  if (snapshot.activeAction == CONTROL_ACTION_STREAM_STOP) {
    return "going_standby";
  }
  if (snapshot.activeAction == CONTROL_ACTION_TRIGGER_PHOTO) {
    return wifiConnected ? "triggering_photo" : "bringing_up";
  }
  return "working";
}

String controlProgressText(const String &progress) {
  if (progress == "idle") return "Idle";
  if (progress == "queued") return "Queued";
  if (progress == "ble_scanning") return "Scanning for camera BLE";
  if (progress == "ble_connecting") return "Connecting BLE";
  if (progress == "ble_waking") return "Waking camera over BLE";
  if (progress == "waiting_for_camera_hotspot") return "Waiting for camera Wi-Fi hotspot";
  if (progress == "bringing_up") return "Bringing camera up";
  if (progress == "camera_wifi_connected") return "Camera Wi-Fi connected";
  if (progress == "camera_bringup") return "Bringing camera up for live view";
  if (progress == "camera_wifi_down") return "Camera Wi-Fi unavailable";
  if (progress == "starting_rtsp") return "Starting live view";
  if (progress == "rtp_stalled") return "Camera RTP stalled";
  if (progress == "streaming") return "Streaming";
  if (progress == "going_standby") return "Going standby";
  if (progress == "triggering_photo") return "Triggering photo";
  if (progress == "failed") return "Failed";
  return progress;
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
  if (accepted) {
    controlState.lastError[0] = '\0';
    controlState.lastMessage[0] = '\0';
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
  const String messageText = message == nullptr ? String("") : String(message);
  const String errorText = ok ? String("") : controlErrorFromMessage(action, messageText);
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
           messageText.c_str());
  snprintf(controlState.lastError,
           sizeof(controlState.lastError),
           "%s",
           errorText.c_str());
  portEXIT_CRITICAL(&controlState.lock);

  String details = "{";
  details += "\"action\":\"" + String(controlActionName(action)) + "\"";
  details += ",\"message\":\"" + jsonEscape(messageText) + "\"";
  details += ",\"error\":" + jsonNullableString(errorText);
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

  String extraHeaders = "Session: " + rtspSessionHeader + "\r\n";
  String responseText;
  int statusCode = 0;

  // GardePro-compatible RTSP servers commonly expect GET_PARAMETER during
  // playback.  Keep OPTIONS as a fallback for cameras that only implement
  // the older keepalive behavior.
  const char *methods[] = {"OPTIONS", "GET_PARAMETER"};
  for (size_t i = 0; i < 2; ++i) {
    responseText = "";
    statusCode = 0;
    lastRtspKeepaliveMethod = methods[i];
    const bool exchanged = exchangeCameraRtspRequest(cameraRtspClient,
                                                     methods[i],
                                                     rtspPlayUrl,
                                                     extraHeaders,
                                                     responseText,
                                                     statusCode);
    lastRtspKeepaliveStatus = statusCode;
    if (exchanged && statusCode == 200) {
      ++rtspKeepaliveCount;
      lastRtspKeepaliveMs = millis();
      Serial.printf("[rtsp] keepalive ok method=%s\n", methods[i]);
      return true;
    }
    // A method rejection is safe to fall through to the alternate method;
    // transport failure is not, but leave the session open for the stall
    // guard to make the final decision if RTP also stops.
    Serial.printf("[rtsp] keepalive method=%s status=%d exchanged=%s\n",
                  methods[i],
                  statusCode,
                  exchanged ? "yes" : "no");
    if (!cameraRtspClient.connected()) {
      break;
    }
  }

  ++rtspKeepaliveFailures;
  return false;
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

String cameraWifiSsidFromBleMac(const String &mac) {
  String suffix = mac;
  suffix.replace(":", "");
  suffix.toUpperCase();
  if (suffix.length() != 12) {
    return "";
  }
  return String(CAMERA_BLE_NAME_PREFIX) + suffix;
}

void setCameraTargetFromAdvertisement(const String &mac, const String &name) {
  cameraTargetBleMac = mac;
  cameraTargetBleName = name;
  const String derivedSsid = cameraWifiSsidFromBleMac(mac);
  if (!derivedSsid.isEmpty()) {
    cameraTargetWifiSsid = derivedSsid;
  }
  Serial.printf("[camera-target] ble_mac=%s name=%s wifi_ssid=%s\n",
                cameraTargetBleMac.c_str(),
                cameraTargetBleName.c_str(),
                cameraTargetWifiSsid.c_str());
}

bool persistCameraTarget() {
  Preferences prefs;
  if (!prefs.begin("camera_target", false)) return false;
  const bool ok = prefs.putString("ble_mac", cameraTargetBleMac) > 0 &&
                  prefs.putString("ble_name", cameraTargetBleName) > 0 &&
                  prefs.putString("wifi_ssid", cameraTargetWifiSsid) > 0;
  prefs.end();
  return ok;
}

void loadPersistedCameraTarget() {
  Preferences prefs;
  if (!prefs.begin("camera_target", true)) return;
  String mac = prefs.getString("ble_mac", "");
  String name = prefs.getString("ble_name", "");
  String ssid = prefs.getString("wifi_ssid", "");
  prefs.end();
  mac.trim();
  ssid.trim();
  if (mac.isEmpty() || ssid.isEmpty()) return;
  cameraTargetBleMac = mac;
  cameraTargetBleName = name;
  cameraTargetWifiSsid = ssid;
  Serial.printf("[camera-target] restored ble_mac=%s name=%s wifi_ssid=%s\n",
                cameraTargetBleMac.c_str(), cameraTargetBleName.c_str(),
                cameraTargetWifiSsid.c_str());
}

bool setCameraTarget(const String &rawBleMac,
                     const String &rawWifiSsid,
                     const String &rawName,
                     String &error) {
  String mac = rawBleMac;
  String ssid = rawWifiSsid;
  String name = rawName;
  mac.trim();
  ssid.trim();
  name.trim();
  mac.toLowerCase();

  if (mac.isEmpty()) {
    error = "missing_ble_mac";
    return false;
  }

  int colonCount = 0;
  for (size_t i = 0; i < mac.length(); ++i) {
    if (mac[i] == ':') {
      ++colonCount;
    }
  }
  if (!(mac.length() == 17 && colonCount == 5) && mac.length() != 12) {
    error = "invalid_ble_mac";
    return false;
  }
  if (mac.length() == 12) {
    String formatted;
    for (size_t i = 0; i < mac.length(); ++i) {
      if (i > 0 && (i % 2) == 0) {
        formatted += ":";
      }
      formatted += mac[i];
    }
    mac = formatted;
  }

  if (ssid.isEmpty()) {
    ssid = cameraWifiSsidFromBleMac(mac);
  }
  if (ssid.isEmpty()) {
    error = "missing_wifi_ssid";
    return false;
  }

  const bool changed = !mac.equalsIgnoreCase(cameraTargetBleMac) || ssid != cameraTargetWifiSsid;
  if (changed) {
    closeBleWakeSession();
    if (wifiConnected || WiFi.status() == WL_CONNECTED) {
      WiFi.disconnect(true, true);
      cooperativeDelay(200);
      refreshWifiState();
    }
    standbyRequested = false;
    cameraWifiEverConnected = false;
    streamSessionActive = false;
    bleTargetDeviceFound = false;
  }

  cameraTargetBleMac = mac;
  cameraTargetBleName = name;
  cameraTargetWifiSsid = ssid;
  if (changed && !persistCameraTarget()) {
    Serial.println("[camera-target] warning: failed to persist selection");
  }
  error = "";
  Serial.printf("[camera-target] selected ble_mac=%s name=%s wifi_ssid=%s changed=%s\n",
                cameraTargetBleMac.c_str(),
                cameraTargetBleName.c_str(),
                cameraTargetWifiSsid.c_str(),
                changed ? "yes" : "no");
  return true;
}

String buildCameraTargetJson() {
  String payload = "{";
  payload += "\"active\":{";
  payload += "\"ble_mac\":\"" + jsonEscape(cameraTargetBleMac) + "\"";
  payload += ",\"ble_name\":\"" + jsonEscape(cameraTargetBleName) + "\"";
  payload += ",\"wifi_ssid\":\"" + jsonEscape(cameraTargetWifiSsid) + "\"";
  payload += "}";
  payload += ",\"profiles\":[";
  for (size_t i = 0; i < sizeof(CAMERA_PROFILES) / sizeof(CAMERA_PROFILES[0]); ++i) {
    if (i > 0) payload += ",";
    payload += "{";
    payload += "\"id\":\"" + String(CAMERA_PROFILES[i].id) + "\"";
    payload += ",\"label\":\"" + jsonEscape(String(CAMERA_PROFILES[i].label)) + "\"";
    payload += ",\"ble_mac\":\"" + String(CAMERA_PROFILES[i].bleMac) + "\"";
    payload += ",\"ble_name\":\"" + jsonEscape(String(CAMERA_PROFILES[i].bleName)) + "\"";
    payload += ",\"wifi_ssid\":\"" + jsonEscape(String(CAMERA_PROFILES[i].wifiSsid)) + "\"";
    payload += ",\"selected\":" + String(cameraTargetBleMac.equalsIgnoreCase(CAMERA_PROFILES[i].bleMac) ? "true" : "false");
    payload += "}";
  }
  payload += "]";
  payload += ",\"recent_ble_devices\":" + buildBleRecentDevicesJson();
  payload += "}";
  return payload;
}

bool advertisedDeviceLooksLikeCamera(const String &mac, const String &name, const String &serviceUuid) {
  // An explicit profile is authoritative. Do not let the first other
  // GardePro advertisement silently replace it; that previously made E6+
  // jobs connect to the nearby original E6 and accept its empty gallery.
  if (!cameraTargetBleMac.isEmpty()) {
    return mac.equalsIgnoreCase(cameraTargetBleMac);
  }
  if (!name.isEmpty() && (name == CAMERA_BLE_NAME || name.startsWith(CAMERA_BLE_NAME_PREFIX))) {
    return true;
  }
#if CAMERA_DISCOVER_ANY_GARDEPRO
  if (!serviceUuid.isEmpty() && serviceUuid == CAMERA_BLE_SERVICE_UUID) {
    return true;
  }
#endif
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
  const bool radioConnected = (WiFi.status() == WL_CONNECTED) &&
                              (WiFi.localIP() != IPAddress((uint32_t)0));
  wifiConnected = radioConnected && WiFi.SSID() == cameraTargetWifiSsid;
  transferWifiConnected = radioConnected && WiFi.SSID() == TRANSFER_WIFI_SSID;
}

void powerDownTrailCameraWifiRadio(const char *reason) {
  if (streamSessionActive) {
    Serial.printf("[power] keeping trail camera WiFi active during stream reason=%s\n", reason);
    return;
  }
  if (wifiRadioMutex != nullptr &&
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.printf("[power] WiFi radio busy; deferring power-down reason=%s\n", reason);
    return;
  }
  closeBleWakeSession();
  if (wifiConnected || WiFi.status() == WL_CONNECTED) {
    Serial.printf("[power] disconnecting trail camera WiFi reason=%s ip=%s\n",
                  reason,
                  WiFi.localIP().toString().c_str());
    WiFi.disconnect(true, true);
    cooperativeDelay(150);
  }
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  transferWifiConnected = false;
  if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
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
      setCameraTargetFromAdvertisement(mac, name);
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

bool connectBleTargetAddressWithRetries() {
  bleLastConnectError = 0;
  bleConnectAttempts = 0;
  bleStage = "direct_connect";
  cooperativeDelay(BLE_SCAN_CONNECT_SETTLE_MS);

  const NimBLEAddress targetAddress(cameraTargetBleMac.c_str(), 0 /* public */);
  for (uint8_t attempt = 1; attempt <= BLE_DIRECT_CONNECT_ATTEMPTS; ++attempt) {
    bleConnectAttempts = attempt;
    if (bleClient != nullptr) {
      NimBLEDevice::deleteClient(bleClient);
      bleClient = nullptr;
    }
    Serial.printf("[BLE] direct connect attempt %u/%u address=%s\n",
                  static_cast<unsigned>(attempt),
                  static_cast<unsigned>(BLE_DIRECT_CONNECT_ATTEMPTS),
                  cameraTargetBleMac.c_str());
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(&bridgeBleClientCallbacks, false);
    bleClient->setConnectTimeout(BLE_DIRECT_CONNECT_TIMEOUT_MS);
    bleClient->setConnectionParams(24, 40, 0, 400, 160, 120);
    if (bleClient->connect(targetAddress, true, false, false)) {
      bleLastConnectError = 0;
      return true;
    }
    bleLastConnectError = bleClient->getLastError();
    Serial.printf("[BLE] direct connect attempt %u failed last_error=%d\n",
                  static_cast<unsigned>(attempt), bleLastConnectError);
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    cooperativeDelay(BLE_CONNECT_RETRY_DELAY_MS);
  }
  bleStage = "direct_connect_failed";
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

  Serial.printf("[BLE] scanning for camera target current=%s discover_any=%s\n",
                cameraTargetBleMac.c_str(),
                CAMERA_DISCOVER_ANY_GARDEPRO ? "yes" : "no");
  closeBleWakeSession();
  NimBLEDevice::deinit(true);
  bleInitialized = false;
  cooperativeDelay(100);
  NimBLEDevice::init(BOARD_HOSTNAME);
  bleInitialized = true;
  cooperativeDelay(BLE_INIT_WARMUP_MS);
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->stop();
  scan->clearResults();
  scan->setScanCallbacks(&bridgeBleScanCallbacks, true);
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);
  scan->setMaxResults(0);
  bleTargetDeviceFound = false;
  runBleDiscoveryPass(scan, true, BLE_SCAN_ATTEMPTS, BLE_SCAN_WINDOW_SEC, "active");
  if (!bleTargetDeviceFound) {
    runBleDiscoveryPass(scan, false, BLE_PASSIVE_SCAN_ATTEMPTS, BLE_PASSIVE_SCAN_WINDOW_SEC, "passive");
  }

  bool bleConnected = false;
  if (!bleTargetDeviceFound) {
    Serial.printf("[BLE] target advertisement not found results=%u best_mac=%s best_name=%s best_rssi=%d attempts=%u\n",
                  static_cast<unsigned>(bleScanResultCount),
                  bleBestSeenMac.c_str(),
                  bleBestSeenName.c_str(),
                  bleBestSeenRssi,
                  static_cast<unsigned>(bleScanAttemptCounter));
    // A powered E6+ may emit only a single advertisement over tens of
    // seconds.  Let the controller initiate directly to the selected public
    // address before declaring it absent.
    bleConnected = connectBleTargetAddressWithRetries();
    if (!bleConnected) {
      bleStage = "scan_not_found";
      return false;
    }
  }

  if (!bleConnected && !connectBleTargetWithRetries()) {
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
  Serial.println("  http_restart");
  Serial.println("  selftest");
  Serial.println("  http <path>");
  Serial.println("  httpm <METHOD> <path>");
  Serial.println("  rtsp_probe");
  Serial.println("  rtsp_live");
  Serial.println("  stream_start");
  Serial.println("  stream_stop");
  Serial.println("  battery");
  Serial.println("  diagnostics");
  Serial.println("  onboard_status");
  Serial.println("  onboard_capture");
  Serial.println("  onboard_delete_all");
  Serial.println("  onboard_config key=value [key=value...]");
  Serial.println("  onboard_timelapse hours=<value> [interval_ms=300000]");
  Serial.println("  onboard_timelapse_stop");
  Serial.println("  onboard_dump [fresh]");
  Serial.println("  sd_status");
  Serial.println("  sd_mount");
  Serial.println("  upload_status");
  Serial.println("  upload_requeue");
  Serial.println("  upload_telemetry");
  Serial.println("  upload_events");
  Serial.println("  upload_all");
  Serial.println("  camera_target [ble_mac] [wifi_ssid]");
  Serial.println("  rtsp <METHOD> <url>");
  Serial.println("  wake");
  Serial.println("  bleclose");
}

void connectCameraWifi() {
  if (wifiRadioMutex != nullptr &&
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[WiFi] radio busy; camera connect deferred");
    wifiConnected = false;
    return;
  }
  const unsigned long connectStartedMs = millis();
  Serial.printf("Connecting camera WiFi SSID %s\n", cameraTargetWifiSsid.c_str());
  WiFi.persistent(false);
  WiFi.scanDelete();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  cooperativeDelay(200);
  WiFi.mode(WIFI_MODE_STA);
  WiFi.setHostname(BOARD_HOSTNAME);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // The field camera sits near -90 dBm. Joining as an 802.11b-only station
  // makes the camera send to us with DSSS/CCK rates, which decode several dB
  // below the weakest OFDM rate. Fall back to b/g/n if it refuses b-only.
  for (uint8_t pass = 0; pass < 2 && WiFi.status() != WL_CONNECTED; ++pass) {
    const bool bOnly = pass == 0 && cameraWifiPrefer11b;
    if (pass == 1 && !cameraWifiPrefer11b) break;
    if (pass == 1) {
      WiFi.disconnect(false, true);
      cooperativeDelay(200);
    }
    esp_wifi_set_protocol(WIFI_IF_STA, bOnly ? WIFI_PROTOCOL_11B
                                             : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    cooperativeDelay(250);
    Serial.printf("[WiFi] camera join phy=%s\n", bOnly ? "11b" : "11bgn");
    WiFi.begin(cameraTargetWifiSsid.c_str(), CAMERA_WIFI_PASS);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      cooperativeDelay(100);
      Serial.print(".");
    }
    Serial.println();
    cameraWifiLast11b = bOnly && WiFi.status() == WL_CONNECTED;
    if (pass == 1 && WiFi.status() == WL_CONNECTED) {
      // This camera rejects b-only stations; stop paying the extra timeout.
      Serial.println("[WiFi] camera refused 11b-only join; using 11bgn from now on");
      cameraWifiPrefer11b = false;
    }
  }

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
  if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
}

bool recoverActiveStream(const char *reason) {
  if (millis() - lastStreamRecoveryMs < STREAM_RECOVERY_COOLDOWN_MS) {
    return false;
  }
  lastStreamRecoveryMs = millis();
  lastStreamRecoveryStartedMs = lastStreamRecoveryMs;
  lastStreamRecoveryFinishedMs = 0;
  lastStreamRecoveryDurationMs = 0;
  lastStreamRecoveryReason = reason;
  lastStreamRecoverySucceeded = false;
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
    lastStreamRecoveryFinishedMs = millis();
    lastStreamRecoveryDurationMs = lastStreamRecoveryFinishedMs - lastStreamRecoveryStartedMs;
    return false;
  }

  const bool restarted = startStreamSession();
  lastStreamRecoverySucceeded = restarted;
  lastStreamRecoveryFinishedMs = millis();
  lastStreamRecoveryDurationMs = lastStreamRecoveryFinishedMs - lastStreamRecoveryStartedMs;
  Serial.printf("[stream] recovery restart=%s\n", restarted ? "ok" : "failed");
  return restarted;
}

bool recoverIdleWifi(const char *reason) {
  if (isControlActionActive()) {
    Serial.printf("[idle] skipping wifi recovery during control action reason=%s\n", reason);
    return false;
  }
  if (durableMediaJobIsActive()) {
    Serial.printf("[idle] skipping wifi recovery during durable media job reason=%s\n", reason);
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  const int count = WiFi.scanNetworks();
  bool foundTarget = false;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid == cameraTargetWifiSsid) {
      foundTarget = true;
      Serial.printf("Target SSID %s visible RSSI=%d channel=%d\n",
                    ssid.c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i));
    }
  }
  if (!foundTarget) {
    Serial.printf("Target SSID %s still not visible\n", cameraTargetWifiSsid.c_str());
  }
}

bool waitForCameraWifiPresence(unsigned long timeoutMs, unsigned long intervalMs) {
  const unsigned long start = millis();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  while (millis() - start < timeoutMs) {
    const unsigned long elapsedMs = millis() - start;
    const int count = WiFi.scanNetworks();
    for (int i = 0; i < count; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid == cameraTargetWifiSsid) {
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
                  cameraTargetWifiSsid.c_str(),
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
  portENTER_CRITICAL(&halowConnectMux);
  if (halowConnectInProgress) {
    portEXIT_CRITICAL(&halowConnectMux);
    return;
  }
  halowConnectInProgress = true;
  portEXIT_CRITICAL(&halowConnectMux);
  halowLastRecoveryAttemptMs = millis();
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
  while (HaLow.status() != WL_CONNECTED && millis() - start < HALOW_CONNECT_WAIT_MS) {
    cooperativeDelay(100);
    Serial.print("#");
  }
  Serial.println();

  halowConnected = (HaLow.status() == WL_CONNECTED);
  if (halowConnected) {
    halowRecoveryIntervalMs = HALOW_RECOVERY_INTERVAL_MS;
    halowDownSinceMs = 0;
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
  portENTER_CRITICAL(&halowConnectMux);
  halowConnectInProgress = false;
  portEXIT_CRITICAL(&halowConnectMux);
}

bool probeHaLowDataPlane(int &errorCode) {
  errorCode = 0;
  const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    errorCode = errno;
    return false;
  }
  // SO_BINDTODEVICE is unusable on this lwIP build (netif name lookup fails),
  // which made every probe report ENODEV and forced a HaLow reconnect every
  // minute. Source-address binding selects the HaLow netif, as for uploads.
  if (!bindSocketToLocalAddress(sock, HaLow.localIP())) {
    errorCode = ENODEV;
    close(sock);
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = static_cast<uint32_t>(HaLow.gatewayIP());
  address.sin_port = htons(UPSTREAM_API_PORT);
  const bool connected = connectSocketWithTimeout(sock, address, 2000UL, errorCode);
  close(sock);
  // A refused TCP connection still proves that ARP, routing, and the remote
  // network stack are alive; timeouts and unreachable errors do not.
  return connected || errorCode == ECONNREFUSED;
}

void forceHaLowReconnect(const char *reason) {
  ++halowForcedReconnects;
  Serial.printf("[HaLow] forcing reconnect reason=%s count=%u status=%d\n",
                reason, halowForcedReconnects, static_cast<int>(HaLow.status()));
  halowConnected = false;
  mmwlan_sta_disable();
  cooperativeDelay(500);
  HaLow._setStatus(WL_DISCONNECTED);
  connectHaLow();
}

void serviceHaLowGratuitousArp() {
  if (halowLastGratuitousArpMs != 0 &&
      millis() - halowLastGratuitousArpMs < HALOW_GRATUITOUS_ARP_INTERVAL_MS) return;
  halowLastGratuitousArpMs = millis();
  struct netif *interface = HaLow.netif();
  if (interface == nullptr || ip4_addr_isany_val(*netif_ip4_addr(interface))) {
    ++halowGratuitousArpFailures;
    return;
  }
  const err_t result = etharp_gratuitous(interface);
  if (result == ERR_OK) ++halowGratuitousArpCount;
  else ++halowGratuitousArpFailures;
}

void serviceHaLowRecovery() {
  // HaLow driver reconnects are comparatively expensive and have previously
  // overlapped BLE/GATT discovery, starving the control worker and breaking
  // an otherwise valid camera wake. Defer them until camera bring-up exits.
  if (otaInProgress || otaRestartPending || halowConnectInProgress ||
      cameraBringupInProgress) return;
  if (HaLow.status() == WL_CONNECTED) {
    halowConnected = true;
    halowDownSinceMs = 0;
    serviceHaLowGratuitousArp();
    if (halowLastLivenessProbeMs == 0 ||
        millis() - halowLastLivenessProbeMs >= HALOW_LIVENESS_INTERVAL_MS) {
      halowLastLivenessProbeMs = millis();
      int errorCode = 0;
      if (probeHaLowDataPlane(errorCode)) {
        ++halowLivenessProbeSuccesses;
        halowConsecutiveLivenessFailures = 0;
        halowLastLivenessError = 0;
      } else {
        ++halowLivenessProbeFailures;
        ++halowConsecutiveLivenessFailures;
        halowLastLivenessError = errorCode;
        Serial.printf("[HaLow] liveness failed consecutive=%u total=%u error=%d\n",
                      halowConsecutiveLivenessFailures,
                      halowLivenessProbeFailures,
                      errorCode);
        if (halowConsecutiveLivenessFailures >= HALOW_LIVENESS_FAILURE_THRESHOLD) {
          halowConsecutiveLivenessFailures = 0;
          forceHaLowReconnect("data_plane_unreachable");
        }
      }
    }
    return;
  }
  halowConnected = false;
  if (halowDownSinceMs == 0) halowDownSinceMs = millis();
  if (halowConnectInProgress ||
      (halowLastRecoveryAttemptMs != 0 &&
       millis() - halowLastRecoveryAttemptMs < halowRecoveryIntervalMs)) return;
  ++halowRecoveryAttempts;
  Serial.printf("[HaLow] recovery attempt=%u camera_wifi=%s durable_job=%s\n",
                halowRecoveryAttempts,
                wifiConnected ? "up" : "down",
                durableMediaJobIsActive() ? "active" : "idle");
  connectHaLow();
  if (halowConnected) {
    halowRecoveryIntervalMs = HALOW_RECOVERY_INTERVAL_MS;
    ++halowRecoverySuccesses;
    Serial.printf("[HaLow] recovery succeeded count=%u ip=%s\n",
                  halowRecoverySuccesses,
                  HaLow.localIP().toString().c_str());
  } else {
    halowRecoveryIntervalMs = std::min(halowRecoveryIntervalMs * 2UL,
                                       HALOW_RECOVERY_MAX_INTERVAL_MS);
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
                   int &statusCode,
                   UpstreamTransport requestedTransport = UPSTREAM_TRANSPORT_AUTO) {
  UpstreamTransport transport = requestedTransport;
  if (transport == UPSTREAM_TRANSPORT_AUTO) {
    // Management traffic remains on HaLow even while bulk media uses 2.4 GHz.
    // This avoids default-route ambiguity when both interfaces share a subnet.
    transport = (halowConnected && HaLow.status() == WL_CONNECTED)
                  ? UPSTREAM_TRANSPORT_HALOW
                  : UPSTREAM_TRANSPORT_TRANSFER_WIFI;
  }
  const bool transportReady = transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI
                                ? transferWifiConnected
                                : (halowConnected && HaLow.status() == WL_CONNECTED);
  if (!transportReady) {
    statusCode = 0;
    responseBody = "{\"error\":\"requested_transport_down\"}";
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
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &receiveTimeout, sizeof(receiveTimeout));
  if (!bindSocketToTransport(sock, transport)) {
    Serial.printf("[upload] transport bind failed transport=%s errno=%d\n",
                  transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI ? "wifi_2_4ghz" : "halow",
                  errno);
    close(sock);
    statusCode = 0;
    responseBody = "{\"error\":\"transport_bind_failed\"}";
    return false;
  }

  sockaddr_in upstreamAddr{};
  upstreamAddr.sin_family = AF_INET;
  upstreamAddr.sin_addr.s_addr = inet_addr(apiHost);
  upstreamAddr.sin_port = htons(apiPort);

  int connectError = 0;
  if (!connectSocketWithTimeout(sock, upstreamAddr, 10000UL, connectError)) {
    Serial.printf("[upload] connect failed transport=%s host=%s:%u error=%d\n",
                  transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI ? "wifi_2_4ghz" : "halow",
                  apiHost, apiPort, connectError);
    close(sock);
    statusCode = 0;
    responseBody = "{\"error\":\"api_connect_failed_" + String(connectError) + "\"}";
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
                        int &statusCode,
                        UpstreamTransport transport = UPSTREAM_TRANSPORT_AUTO) {
  return postJsonToApi(UPSTREAM_API_HOST,
                       UPSTREAM_API_PORT,
                       normalizeApiPath(path),
                       requestBody,
                       responseBody,
                       statusCode,
                       transport);
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
    if (observationUploadMutex != nullptr) {
      xSemaphoreTake(observationUploadMutex, portMAX_DELAY);
    }
    String queueSummary;
    const bool queueOk = replayQueuedObservationBatches(4, queueSummary);
    bool wifiOk = true;
    bool bleOk = true;
    if (scannerScheduleEnabled) {
      String wifiResponse;
      int wifiStatus = 0;
      wifiOk = uploadWifiObservationsNow(wifiResponse, wifiStatus);
      String bleResponse;
      int bleStatus = 0;
      bleOk = uploadBleObservationsNow(bleResponse, bleStatus);
    }
    if (observationUploadMutex != nullptr) {
      xSemaphoreGive(observationUploadMutex);
    }
    const unsigned long waitMs = scannerScheduleEnabled
                                   ? ((queueOk && wifiOk && bleOk) ? currentWifiScannerIntervalMs() : 30000UL)
                                   : 60000UL;
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
}

void startWifiScannerTask() {
  scannerScheduleEnabled = false;
  wifiScannerLastError = "disabled_camera_priority";
  bleScannerLastError = "disabled_camera_priority";
  Serial.println("[scanner] RF scanner task disabled in camera-priority firmware");
  return;
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
  // Trail cameras can pause for several seconds between HTTP chunks while
  // reading their own card. Stream's one-second default timeout turned that
  // normal pause into an empty chunk-size line and aborted otherwise healthy
  // downloads. The byte loops below still enforce their own 15-second stall
  // limit, so this remains bounded.
  client.setTimeout(5000UL);

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
      powerDownTrailCameraWifiRadio("camera_standby_request");
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

bool requestCameraStandbyAndPowerDownWifi(const char *reason) {
  if (streamSessionActive) {
    Serial.printf("[power] not requesting standby during active stream reason=%s\n", reason);
    return false;
  }
  refreshWifiState();
  bool standbyOk = false;
  if (wifiConnected) {
    String body;
    int statusCode = 500;
    const bool requestOk = proxyCameraRequest("GET", "/cmd/standby/now", body, statusCode);
    standbyOk = requestOk && statusCode >= 200 && statusCode < 300;
    Serial.printf("[power] standby request reason=%s ok=%s status=%d\n",
                  reason,
                  standbyOk ? "yes" : "no",
                  statusCode);
  }
  powerDownTrailCameraWifiRadio(reason);
  return standbyOk;
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
    cameraSessionLeaseExpiredStandbySent = true;
    Serial.println("[session] lease expired, requesting standby");
    requestCameraStandbyAndPowerDownWifi("session_lease_expired");
  } else if (!streamSessionActive) {
    powerDownTrailCameraWifiRadio("session_lease_expired_no_standby");
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
  chunkLine.trim();
  if (chunkLine.isEmpty() || chunkLine.length() > 8) {
    return -1;
  }
  // strtol() returns 0 for non-hex text, which previously turned a garbled
  // header into a clean end-of-body and could stage a truncated file.
  char *end = nullptr;
  const long size = strtol(chunkLine.c_str(), &end, 16);
  if (end == nullptr || *end != '\0' || size < 0) {
    return -1;
  }
  return size;
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

  // Establish the HaLow tunnel before PLAY so the first SPS/PPS/IDR packets
  // can be queued immediately instead of being lost during TCP connection.
  lastStreamStartStage = "tunnel_connect_before_play";
  if (halowConnected && !connectTunnelSocket()) {
    Serial.println("[stream] pre-PLAY tunnel connect failed; continuing local-only");
    lastStreamStartMessage = "camera_stream_active_tunnel_connect_failed";
  } else if (!halowConnected) {
    lastStreamStartMessage = "camera_stream_active_tunnel_halow_down";
  }

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
    // Packet timestamps are updated only by the UDP forwarders.  Do not mark
    // RTP as flowing merely because PLAY returned 200.
    lastPrimaryPacketMs = 0;
    lastSecondaryPacketMs = 0;
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
  ++streamSessionId;
  if (streamSessionId == 0) ++streamSessionId;
  resetRtpTelemetry();
  streamForwardingEnabled = false;
  closeTunnelSocket();
  if (streamPacketQueue != nullptr) xQueueReset(streamPacketQueue);
  if (!wifiConnected) {
    Serial.println("[stream] camera WiFi is down, running bringup");
    lastStreamStartStage = "camera_bringup";
    lastStreamStartMessage = "stream_camera_bringup";
    if (!runBringupSequence()) {
      lastStreamStartStage = "camera_wifi_down";
      lastStreamStartMessage = "stream_camera_wifi_down";
      lastStreamStartElapsedMs = millis() - startedMs;
      return false;
    }
  }

  RtspSessionInfo info{};
  if (!runRtspLiveSequence(info)) {
    Serial.println("[stream] RTSP live sequence failed");
    if (lastStreamStartMessage.isEmpty()) {
      lastStreamStartMessage = "stream_rtsp_failed";
    }
    lastStreamStartElapsedMs = millis() - startedMs;
    streamForwardingEnabled = false;
    closeTunnelSocket();
    return false;
  }

  const bool tunnelOk = getTunnelSocketSnapshot() >= 0;
  if (!tunnelOk) lastStreamStartStage = "camera_stream_local";

  startHttpServer();
  streamSessionActive = true;
  streamSessionStartedMs = millis();
  lastStreamStallMs = 0;
  if (tunnelOk) {
    lastStreamStartStage = "stream_active";
    lastStreamStartMessage = "stream_active";
  }
  lastStreamStartElapsedMs = millis() - startedMs;
  Serial.printf("[stream] camera session active started_ms=%lu tunnel=%s\n",
                streamSessionStartedMs,
                tunnelOk ? "connected" : "local_only");
  return true;
}

bool runTriggerPhotoJob() {
  const bool keepCameraSession = cameraSessionLeaseActive;
  if (!wifiConnected) {
    Serial.println("[job] trigger_photo requires camera bringup");
    if (!runBringupSequence()) {
      lastStreamStartMessage = "trigger_photo_bringup_failed";
      if (!keepCameraSession) {
        powerDownTrailCameraWifiRadio("trigger_photo_bringup_failed");
      }
      return false;
    }
  }

  String body;
  int statusCode = 0;
  if (!proxyCameraRequest("GET", "/media/pic/take", body, statusCode) ||
      statusCode < 200 || statusCode >= 300) {
    Serial.printf("[job] trigger_photo take failed status=%d body=%s\n", statusCode, body.c_str());
    lastStreamStartMessage = "trigger_photo_take_failed";
    if (!keepCameraSession) {
      requestCameraStandbyAndPowerDownWifi("trigger_photo_take_failed");
    }
    return false;
  }

  for (int attempt = 1; attempt <= 12; ++attempt) {
    String resultBody;
    int resultStatus = 0;
    if (proxyCameraRequest("GET", "/media/pic/result", resultBody, resultStatus) &&
        resultStatus >= 200 && resultStatus < 300) {
      Serial.printf("[job] trigger_photo result attempt=%d status=%d body=%s\n",
                    attempt,
                    resultStatus,
                    resultBody.c_str());
      lastStreamStartMessage = "trigger_photo_complete";
      if (!keepCameraSession) {
        requestCameraStandbyAndPowerDownWifi("trigger_photo_complete");
      }
      return true;
    }
    cooperativeDelay(2000);
  }

  lastStreamStartMessage = "trigger_photo_result_timeout";
  if (!keepCameraSession) {
    requestCameraStandbyAndPowerDownWifi("trigger_photo_result_timeout");
  }
  return false;
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
  const uint32_t bringupBatteryMv = readBatteryMilliVolts(8);
  if (bringupBatteryMv < CAMERA_BRINGUP_MIN_BATTERY_MV) {
    bleStage = "low_battery";
    bleWakeConfirmed = false;
    Serial.printf("[bringup] refused: battery=%u mV minimum=%u mV\n",
                  bringupBatteryMv, CAMERA_BRINGUP_MIN_BATTERY_MV);
    return false;
  }
  if (wifiRadioMutex != nullptr &&
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[bringup] WiFi radio busy");
    return false;
  }
  cameraBringupInProgress = true;
  const unsigned long bringupStartedMs = millis();
  refreshWifiState();
  if (wifiConnected) {
    lastBringupElapsedMs = millis() - bringupStartedMs;
    Serial.println("[bringup] camera WiFi already connected");
    cameraBringupInProgress = false;
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
    return true;
  }

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  cooperativeDelay(250);
  refreshWifiState();
  bleWakeAttempted = true;
  // Preserve the known-good radio sequence: initialize and scan BLE before
  // running any 2.4 GHz Wi-Fi scan. A hotspot pre-scan here was added with the
  // storage/recovery work and could leave coexistence favoring Wi-Fi while the
  // following NimBLE scan received no advertisements at all.
  bool hotspotVisible = false;

  if (!hotspotVisible && tryExistingBleWakeSession()) {
    // The wake token has been delivered; BLE is no longer needed. Release the
    // shared 2.4 GHz controller before scanning for or joining the camera AP.
    Serial.println("[BLE] wake sent; closing cached session before WiFi");
    closeBleWakeSession();
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
    // Match the camera application's sequential radio lifecycle: BLE wake,
    // BLE shutdown, then WiFi discovery/association. Keeping GATT connected
    // here adds power draw and radio-coexistence pressure without helping the
    // already-issued wake command.
    Serial.println("[BLE] wake transaction complete; closing session before WiFi");
    closeBleWakeSession();
    const unsigned long hotspotStartedMs = millis();
    const unsigned long hotspotWaitMs = bleWakeConfirmed
                                          ? BRINGUP_HOTSPOT_WAIT_MS
                                          : BRINGUP_FAILED_WAKE_HOTSPOT_WAIT_MS;
    hotspotVisible = waitForCameraWifiPresence(hotspotWaitMs,
                                               BRINGUP_HOTSPOT_POLL_MS);
    lastHotspotWaitElapsedMs = millis() - hotspotStartedMs;
    Serial.printf("[WiFi] hotspot visibility after BLE wake: %s\n", hotspotVisible ? "yes" : "no");
    if (hotspotVisible && !bleWakeConfirmed) {
      bleWakeConfirmed = true;
      bleStage = "wake_hotspot_visible";
      Serial.println("[BLE] wake confirmed by camera hotspot visibility");
    }
    if (!bleWakeConfirmed && !hotspotVisible) {
      Serial.println("[bringup] aborting after BLE wake failure and hotspot absence");
      refreshWifiState();
      lastBringupElapsedMs = millis() - bringupStartedMs;
      cameraBringupInProgress = false;
      if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
      return false;
    }
  }
  if (!hotspotVisible) {
    Serial.println("[bringup] aborting because camera hotspot did not appear");
    refreshWifiState();
    lastBringupElapsedMs = millis() - bringupStartedMs;
    cameraBringupInProgress = false;
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
    return false;
  }
  connectCameraWifi();
  refreshWifiState();
  lastBringupElapsedMs = millis() - bringupStartedMs;
  const bool connected = wifiConnected;
  cameraBringupInProgress = false;
  if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
  return connected;
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

String buildSystemStatusJson() {
  String payload = "{";
  payload += "\"uptime_ms\":" + String(millis());
  payload += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  payload += ",\"boot_count\":" + String(persistentBootCount);
  payload += ",\"boot_session_id\":" + String(bootSessionId);
  payload += ",\"psram_found\":" + String(psramFound() ? "true" : "false");
  payload += ",\"psram_size\":" + String(ESP.getPsramSize());
  payload += ",\"psram_free\":" + String(ESP.getFreePsram());
  payload += ",\"chip_temperature_c\":" + String(readChipTemperatureC(), 1);
  payload += ",\"http_service\":" + buildHttpServiceJson();
  payload += ",\"diagnostics\":" + buildDiagnosticStatusJson();
  payload += "}";
  return payload;
}

String buildWifiStatusJson() {
  String payload = "{";
  payload += "\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  payload += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
  payload += ",\"scheduled_scans_enabled\":" + String(scannerScheduleEnabled ? "true" : "false");
  payload += ",\"wifi_scan_busy\":" + String(wifiScanBusy ? "true" : "false");
  payload += ",\"wifi_scan_last_count\":" + String(wifiScanLastCount);
  payload += ",\"wifi_scan_last_age_ms\":" + String(msSince(wifiScanLastMs));
  payload += ",\"scanner\":{";
  payload += "\"runs\":" + String(wifiScannerRunCount);
  payload += ",\"upload_successes\":" + String(wifiScannerUploadSuccessCount);
  payload += ",\"upload_failures\":" + String(wifiScannerUploadFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(wifiScannerLastError) + "\"";
  payload += ",\"last_run_age_ms\":" + String(msSince(wifiScannerLastRunMs));
  payload += ",\"last_upload_age_ms\":" + String(msSince(wifiScannerLastUploadMs));
  payload += "}";
  payload += "}";
  return payload;
}

String buildCameraBridgeStatusJson() {
  String payload = "{";
  payload += "\"camera_ip\":\"" + CAMERA_IP.toString() + "\"";
  payload += ",\"camera_target_ble_mac\":\"" + jsonEscape(cameraTargetBleMac) + "\"";
  payload += ",\"camera_target_ble_name\":\"" + jsonEscape(cameraTargetBleName) + "\"";
  payload += ",\"camera_target_wifi_ssid\":\"" + jsonEscape(cameraTargetWifiSsid) + "\"";
  payload += ",\"camera_wifi_ever_connected\":" + String(cameraWifiEverConnected ? "true" : "false");
  payload += ",\"standby_requested\":" + String(standbyRequested ? "true" : "false");
  payload += ",\"session\":" + buildCameraSessionJson();
  payload += "}";
  return payload;
}

String buildStreamRuntimeStatusJson() {
  const bool tunnelActive = streamSessionActive && getTunnelSocketSnapshot() >= 0;
  String payload = "{";
  payload += "\"stream_status\":" + buildStreamStatusJson();
  payload += ",\"stream_active\":" + String(streamSessionActive ? "true" : "false");
  payload += ",\"tunnel_connected\":" + String(tunnelActive ? "true" : "false");
  payload += ",\"recoveries\":" + String(streamRecoveryAttempts);
  payload += ",\"idle_recoveries\":" + String(idleRecoveryAttempts);
  payload += ",\"http_keepalive_failures\":" + String(httpKeepaliveFailures);
  payload += ",\"idle_recovery_last_ms\":" + String(msSince(lastIdleRecoveryMs));
  payload += ",\"media_primary_packets\":" + String(mediaPrimaryPackets);
  payload += ",\"media_primary_bytes\":" + String(mediaPrimaryBytes);
  payload += ",\"media_secondary_packets\":" + String(mediaSecondaryPackets);
  payload += ",\"media_secondary_bytes\":" + String(mediaSecondaryBytes);
  payload += "}";
  return payload;
}

String buildBleStatusJson() {
  String payload = "{";
  payload += "\"scheduled_scans_enabled\":" + String(scannerScheduleEnabled ? "true" : "false");
  payload += ",\"camera_target_ble_mac\":\"" + jsonEscape(cameraTargetBleMac) + "\"";
  payload += ",\"camera_target_ble_name\":\"" + jsonEscape(cameraTargetBleName) + "\"";
  payload += ",\"camera_target_wifi_ssid\":\"" + jsonEscape(cameraTargetWifiSsid) + "\"";
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
  payload += ",\"scanner\":{";
  payload += "\"runs\":" + String(bleScannerRunCount);
  payload += ",\"last_count\":" + String(bleScannerLastCount);
  payload += ",\"last_manufacturer_count\":" + String(bleScannerLastManufacturerCount);
  payload += ",\"last_services_count\":" + String(bleScannerLastServicesCount);
  payload += ",\"last_service_data_count\":" + String(bleScannerLastServiceDataCount);
  payload += ",\"last_tx_power_count\":" + String(bleScannerLastTxPowerCount);
  payload += ",\"last_name_count\":" + String(bleScannerLastNameCount);
  payload += ",\"upload_successes\":" + String(bleScannerUploadSuccessCount);
  payload += ",\"upload_failures\":" + String(bleScannerUploadFailureCount);
  payload += ",\"last_error\":\"" + jsonEscape(bleScannerLastError) + "\"";
  payload += ",\"last_run_age_ms\":" + String(msSince(bleScannerLastRunMs));
  payload += ",\"last_upload_age_ms\":" + String(msSince(bleScannerLastUploadMs));
  payload += "}";
  payload += "}";
  return payload;
}

String buildControlStatusJson() {
  ControlState controlSnapshot{};
  snapshotControlState(controlSnapshot);
  const String state = controlStateFromSnapshot(controlSnapshot);
  const String progress = controlProgressFromSnapshot(controlSnapshot);
  String payload = "{";
  payload += "\"control_busy\":" + String(controlSnapshot.busy ? "true" : "false");
  payload += ",\"control_state\":\"" + state + "\"";
  payload += ",\"control_pending\":" + controlActionJson(controlSnapshot.pendingAction);
  payload += ",\"control_action\":" + controlActionJson(controlSnapshot.activeAction);
  payload += ",\"control_last_action\":" + controlActionJson(controlSnapshot.lastAction);
  payload += ",\"control_last_ok\":" + String(controlSnapshot.lastOk ? "true" : "false");
  payload += ",\"control_error\":" + jsonNullableString(String(controlSnapshot.lastError));
  payload += ",\"control_message\":" + jsonNullableString(String(controlSnapshot.lastMessage));
  payload += ",\"control_progress\":\"" + jsonEscape(progress) + "\"";
  payload += ",\"control_progress_text\":\"" + jsonEscape(controlProgressText(progress)) + "\"";
  payload += ",\"control_active_ms\":" + String(msSince(controlSnapshot.activeSinceMs));
  payload += ",\"control_last_finished_ms\":" + String(msSince(controlSnapshot.lastFinishedMs));
  payload += ",\"control_last_message\":" + jsonNullableString(String(controlSnapshot.lastMessage));
  payload += "}";
  return payload;
}

String buildControlSummaryJson() {
  ControlState controlSnapshot{};
  snapshotControlState(controlSnapshot);
  const String state = controlStateFromSnapshot(controlSnapshot);
  const String progress = controlProgressFromSnapshot(controlSnapshot);
  const bool tunnelActive = streamSessionActive && getTunnelSocketSnapshot() >= 0;
  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"control_busy\":" + String(controlSnapshot.busy ? "true" : "false");
  payload += ",\"control_state\":\"" + state + "\"";
  payload += ",\"control_action\":" + controlActionJson(controlSnapshot.activeAction);
  payload += ",\"control_pending\":" + controlActionJson(controlSnapshot.pendingAction);
  payload += ",\"control_last_action\":" + controlActionJson(controlSnapshot.lastAction);
  payload += ",\"control_last_ok\":" + String(controlSnapshot.lastOk ? "true" : "false");
  payload += ",\"control_error\":" + jsonNullableString(String(controlSnapshot.lastError));
  payload += ",\"control_message\":" + jsonNullableString(String(controlSnapshot.lastMessage));
  payload += ",\"control_progress\":\"" + jsonEscape(progress) + "\"";
  payload += ",\"control_progress_text\":\"" + jsonEscape(controlProgressText(progress)) + "\"";
  payload += ",\"control_active_ms\":" + String(msSince(controlSnapshot.activeSinceMs));
  payload += ",\"control_last_finished_ms\":" + String(msSince(controlSnapshot.lastFinishedMs));
  payload += ",\"eta_sec\":" + String(controlSnapshot.busy ? controlActionEtaSec(controlSnapshot.activeAction) : 0);
  payload += ",\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  payload += ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\"";
  payload += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"stream_active\":" + String(streamSessionActive ? "true" : "false");
  payload += ",\"live_view_active\":" + String(streamSessionActive ? "true" : "false");
  payload += ",\"tunnel_connected\":" + String(tunnelActive ? "true" : "false");
  payload += ",\"ble_stage\":\"" + jsonEscape(bleStage) + "\"";
  payload += ",\"durable_media_job\":" + buildDurableMediaJobJson();
  payload += "}";
  return payload;
}

void handleStatus() {
  ControlState controlSnapshot{};
  snapshotControlState(controlSnapshot);
  const String controlStateValue = controlStateFromSnapshot(controlSnapshot);
  const String controlProgress = controlProgressFromSnapshot(controlSnapshot);
  const bool tunnelActive = streamSessionActive && getTunnelSocketSnapshot() >= 0;
  String basic = "{";
  basic += "\"uptime_ms\":" + String(millis());
  basic += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  basic += ",\"boot_count\":" + String(persistentBootCount);
  basic += ",\"boot_session_id\":" + String(bootSessionId);
  basic += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  basic += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  basic += ",\"halow_rssi\":" + String(halowConnected ? HaLow.RSSI() : 0);
  basic += ",\"wifi_connected\":" + String(wifiConnected ? "true" : "false");
  basic += ",\"stream_active\":" + String(streamSessionActive ? "true" : "false");
  basic += ",\"tunnel_connected\":" + String(tunnelActive ? "true" : "false");
  basic += ",\"camera_target_wifi_ssid\":\"" + jsonEscape(cameraTargetWifiSsid) + "\"";
  basic += ",\"camera_target_ble_mac\":\"" + jsonEscape(cameraTargetBleMac) + "\"";
  basic += ",\"control_busy\":" + String(controlSnapshot.busy ? "true" : "false");
  basic += ",\"control_state\":\"" + controlStateValue + "\"";
  basic += ",\"control_action\":" + controlActionJson(controlSnapshot.activeAction);
  basic += ",\"control_pending\":" + controlActionJson(controlSnapshot.pendingAction);
  basic += ",\"control_last_action\":" + controlActionJson(controlSnapshot.lastAction);
  basic += ",\"control_last_ok\":" + String(controlSnapshot.lastOk ? "true" : "false");
  basic += ",\"control_error\":" + jsonNullableString(String(controlSnapshot.lastError));
  basic += ",\"control_message\":" + jsonNullableString(String(controlSnapshot.lastMessage));
  basic += ",\"control_progress\":\"" + jsonEscape(controlProgress) + "\"";
  basic += ",\"control_progress_text\":\"" + jsonEscape(controlProgressText(controlProgress)) + "\"";
  basic += ",\"control_last_message\":" + jsonNullableString(String(controlSnapshot.lastMessage));
  basic += ",\"ble_stage\":\"" + jsonEscape(bleStage) + "\"";
  basic += ",\"control\":";
  basic += buildControlSummaryJson();
  basic += ",\"clock_valid\":" + String(onboardClockValid() ? "true" : "false");
  basic += ",\"storage_type\":\"" + String(persistentStorageType()) + "\"";
  basic += ",\"storage_ready\":" + String(onboardStorageReady ? "true" : "false");
  basic += ",\"sd_ready\":" + String(sdReady ? "true" : "false");
  basic += ",\"scanner_schedule_enabled\":" + String(scannerScheduleEnabled ? "true" : "false");
  basic += ",\"stored_photo_count\":" + String(onboardStoredPhotoCount);
  basic += ",\"latest_media_id\":" + (onboardLatestMediaId == 0 ? String("null") : "\"" + onboardMediaIdString(onboardLatestMediaId) + "\"");
  basic += ",\"onboard_captures\":" + String(onboardCaptureCount);
  basic += ",\"schedule_mode\":\"" + String(onboardClockValid() ? "clock_window" : "uptime_fallback") + "\"";
  basic += ",\"http_service\":" + buildHttpServiceJson();
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
}

void handleBatteryStatus() {
  server.send(200, "application/json", buildBatteryJson());
}

void handleHealthz() {
  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"uptime_ms\":" + String(millis());
  payload += ",\"hostname\":\"" + jsonEscape(BOARD_HOSTNAME) + "\"";
  payload += ",\"halow_connected\":" + String(halowConnected ? "true" : "false");
  payload += ",\"halow_ip\":\"" + HaLow.localIP().toString() + "\"";
  payload += ",\"http_service\":" + buildHttpServiceJson();
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleHttpRestart() {
  server.send(202, "application/json", "{\"ok\":true,\"action\":\"http_restart\"}");
  restartHttpServer("api_request");
}

void handleSystemStatus() {
  server.send(200, "application/json", buildSystemStatusJson());
}

void handleDiagnosticStatus() {
  server.send(200, "application/json", buildDiagnosticStatusJson());
}

void handleRecoveryResume() {
  automaticRecoverySuspended = false;
  failsafeConsecutiveSameReason = 0;
  Preferences prefs;
  if (prefs.begin("runtime", false)) {
    prefs.putUInt("failsafe_same", 0);
    prefs.end();
  }
  server.send(200, "application/json",
              "{\"ok\":true,\"automatic_recovery_suspended\":false}");
}

void handleDiagnosticLog() {
  if (!sdReady || cameraSdTransferActive) {
    server.send(503, "application/json", "{\"error\":\"sd_unavailable\"}");
    return;
  }
  const char *path = (server.hasArg("previous") && server.arg("previous") == "1")
                       ? "/diagnostics/health.previous.jsonl"
                       : DIAGNOSTIC_LOG_PATH;
  File file = SD.open(path, FILE_READ);
  if (!file) {
    server.send(404, "application/json", "{\"error\":\"diagnostic_log_not_found\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(file, "application/x-ndjson");
  file.close();
}

void handleHaLowStatusHttp() {
  server.send(200, "application/json", buildHaLowStatusJson());
}

void handleWifiStatus() {
  server.send(200, "application/json", buildWifiStatusJson());
}

void handleTransferWifiTest() {
  if (isControlActionActive() || streamSessionActive || trailMediaUploadActive || wifiConnected) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"radio_busy\"}");
    return;
  }
  const unsigned long startedMs = millis();
  const bool associated = ensureTransferWifiConnected();
  const String ip = associated ? WiFi.localIP().toString() : String("0.0.0.0");
  const String gateway = associated ? WiFi.gatewayIP().toString() : String("0.0.0.0");
  const int rssi = associated ? WiFi.RSSI() : 0;
  const int channel = associated ? WiFi.channel() : 0;
  bool serverReachable = false;
  int connectError = 0;
  if (associated) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock >= 0 && bindSocketToTransferWifi(sock)) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = inet_addr(UPSTREAM_API_HOST);
      address.sin_port = htons(UPSTREAM_API_PORT);
      serverReachable = connectSocketWithTimeout(sock, address, 5000UL, connectError);
    } else {
      connectError = errno == 0 ? ENODEV : errno;
    }
    if (sock >= 0) close(sock);
  }
  disconnectTransferWifi();
  String payload = "{";
  payload += "\"ok\":" + String((associated && serverReachable) ? "true" : "false");
  payload += ",\"ssid_configured\":" + String(strlen(TRANSFER_WIFI_SSID) > 0 ? "true" : "false");
  payload += ",\"associated\":" + String(associated ? "true" : "false");
  payload += ",\"ip\":\"" + ip + "\"";
  payload += ",\"gateway\":\"" + gateway + "\"";
  payload += ",\"rssi\":" + String(rssi);
  payload += ",\"channel\":" + String(channel);
  payload += ",\"server_reachable_over_wifi\":" + String(serverReachable ? "true" : "false");
  payload += ",\"server_connect_error\":" + String(connectError);
  payload += ",\"elapsed_ms\":" + String(millis() - startedMs);
  payload += ",\"disconnected_after_test\":true";
  payload += "}";
  server.send(associated && serverReachable ? 200 : 503, "application/json", payload);
}

void handleCameraBridgeStatus() {
  server.send(200, "application/json", buildCameraBridgeStatusJson());
}

void handleTimingStatus() {
  server.send(200, "application/json", buildTimingJson());
}

void handleStreamRuntimeStatus() {
  server.send(200, "application/json", buildStreamRuntimeStatusJson());
}

void handleBleStatus() {
  server.send(200, "application/json", buildBleStatusJson());
}

void handleControlStatus() {
  server.send(200, "application/json", buildControlStatusJson());
}

void handleControlSummary() {
  server.send(200, "application/json", buildControlSummaryJson());
}

void handleTrailMediaStatus() {
  String payload = "{";
  payload += "\"ready\":" + String(sdReady ? "true" : "false");
  payload += ",\"job\":" + buildDurableMediaJobJson();
  if (sdReady) {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    portENTER_CRITICAL(&sdCapacityCacheMux);
    totalBytes = sdCachedTotalBytes;
    usedBytes = sdCachedUsedBytes;
    portEXIT_CRITICAL(&sdCapacityCacheMux);
    const uint64_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
    payload += ",\"free_bytes\":" + String(static_cast<unsigned long long>(freeBytes));
  }
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleTrailMediaList() {
  if (!sdReady || cameraSdTransferActive) {
    server.send(503, "application/json", "{\"error\":\"sd_unavailable\"}");
    return;
  }
  String payload = "{\"items\":[";
  bool first = true;
  File dir = SD.open(TRAIL_MEDIA_DIR, FILE_READ);
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile(FILE_READ);
    while (file) {
      const String name = file.name();
      if (!file.isDirectory() &&
          (name.endsWith(".JPG") || name.endsWith(".mp4")) &&
          !name.endsWith(".part")) {
        const int slash = name.lastIndexOf('/');
        const String filename = slash >= 0 ? name.substring(slash + 1) : name;
        if (!first) payload += ',';
        first = false;
        payload += "{\"filename\":\"" + jsonEscape(filename) + "\"";
        payload += ",\"bytes\":" + String(static_cast<unsigned long>(file.size()));
        payload += ",\"url\":\"/trail-media/file/" + jsonEscape(filename) + "\"}";
      }
      file.close();
      file = dir.openNextFile(FILE_READ);
    }
    dir.close();
  }
  payload += "]}";
  server.send(200, "application/json", payload);
}

void handleTrailMediaFile() {
  const String filename = server.pathArg(0);
  if (filename.isEmpty() || filename.indexOf("..") >= 0 || filename.indexOf('/') >= 0 ||
      !(filename.endsWith(".JPG") || filename.endsWith(".mp4"))) {
    server.send(400, "application/json", "{\"error\":\"invalid_filename\"}");
    return;
  }
  const String path = String(TRAIL_MEDIA_DIR) + "/" + filename;
  if (cameraSdTransferActive) {
    server.send(503, "application/json", "{\"error\":\"storage_busy_camera_transfer\"}");
    return;
  }
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"storage_busy\"}");
    return;
  }
  File file = SD.open(path, FILE_READ);
  if (!file) {
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
    return;
  }
  const size_t fileBytes = file.size();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.setContentLength(fileBytes);
  server.send(200, filename.endsWith(".JPG") ? "image/jpeg" : "video/mp4", "");
  WiFiClient client = server.client();
  // Pace direct SD-to-HTTP streaming so the SPI and network drivers do not
  // starve the monitored idle task.
  uint8_t buffer[512];
  size_t sent = 0;
  httpLongOperationActive = true;
  httpLongOperationLastProgressMs = millis();
  while (sent < fileBytes && client.connected()) {
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) break;
    const int count = file.read(buffer, std::min(sizeof(buffer), fileBytes - sent));
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    if (count <= 0 || client.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) break;
    sent += static_cast<size_t>(count);
    httpLongOperationLastProgressMs = millis();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (onboardStorageMutex == nullptr ||
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    file.close();
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  }
  httpLongOperationActive = false;
}

void handleSystemRestart() {
  server.send(202, "application/json", "{\"ok\":true,\"action\":\"system_restart\",\"restart_pending\":true}");
  delay(100);
  ESP.restart();
}

void handleOnboardCameraReinit() {
  String error;
  if (!queueOnboardJob(ONBOARD_JOB_REINIT, "manual", false, nullptr, error)) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(error) + "\"}");
    return;
  }
  OnboardJobState result{};
  const bool completed = waitOnboardJob(20000UL, result, error);
  const bool ok = completed && result.ok;
  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  if (!ok) payload += ",\"error\":\"" + jsonEscape(error.isEmpty() ? String("onboard_camera_not_ready") : error) + "\"";
  payload += ",\"onboard_camera\":" + buildOnboardCameraStatusJson();
  payload += "}";
  server.send(ok ? 200 : 503, "application/json", payload);
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
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"storage_busy\"}");
    return;
  }
  File file = onboardMediaFs().open(onboardMediaImagePath(onboardLatestMediaId), FILE_READ);
  if (!file) {
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    server.send(404, "application/json", "{\"error\":\"media_not_found\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  const size_t fileBytes = file.size();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  server.setContentLength(fileBytes);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  uint8_t buffer[4096];
  size_t sent = 0;
  httpLongOperationActive = true;
  httpLongOperationLastProgressMs = millis();
  while (sent < fileBytes && client.connected()) {
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) break;
    const int count = file.read(buffer, std::min(sizeof(buffer), fileBytes - sent));
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    if (count <= 0 || client.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) break;
    sent += static_cast<size_t>(count);
    httpLongOperationLastProgressMs = millis();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (onboardStorageMutex == nullptr ||
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    file.close();
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  }
  httpLongOperationActive = false;
}

void handleOnboardCameraCapture() {
  const bool hasOneShotSettings = serverHasOnboardSensorArgs();
  OnboardCameraSettings oneShotSettings = snapshotOnboardCameraSettings();
  if (hasOneShotSettings) {
    String sensorError;
    if (!parseOnboardCameraSettingsFromServer(oneShotSettings, sensorError)) {
      server.send(400, "application/json", "{\"error\":\"" + jsonEscape(sensorError) + "\"}");
      return;
    }
  }

  String queueError;
  if (!queueOnboardJob(ONBOARD_JOB_CAPTURE, "manual", hasOneShotSettings, &oneShotSettings, queueError)) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(queueError) + "\"}");
    return;
  }
  OnboardJobState result{};
  String waitError;
  const bool completed = waitOnboardJob(45000UL, result, waitError);
  const bool ok = completed && result.ok;
  const String captureError = completed ? String(result.error) : waitError;

  String payload = "{";
  payload += "\"ok\":" + String(ok ? "true" : "false");
  if (!ok) payload += ",\"error\":\"" + jsonEscape(captureError.isEmpty() ? String("capture_failed") : captureError) + "\"";
  if (ok) payload += ",\"media\":" + buildOnboardMediaJson(result.media, false);
  payload += ",\"latest_updated\":" + String(ok ? "true" : "false");
  payload += ",\"one_shot_settings\":" + String(hasOneShotSettings ? "true" : "false");
  payload += ",\"settings_restored\":" + String(result.settingsRestored ? "true" : "false");
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
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return false;
  }
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
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);

  // refreshOnboardMediaState() takes the storage mutex itself. Calling it while
  // holding the non-recursive mutex deadlocked the sole HTTP service task.
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
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"storage_busy\"}");
    return;
  }
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
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"storage_busy\"}");
    return;
  }
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
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"storage_busy\"}");
    return;
  }
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
  // Do not let the staged-media uploader immediately reacquire the SD lock
  // between a busy response and the caller's retry.
  trailMediaUploadPauseUntilMs = millis() + 60000UL;
  unsigned deleted = 0;
  unsigned failed = 0;
  if (!deleteAllOnboardMedia(deleted, failed)) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"storage_busy\"}");
    return;
  }
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
  String cameraError;
  if (!ensureOnboardCameraReady(&cameraError)) {
    server.send(503, "application/json", "{\"error\":\"" + jsonEscape(cameraError) + "\"}");
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
  if (serverHasOnboardSensorArgs()) {
    String cameraError;
    if (!ensureOnboardCameraReady(&cameraError)) {
      server.send(503, "application/json", "{\"error\":\"" + jsonEscape(cameraError) + "\"}");
      return;
    }
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
  if (!onboardTimelapseActive) {
    if (onboardCaptureEnabled && onboardCaptureWindowActive()) {
      String cameraError;
      if (ensureOnboardCameraReady(&cameraError)) {
        onboardCameraPoweredDownBySchedule = false;
      }
    } else {
      shutdownOnboardCamera(onboardCaptureEnabled ? "scheduled_window_inactive" : "scheduled_disabled");
      onboardCameraPoweredDownBySchedule = onboardCaptureEnabled;
    }
  }
  server.send(200, "application/json", buildOnboardCameraStatusJson());
}

void handleWifiScan() {
  server.send(410, "application/json", "{\"error\":\"scanner_disabled\",\"detail\":\"WiFi/BLE scanning is disabled in camera-priority firmware\"}");
  return;
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

void clearTrailMediaUploadMarkers(uint32_t &removed, bool &storageBusy) {
  removed = 0;
  storageBusy = false;
  trailMediaRequeueActive = true;
  trailMediaUploadBlockedReason = "requeue_in_progress";
  bool storageLockHeld = onboardStorageMutex == nullptr;
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    storageBusy = true;
  } else if (onboardStorageMutex != nullptr) {
    storageLockHeld = true;
  }
  File manifest;
  if (!storageBusy) {
    manifest = SD.open(DURABLE_JOB_MANIFEST_PATH, FILE_READ);
    if (!manifest) storageBusy = true;
  }
  for (uint32_t index = 0; !storageBusy && index < durableMediaJob.manifestCount; ++index) {
    DurableMediaRecord record{};
    if (manifest.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) == sizeof(record)) {
      const String marker = String(TRAIL_MEDIA_DIR) + "/" + String(record.id) +
                            (record.type == 1 ? ".JPG.uploaded" : ".mp4.uploaded");
      if (SD.remove(marker)) ++removed;
      const String mediaPath = marker.substring(0, marker.length() - 9);
      SD.remove(mediaPath + ".upload_attempts");
      SD.remove(mediaPath + ".upload_failed");
    } else {
      storageBusy = true;
      break;
    }
    lastHttpServiceMs = millis();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (manifest) manifest.close();
  if (onboardStorageMutex != nullptr && storageLockHeld) xSemaphoreGive(onboardStorageMutex);
  trailMediaRequeueActive = false;
  trailMediaUploadLastAttemptMs = 0;
  trailMediaUploadBlockedReason = "";
}

void handleTrailMediaRequeue() {
  if (!sdReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sd_unavailable\"}");
    return;
  }
  if (trailMediaUploadActive || trailMediaRequeueActive || durableMediaJobIsActive()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"media_busy\"}");
    return;
  }
  uint32_t removed = 0;
  bool storageBusy = false;
  clearTrailMediaUploadMarkers(removed, storageBusy);
  String payload = "{\"ok\":" + String(storageBusy ? "false" : "true");
  payload += ",\"markers_removed\":" + String(removed);
  if (storageBusy) payload += ",\"error\":\"storage_busy\"";
  payload += ",\"upload_resumes_automatically\":true}";
  server.send(storageBusy ? 503 : 200, "application/json", payload);
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
  if (sdReady && durableMediaJobLoaded && durableMediaJobIsActive()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"durable_job_active\"}");
    return;
  }
  bool ok = mountSdCardWithRetry();
  if (ok && !loadDurableMediaJob()) {
    sdLastMessage = "durable_job_load_failed";
    ok = false;
  }
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
  server.send(410, "application/json", "{\"error\":\"scanner_disabled\",\"detail\":\"WiFi/BLE observation upload is disabled in camera-priority firmware\"}");
  return;
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

void handleScannerConfigGet() {
  server.send(200, "application/json", buildScannerConfigJson());
}

void handleScannerConfigPost() {
  scannerScheduleEnabled = false;
  saveScannerConfig();
  server.send(410, "application/json", buildScannerConfigJson());
  return;
  if (server.hasArg("enabled")) {
    scannerScheduleEnabled = boolLikeValue(server.arg("enabled"));
  } else if (server.hasArg("scheduled_scans_enabled")) {
    scannerScheduleEnabled = boolLikeValue(server.arg("scheduled_scans_enabled"));
  } else if (server.hasArg("plain")) {
    String body = server.arg("plain");
    body.toLowerCase();
    if (body.indexOf("\"enabled\":true") >= 0 || body.indexOf("\"scheduled_scans_enabled\":true") >= 0) {
      scannerScheduleEnabled = true;
    } else if (body.indexOf("\"enabled\":false") >= 0 || body.indexOf("\"scheduled_scans_enabled\":false") >= 0) {
      scannerScheduleEnabled = false;
    } else {
      server.send(400, "application/json", "{\"error\":\"invalid_request\",\"detail\":\"expected enabled=true|false\"}");
      return;
    }
  } else {
    server.send(400, "application/json", "{\"error\":\"invalid_request\",\"detail\":\"expected enabled=true|false\"}");
    return;
  }
  saveScannerConfig();
  enqueueUploadEvent("scanner_config", scannerScheduleEnabled ? "scheduled_scans_enabled" : "scheduled_scans_disabled", "{}", true);
  server.send(200, "application/json", buildScannerConfigJson());
}

void handleScannerEnable() {
  scannerScheduleEnabled = false;
  saveScannerConfig();
  enqueueUploadEvent("scanner_config", "scanner_disabled_camera_priority", "{}", true);
  server.send(410, "application/json", buildScannerConfigJson());
}

void handleScannerDisable() {
  scannerScheduleEnabled = false;
  saveScannerConfig();
  enqueueUploadEvent("scanner_config", "scheduled_scans_disabled", "{}", true);
  server.send(200, "application/json", buildScannerConfigJson());
}

bool firmwareUpdateAuthorized() {
  if (String(UPSTREAM_API_TOKEN).isEmpty()) return true;
  return server.hasArg("token") && server.arg("token") == UPSTREAM_API_TOKEN;
}

String buildFirmwareStatusJson() {
  String payload = "{";
  payload += "\"firmware_name\":\"" + jsonEscape(FIRMWARE_NAME) + "\"";
  payload += ",\"firmware_version\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"";
  payload += ",\"update_endpoint\":\"/firmware/update\"";
  payload += ",\"in_progress\":" + String(otaInProgress ? "true" : "false");
  payload += ",\"last_succeeded\":" + String(otaSucceeded ? "true" : "false");
  payload += ",\"restart_pending\":" + String(otaRestartPending ? "true" : "false");
  payload += ",\"bytes_written\":" + String(static_cast<unsigned>(otaBytesWritten));
  payload += ",\"last_error\":\"" + jsonEscape(otaLastError) + "\"";
  payload += ",\"started_age_ms\":" + String(msSince(otaStartedMs));
  payload += ",\"finished_age_ms\":" + String(msSince(otaFinishedMs));
  payload += ",\"free_sketch_space\":" + String(static_cast<unsigned>(ESP.getFreeSketchSpace()));
  payload += "}";
  return payload;
}

void handleFirmwareStatus() {
  server.send(200, "application/json", buildFirmwareStatusJson());
}

void handleFirmwareUpdateFinished() {
  if (!firmwareUpdateAuthorized()) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (!otaSucceeded) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(otaLastError.isEmpty() ? String("update_failed") : otaLastError) + "\"}");
    return;
  }
  otaRestartPending = true;
  otaRestartAtMs = millis() + 1500UL;
  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"bytes_written\":" + String(static_cast<unsigned>(otaBytesWritten));
  payload += ",\"restart_pending\":true";
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleFirmwareUploadStream() {
  HTTPUpload &upload = server.upload();
  if (!firmwareUpdateAuthorized()) {
    otaLastError = "unauthorized";
    return;
  }

  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    httpLongOperationActive = true;
    httpLongOperationLastProgressMs = millis();
    otaSucceeded = false;
    otaRestartPending = false;
    otaStartedMs = millis();
    otaFinishedMs = 0;
    otaBytesWritten = 0;
    otaLastError = "";
    Serial.printf("[ota] update start filename=%s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaLastError = "begin_failed";
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    httpLongOperationLastProgressMs = millis();
    if (!otaLastError.isEmpty()) return;
    const size_t written = Update.write(upload.buf, upload.currentSize);
    otaBytesWritten += written;
    if (written != upload.currentSize) {
      otaLastError = "write_failed";
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaLastError.isEmpty() && Update.end(true)) {
      otaSucceeded = true;
      otaFinishedMs = millis();
      Serial.printf("[ota] update complete bytes=%u\n", static_cast<unsigned>(otaBytesWritten));
    } else {
      if (otaLastError.isEmpty()) otaLastError = "end_failed";
      Update.printError(Serial);
    }
    otaInProgress = false;
    httpLongOperationActive = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    httpLongOperationActive = false;
    otaSucceeded = false;
    otaFinishedMs = millis();
    otaLastError = "aborted";
    Serial.println("[ota] update aborted");
  }
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
  payload += ",\"message\":" + jsonNullableString(message);
  payload += ",\"eta_sec\":" + String(controlActionEtaSec(CONTROL_ACTION_BRINGUP));
  payload += ",\"poll_path\":\"/control/summary\"";
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
  payload += ",\"message\":" + jsonNullableString(message);
  payload += ",\"eta_sec\":" + String(controlActionEtaSec(CONTROL_ACTION_STREAM_START));
  payload += ",\"poll_path\":\"/control/summary\"";
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
  payload += ",\"message\":" + jsonNullableString(message);
  payload += ",\"eta_sec\":" + String(controlActionEtaSec(CONTROL_ACTION_STREAM_STOP));
  payload += ",\"poll_path\":\"/control/summary\"";
  payload += "}";
  server.send(accepted ? 202 : 409, "application/json", payload);
}

uint32_t durableJobChecksum(const DurableMediaJobState &state) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&state);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < offsetof(DurableMediaJobState, checksum); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool persistDurableMediaJobMirror(bool force = false) {
  const bool importantChange = durableMediaJob.jobId != durableMirrorLastJobId ||
                               durableMediaJob.status != durableMirrorLastStatus;
  const bool progressCheckpoint = durableMediaJob.currentIndex >= durableMirrorLastIndex + 10;
  const bool timeCheckpoint = durableMirrorLastWriteMs == 0 ||
                              millis() - durableMirrorLastWriteMs >= 300000UL;
  if (!force && !importantChange && !progressCheckpoint && !timeCheckpoint) return true;
  Preferences mirrorPrefs;
  if (!mirrorPrefs.begin("durable", false)) return false;
  const size_t written = mirrorPrefs.putBytes("job", &durableMediaJob, sizeof(durableMediaJob));
  mirrorPrefs.end();
  if (written != sizeof(durableMediaJob)) return false;
  durableMirrorLastJobId = durableMediaJob.jobId;
  durableMirrorLastIndex = durableMediaJob.currentIndex;
  durableMirrorLastStatus = durableMediaJob.status;
  durableMirrorLastWriteMs = millis();
  return true;
}

bool loadDurableMediaJobMirror() {
  Preferences mirrorPrefs;
  if (!mirrorPrefs.begin("durable", true)) return false;
  DurableMediaJobState mirrored{};
  const size_t read = mirrorPrefs.getBytes("job", &mirrored, sizeof(mirrored));
  mirrorPrefs.end();
  if (read != sizeof(mirrored) || mirrored.magic != DURABLE_MEDIA_JOB_MAGIC ||
      mirrored.version != DURABLE_MEDIA_JOB_VERSION ||
      mirrored.checksum != durableJobChecksum(mirrored)) return false;
  durableMediaJob = mirrored;
  durableMediaJobFromMirror = true;
  durableMirrorLastJobId = mirrored.jobId;
  durableMirrorLastIndex = mirrored.currentIndex;
  durableMirrorLastStatus = mirrored.status;
  Serial.printf("[durable-job] loaded NVS mirror id=%u status=%s index=%u/%u\n",
                mirrored.jobId, durableMediaStatusName(mirrored.status),
                mirrored.currentIndex, mirrored.manifestCount);
  return true;
}

const char *durableMediaActionName(uint8_t action) {
  switch (action) {
    case DURABLE_MEDIA_ACTION_DOWNLOAD_ALL: return "download_all_media";
    case DURABLE_MEDIA_ACTION_DOWNLOAD_AND_DELETE: return "download_and_delete_media";
    default: return "none";
  }
}

const char *durableMediaStatusName(uint8_t status) {
  switch (status) {
    case DURABLE_MEDIA_QUEUED: return "queued";
    case DURABLE_MEDIA_RUNNING: return "running";
    case DURABLE_MEDIA_WAITING_RETRY: return "waiting_retry";
    case DURABLE_MEDIA_SUCCEEDED: return "succeeded";
    case DURABLE_MEDIA_FAILED: return "failed";
    case DURABLE_MEDIA_CANCELLED: return "cancelled";
    default: return "idle";
  }
}

void setDurableJobText(char *target, size_t targetSize, const String &value) {
  if (targetSize == 0) return;
  strlcpy(target, value.c_str(), targetSize);
}

bool saveDurableMediaJob() {
  if (durableMediaJobMutex != nullptr) xSemaphoreTake(durableMediaJobMutex, portMAX_DELAY);
  durableMediaJob.magic = DURABLE_MEDIA_JOB_MAGIC;
  durableMediaJob.version = DURABLE_MEDIA_JOB_VERSION;
  durableMediaJob.updatedBoot = bootSessionId;
  durableMediaJob.checksum = durableJobChecksum(durableMediaJob);
  persistDurableMediaJobMirror();
  if (!sdReady || !ensureFsDir(SD, DURABLE_JOB_DIR)) {
    if (durableMediaJobMutex != nullptr) xSemaphoreGive(durableMediaJobMutex);
    return false;
  }
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    if (durableMediaJobMutex != nullptr) xSemaphoreGive(durableMediaJobMutex);
    return false;
  }
  SD.remove(DURABLE_JOB_STATE_TEMP_PATH);
  File file = SD.open(DURABLE_JOB_STATE_TEMP_PATH, FILE_WRITE);
  const bool wrote = file && file.write(reinterpret_cast<const uint8_t *>(&durableMediaJob), sizeof(durableMediaJob)) == sizeof(durableMediaJob);
  if (file) {
    file.flush();
    file.close();
  }
  bool ok = wrote;
  if (ok) {
    // Keep the previous checkpoint until the replacement is completely
    // written. A reset at any point leaves at least one valid state among the
    // primary, temporary, backup, and NVS mirror copies.
    SD.remove(DURABLE_JOB_STATE_BACKUP_PATH);
    if (SD.exists(DURABLE_JOB_STATE_PATH)) {
      ok = SD.rename(DURABLE_JOB_STATE_PATH, DURABLE_JOB_STATE_BACKUP_PATH);
    }
  }
  if (ok) {
    ok = SD.rename(DURABLE_JOB_STATE_TEMP_PATH, DURABLE_JOB_STATE_PATH);
  }
  if (!ok && !SD.exists(DURABLE_JOB_STATE_PATH) &&
      SD.exists(DURABLE_JOB_STATE_BACKUP_PATH)) {
    SD.rename(DURABLE_JOB_STATE_BACKUP_PATH, DURABLE_JOB_STATE_PATH);
  }
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  if (durableMediaJobMutex != nullptr) xSemaphoreGive(durableMediaJobMutex);
  return ok;
}

bool readDurableMediaJobFile(const char *path, DurableMediaJobState &state) {
  File file = SD.open(path, FILE_READ);
  if (!file) return false;
  const bool readOk = file.read(reinterpret_cast<uint8_t *>(&state), sizeof(state)) == sizeof(state);
  file.close();
  return readOk && state.magic == DURABLE_MEDIA_JOB_MAGIC &&
         state.version == DURABLE_MEDIA_JOB_VERSION &&
         state.checksum == durableJobChecksum(state);
}

bool durableCheckpointIsNewer(const DurableMediaJobState &candidate,
                              const DurableMediaJobState &selected) {
  if (candidate.jobId != selected.jobId) return candidate.jobId > selected.jobId;
  if (candidate.currentIndex != selected.currentIndex) {
    return candidate.currentIndex > selected.currentIndex;
  }
  // For the same item, prefer a terminal state, then the checkpoint written
  // by the later boot. Temporary is examined first, so equal active records
  // retain the newly written replacement.
  const bool candidateTerminal = candidate.status == DURABLE_MEDIA_SUCCEEDED ||
                                 candidate.status == DURABLE_MEDIA_FAILED ||
                                 candidate.status == DURABLE_MEDIA_CANCELLED;
  const bool selectedTerminal = selected.status == DURABLE_MEDIA_SUCCEEDED ||
                                selected.status == DURABLE_MEDIA_FAILED ||
                                selected.status == DURABLE_MEDIA_CANCELLED;
  if (candidateTerminal != selectedTerminal) return candidateTerminal;
  return candidate.updatedBoot > selected.updatedBoot;
}

bool loadDurableMediaJob() {
  durableMediaJobLoaded = false;
  if (!sdReady) return false;
  // setup() loads the NVS mirror before SD. Preserve it as a recovery
  // candidate instead of replacing it with an empty job when the primary was
  // interrupted between rename operations.
  DurableMediaJobState selected{};
  bool haveSelected = false;
  String selectedSource;
  if (durableMediaJobFromMirror && durableMediaJob.magic == DURABLE_MEDIA_JOB_MAGIC &&
      durableMediaJob.version == DURABLE_MEDIA_JOB_VERSION &&
      durableMediaJob.checksum == durableJobChecksum(durableMediaJob)) {
    selected = durableMediaJob;
    haveSelected = true;
    selectedSource = "nvs_mirror";
  }
  ensureFsDir(SD, DURABLE_JOB_DIR);
  ensureFsDir(SD, TRAIL_MEDIA_DIR);
  const char *paths[] = {DURABLE_JOB_STATE_TEMP_PATH, DURABLE_JOB_STATE_PATH,
                         DURABLE_JOB_STATE_BACKUP_PATH};
  const char *sources[] = {"sd_temp", "sd", "sd_backup"};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    DurableMediaJobState candidate{};
    if (readDurableMediaJobFile(paths[i], candidate) &&
        (!haveSelected || durableCheckpointIsNewer(candidate, selected))) {
      selected = candidate;
      haveSelected = true;
      selectedSource = sources[i];
    }
  }
  if (!haveSelected) {
    durableMediaJob = {};
    durableMediaJob.magic = DURABLE_MEDIA_JOB_MAGIC;
    durableMediaJob.version = DURABLE_MEDIA_JOB_VERSION;
    durableMediaJobLoaded = true;
    const bool saved = saveDurableMediaJob();
    if (!saved) durableMediaJobLoaded = false;
    return saved;
  }
  durableMediaJob = selected;
  durableMediaJobLoaded = true;
  durableMediaJobFromMirror = selectedSource == "nvs_mirror";
  if (durableMediaJob.status == DURABLE_MEDIA_RUNNING) {
    durableMediaJob.status = DURABLE_MEDIA_WAITING_RETRY;
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "resume_after_reset");
    setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError), "board_reset_during_job");
  }
  if (durableMediaJob.status == DURABLE_MEDIA_SUCCEEDED &&
      durableMediaJob.action == DURABLE_MEDIA_ACTION_DOWNLOAD_AND_DELETE &&
      durableMediaJob.deletedCount != durableMediaJob.manifestCount) {
    durableMediaJob.status = DURABLE_MEDIA_FAILED;
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                      "incomplete_delete");
    setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                      "incomplete_delete:deleted=" + String(durableMediaJob.deletedCount) +
                      ",total=" + String(durableMediaJob.manifestCount) +
                      ",skipped=" + String(durableMediaJob.skippedCount));
    Serial.printf("[durable-job] corrected false success id=%u deleted=%u/%u skipped=%u\n",
                  durableMediaJob.jobId, durableMediaJob.deletedCount,
                  durableMediaJob.manifestCount, durableMediaJob.skippedCount);
  }
  // Repair the canonical SD checkpoint from whichever valid copy won.
  saveDurableMediaJob();
  persistDurableMediaJobMirror(true);
  Serial.printf("[durable-job] loaded source=%s id=%u action=%s status=%s index=%u/%u\n",
                selectedSource.c_str(),
                durableMediaJob.jobId,
                durableMediaActionName(durableMediaJob.action),
                durableMediaStatusName(durableMediaJob.status),
                durableMediaJob.currentIndex,
                durableMediaJob.manifestCount);
  return true;
}

String buildDurableMediaJobJson() {
  String payload = "{";
  payload += "\"job_id\":" + String(durableMediaJob.jobId);
  payload += ",\"state_source\":\"" + String(durableMediaJobLoaded ? "sd" : (durableMediaJobFromMirror ? "nvs_mirror" : "memory")) + "\"";
  payload += ",\"action\":\"" + String(durableMediaActionName(durableMediaJob.action)) + "\"";
  payload += ",\"status\":\"" + String(durableMediaStatusName(durableMediaJob.status)) + "\"";
  payload += ",\"stage\":\"" + jsonEscape(String(durableMediaJob.stage)) + "\"";
  payload += ",\"total\":" + String(durableMediaJob.manifestCount);
  payload += ",\"processed\":" + String(durableMediaJob.currentIndex);
  payload += ",\"current_media_id\":" + String(durableMediaJob.currentMediaId);
  payload += ",\"downloaded\":" + String(durableMediaJob.downloadedCount);
  payload += ",\"skipped\":" + String(durableMediaJob.skippedCount);
  payload += ",\"deleted\":" + String(durableMediaJob.deletedCount);
  payload += ",\"retry_count\":" + String(durableMediaJob.retryCount);
  payload += ",\"bytes_downloaded\":" + String(durableMediaJob.bytesDownloaded);
  payload += ",\"item_bytes_received\":" + String(durableTransferBytes);
  payload += ",\"item_expected_bytes\":" + String(durableTransferExpectedBytes);
  payload += ",\"item_elapsed_ms\":" + String(durableTransferStartedMs == 0 ? 0 : msSince(durableTransferStartedMs));
  payload += ",\"item_progress_age_ms\":" + String(durableTransferLastProgressMs == 0 ? 0 : msSince(durableTransferLastProgressMs));
  payload += ",\"last_error\":\"" + jsonEscape(String(durableMediaJob.lastError)) + "\"";
  payload += ",\"battery_mv\":" + String(cachedStorageBatteryMv());
  payload += ",\"minimum_battery_mv\":" + String(DURABLE_MEDIA_MIN_BATTERY_MV);
  payload += ",\"recovery_battery_mv\":" + String(DURABLE_MEDIA_RECOVER_BATTERY_MV);
  payload += ",\"staging_directory\":\"" + String(TRAIL_MEDIA_DIR) + "\"";
  payload += "}";
  return payload;
}

ControlAction parseJobAction(const String &rawAction) {
  String action = rawAction;
  action.trim();
  action.toLowerCase();
  if (action == "bringup" || action == "open_session") return CONTROL_ACTION_BRINGUP;
  if (action == "live_view_start" || action == "stream_start") return CONTROL_ACTION_STREAM_START;
  if (action == "live_view_stop" || action == "stream_stop" || action == "close_session") return CONTROL_ACTION_STREAM_STOP;
  if (action == "trigger_photo" || action == "take_picture" || action == "capture_photo") return CONTROL_ACTION_TRIGGER_PHOTO;
  return CONTROL_ACTION_NONE;
}

void handleJobsGet() {
  String payload = "{";
  payload += "\"ok\":true";
  payload += ",\"durable\":true";
  payload += ",\"supported_actions\":[\"bringup\",\"live_view_start\",\"live_view_stop\",\"trigger_photo\",\"download_all_media\",\"download_and_delete_media\"]";
  payload += ",\"durable_media_job\":" + buildDurableMediaJobJson();
  payload += ",\"control\":";
  payload += buildControlStatusJson();
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleJobsPost() {
  if (automaticRecoverySuspended) {
    server.send(503, "application/json",
                "{\"ok\":false,\"error\":\"automatic_recovery_suspended\","
                "\"resume_url\":\"/recovery/resume\"}");
    return;
  }
  String actionArg = server.hasArg("action") ? server.arg("action") : "";
  if (actionArg.isEmpty() && server.hasArg("plain")) {
    String body = server.arg("plain");
    body.toLowerCase();
    if (body.indexOf("live_view_start") >= 0 || body.indexOf("stream_start") >= 0) {
      actionArg = "live_view_start";
    } else if (body.indexOf("live_view_stop") >= 0 || body.indexOf("stream_stop") >= 0) {
      actionArg = "live_view_stop";
    } else if (body.indexOf("trigger_photo") >= 0 || body.indexOf("take_picture") >= 0) {
      actionArg = "trigger_photo";
    } else if (body.indexOf("download_and_delete") >= 0) {
      actionArg = "download_and_delete_media";
    } else if (body.indexOf("download_all") >= 0) {
      actionArg = "download_all_media";
    } else if (body.indexOf("bringup") >= 0 || body.indexOf("open_session") >= 0) {
      actionArg = "bringup";
    }
  }

  String durableAction = actionArg;
  durableAction.toLowerCase();
  if (durableAction == "download_all" || durableAction == "download_all_media" ||
      durableAction == "download_and_delete" || durableAction == "download_and_delete_media") {
    const DurableMediaJobAction action = durableAction.indexOf("and_delete") >= 0
                                           ? DURABLE_MEDIA_ACTION_DOWNLOAD_AND_DELETE
                                           : DURABLE_MEDIA_ACTION_DOWNLOAD_ALL;
    String message;
    const bool accepted = queueDurableMediaJob(action, message);
    String payload = "{";
    payload += "\"ok\":" + String(accepted ? "true" : "false");
    payload += ",\"accepted\":" + String(accepted ? "true" : "false");
    payload += ",\"durable\":true";
    payload += ",\"message\":\"" + jsonEscape(message) + "\"";
    payload += ",\"job\":" + buildDurableMediaJobJson();
    payload += "}";
    server.send(accepted ? 202 : 409, "application/json", payload);
    return;
  }

  const ControlAction action = parseJobAction(actionArg);
  if (action == CONTROL_ACTION_NONE) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_request\",\"detail\":\"unsupported job action\"}");
    return;
  }

  String message;
  const bool accepted = queueControlAction(action, message);
  String payload = "{";
  payload += "\"ok\":" + String(accepted ? "true" : "false");
  payload += ",\"accepted\":" + String(accepted ? "true" : "false");
  payload += ",\"durable\":false";
  payload += ",\"action\":\"" + String(controlActionName(action)) + "\"";
  payload += ",\"message\":" + jsonNullableString(message);
  payload += ",\"eta_sec\":" + String(controlActionEtaSec(action));
  payload += ",\"poll_path\":\"/control/summary\"";
  payload += ",\"control\":";
  payload += buildControlStatusJson();
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

long jsonObjectIntField(const String &objectJson, const char *fieldName, long fallback = 0) {
  const String key = String("\"") + fieldName + "\"";
  const int keyAt = objectJson.indexOf(key);
  if (keyAt < 0) return fallback;
  const int colon = objectJson.indexOf(':', keyAt + key.length());
  if (colon < 0) return fallback;
  int start = colon + 1;
  while (start < static_cast<int>(objectJson.length()) &&
         (objectJson[start] == ' ' || objectJson[start] == '\t' || objectJson[start] == '"')) ++start;
  return strtol(objectJson.c_str() + start, nullptr, 10);
}

String jsonObjectStringField(const String &objectJson, const char *fieldName) {
  const String key = String("\"") + fieldName + "\"";
  const int keyAt = objectJson.indexOf(key);
  if (keyAt < 0) return "";
  const int colon = objectJson.indexOf(':', keyAt + key.length());
  if (colon < 0) return "";
  const int quote = objectJson.indexOf('"', colon + 1);
  if (quote < 0) return "";
  String value;
  bool escaped = false;
  for (int i = quote + 1; i < static_cast<int>(objectJson.length()); ++i) {
    const char c = objectJson[i];
    if (escaped) {
      value += c;
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      break;
    } else {
      value += c;
    }
  }
  return value;
}

bool appendDurableManifestPage(const String &galleryJson, bool truncate,
                               uint32_t &recordCount, uint32_t &minimumId) {
  recordCount = 0;
  minimumId = UINT32_MAX;
  const int dataKey = galleryJson.indexOf("\"data\"");
  const int arrayStart = dataKey >= 0 ? galleryJson.indexOf('[', dataKey) : -1;
  if (arrayStart < 0 || !sdReady) return false;
  if (truncate) SD.remove(DURABLE_JOB_MANIFEST_PATH);
  File manifest = SD.open(DURABLE_JOB_MANIFEST_PATH, truncate ? FILE_WRITE : FILE_APPEND);
  if (!manifest) return false;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  int objectStart = -1;
  for (int i = arrayStart + 1; i < static_cast<int>(galleryJson.length()); ++i) {
    const char c = galleryJson[i];
    if (escaped) { escaped = false; continue; }
    if (c == '\\' && inString) { escaped = true; continue; }
    if (c == '"') { inString = !inString; continue; }
    if (inString) continue;
    if (c == '{') {
      if (depth == 0) objectStart = i;
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && objectStart >= 0) {
        const String item = galleryJson.substring(objectStart, i + 1);
        DurableMediaRecord record{};
        record.id = static_cast<uint32_t>(jsonObjectIntField(item, "id", 0));
        record.type = static_cast<uint8_t>(jsonObjectIntField(item, "type", 0));
        record.expectedBytes = static_cast<uint32_t>(jsonObjectIntField(item, "size", 0));
        const String uid = jsonObjectStringField(item, "uid");
        const String date = jsonObjectStringField(item, "date");
        strlcpy(record.uid, uid.c_str(), sizeof(record.uid));
        strlcpy(record.date, date.c_str(), sizeof(record.date));
        if (record.id > 0 && (record.type == 1 || record.type == 2) &&
            manifest.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) == sizeof(record)) {
          ++recordCount;
          if (record.id < minimumId) minimumId = record.id;
        }
        objectStart = -1;
      }
    } else if (c == ']' && depth == 0) {
      break;
    }
  }
  manifest.flush();
  manifest.close();
  // A valid data array with zero media records is the pagination terminator.
  // Do not require the camera to serialize the empty array as the exact text
  // "[]"; some revisions include whitespace and line breaks.
  return true;
}

bool readDurableManifestRecord(uint32_t index, DurableMediaRecord &record) {
  File manifest = SD.open(DURABLE_JOB_MANIFEST_PATH, FILE_READ);
  if (!manifest) return false;
  const size_t offset = static_cast<size_t>(index) * sizeof(DurableMediaRecord);
  const bool ok = offset + sizeof(record) <= manifest.size() && manifest.seek(offset) &&
                  manifest.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) == sizeof(record);
  manifest.close();
  return ok;
}

bool writeDurableManifestRecord(uint32_t index, const DurableMediaRecord &record) {
  File manifest = SD.open(DURABLE_JOB_MANIFEST_PATH, "r+");
  if (!manifest) return false;
  const size_t offset = static_cast<size_t>(index) * sizeof(DurableMediaRecord);
  const bool ok = offset + sizeof(record) <= manifest.size() && manifest.seek(offset) &&
                  manifest.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) == sizeof(record);
  if (ok) manifest.flush();
  manifest.close();
  return ok;
}

String durableMediaFilePath(const DurableMediaRecord &record, bool partial = false) {
  String path = String(TRAIL_MEDIA_DIR) + "/" + String(record.id) + (record.type == 1 ? ".JPG" : ".mp4");
  if (partial) path += ".part";
  return path;
}

String durableMediaSizeSidecarPath(const String &mediaPath) {
  return mediaPath + ".size";
}

void invalidateTrailMediaUploadState(const String &mediaPath) {
  // Camera media IDs and therefore staged filenames can be reused.  A fresh
  // download must never inherit the previous file's terminal upload marker.
  // Otherwise the uploader silently treats new bytes as already delivered.
  SD.remove(mediaPath + ".uploaded");
  SD.remove(mediaPath + ".upload_failed");
  SD.remove(mediaPath + ".upload_attempts");
}

uint32_t readDurableMediaStoredSize(const String &mediaPath) {
  File file = SD.open(durableMediaSizeSidecarPath(mediaPath), FILE_READ);
  if (!file) return 0;
  String value = file.readString();
  file.close();
  value.trim();
  const long parsed = strtol(value.c_str(), nullptr, 10);
  return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
}

String durableMediaIdentitySidecarPath(const String &mediaPath) {
  return mediaPath + ".src";
}

// Camera media IDs are reused after deletion, so "<id>.JPG" on the SD card
// may belong to an older capture. The .src sidecar records which camera item
// a staged or partial file came from; staged bytes are only trusted when it
// matches the manifest record exactly.
String durableMediaIdentity(const DurableMediaRecord &record) {
  return String(record.id) + "|" + String(record.type) + "|" + String(record.expectedBytes) +
         "|" + String(record.date) + "|" + String(record.uid);
}

bool durableMediaIdentityMatches(const String &mediaPath, const DurableMediaRecord &record) {
  File file = SD.open(durableMediaIdentitySidecarPath(mediaPath), FILE_READ);
  if (!file) return false;
  String value = file.readString();
  file.close();
  value.trim();
  return value == durableMediaIdentity(record);
}

bool writeDurableMediaIdentity(const String &mediaPath, const DurableMediaRecord &record) {
  File file = SD.open(durableMediaIdentitySidecarPath(mediaPath), FILE_WRITE);
  if (!file) return false;
  const String identity = durableMediaIdentity(record);
  const bool ok = file.print(identity) == identity.length();
  file.flush();
  file.close();
  return ok;
}

// A staged file that belongs to a different camera item. If the server
// already has it, drop it; otherwise move it aside so an un-uploaded capture
// is never overwritten. The "<id>_rN" name keeps the media ID prefix that the
// uploader reports as trail_media_id.
void retireStaleStagedMedia(const String &finalPath, const DurableMediaRecord &record) {
  if (SD.exists(finalPath + ".uploaded")) {
    SD.remove(finalPath);
  } else {
    const String ext = record.type == 1 ? ".JPG" : ".mp4";
    for (uint16_t n = 1; n < 1000; ++n) {
      const String aside = String(TRAIL_MEDIA_DIR) + "/" + String(record.id) + "_r" + String(n) + ext;
      if (SD.exists(aside)) continue;
      if (!SD.rename(finalPath, aside)) SD.remove(finalPath);
      Serial.printf("[durable-job] retired stale staged %s -> %s\n", finalPath.c_str(), aside.c_str());
      break;
    }
  }
  SD.remove(durableMediaSizeSidecarPath(finalPath));
  SD.remove(durableMediaIdentitySidecarPath(finalPath));
  invalidateTrailMediaUploadState(finalPath);
}

// Cheap structural check before a JPEG's camera copy can be deleted: SOI at
// the start and an EOI marker near the end (cameras may pad after EOI).
bool validateJpegFileBasic(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file) return false;
  const size_t size = file.size();
  uint8_t head[2] = {0, 0};
  bool ok = size > 4 && file.read(head, 2) == 2 && head[0] == 0xFF && head[1] == 0xD8;
  if (ok) {
    const size_t tailBytes = std::min(size, static_cast<size_t>(4096));
    uint8_t *tail = static_cast<uint8_t *>(malloc(tailBytes));
    ok = tail != nullptr && file.seek(size - tailBytes) && file.read(tail, tailBytes) == tailBytes;
    bool sawEoi = false;
    for (size_t i = 0; ok && i + 1 < tailBytes; ++i) {
      if (tail[i] == 0xFF && tail[i + 1] == 0xD9) { sawEoi = true; break; }
    }
    free(tail);
    ok = ok && sawEoi;
  }
  file.close();
  return ok;
}

size_t durableSdFileSize(const String &path) {
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;
  size_t size = 0;
  File file = SD.open(path, FILE_READ);
  if (file) {
    size = file.size();
    file.close();
  }
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  return size;
}

void writeDurableMediaStoredSize(const String &mediaPath, uint32_t bytes) {
  if (bytes == 0) return;
  File file = SD.open(durableMediaSizeSidecarPath(mediaPath), FILE_WRITE);
  if (!file) return;
  file.print(String(bytes));
  file.flush();
  file.close();
}

uint64_t readBigEndian32(const uint8_t *bytes) {
  return (static_cast<uint64_t>(bytes[0]) << 24) |
         (static_cast<uint64_t>(bytes[1]) << 16) |
         (static_cast<uint64_t>(bytes[2]) << 8) |
         static_cast<uint64_t>(bytes[3]);
}

bool isPrintableMp4AtomType(const uint8_t *type) {
  for (uint8_t i = 0; i < 4; ++i) {
    if (type[i] < 0x20 || type[i] > 0x7e) return false;
  }
  return true;
}

bool validateMp4FileBasic(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file) return false;
  const uint64_t fileSize = file.size();
  uint64_t offset = 0;
  bool sawFtyp = false;
  bool sawMdat = false;
  bool sawMoov = false;
  uint16_t atomCount = 0;
  while (offset + 8 <= fileSize && atomCount < 256) {
    if (!file.seek(offset)) { file.close(); return false; }
    uint8_t header[8];
    if (file.read(header, sizeof(header)) != sizeof(header)) { file.close(); return false; }
    uint64_t atomSize = readBigEndian32(header);
    const uint8_t *atomType = header + 4;
    if (!isPrintableMp4AtomType(atomType)) { file.close(); return false; }
    uint64_t headerSize = 8;
    if (atomSize == 1) {
      uint8_t extended[8];
      if (file.read(extended, sizeof(extended)) != sizeof(extended)) { file.close(); return false; }
      atomSize = (readBigEndian32(extended) << 32) | readBigEndian32(extended + 4);
      headerSize = 16;
    } else if (atomSize == 0) {
      atomSize = fileSize - offset;
    }
    if (atomSize < headerSize || offset + atomSize > fileSize) {
      file.close();
      return false;
    }
    if (memcmp(atomType, "ftyp", 4) == 0) sawFtyp = true;
    if (memcmp(atomType, "mdat", 4) == 0) sawMdat = true;
    if (memcmp(atomType, "moov", 4) == 0) sawMoov = true;
    offset += atomSize;
    ++atomCount;
  }
  file.close();
  return fileSize > 0 && offset == fileSize && sawFtyp && sawMdat && sawMoov;
}

uint32_t parseContentRangeTotal(const String &line) {
  const int slash = line.lastIndexOf('/');
  if (slash < 0 || slash + 1 >= static_cast<int>(line.length())) return 0;
  const String totalText = line.substring(slash + 1);
  if (totalText == "*") return 0;
  const long parsed = strtol(totalText.c_str(), nullptr, 10);
  return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
}

bool flushCameraSdBuffer(File &output, uint8_t *sdBuffer, size_t &buffered,
                         String &error) {
  if (buffered == 0) return true;
  // The control worker shares core 0 with the radio/idle tasks. Large blocking
  // FAT writes can starve IDLE0 long enough for the ESP task watchdog to fire.
  const size_t requested = buffered;
  size_t committed = 0;
  uint8_t lockTimeouts = 0;
  uint8_t zeroWrites = 0;
  // SD cards can remain busy for hundreds of milliseconds while completing an
  // internal erase/program operation. Three 20 ms retries falsely treated
  // that normal latency as permanent media failure. Keep this bounded to
  // roughly one second and continue feeding the task watchdog.
  static const uint8_t MAX_TRANSIENT_WRITE_FAILURES = 10;
  while (committed < requested &&
         lockTimeouts + zeroWrites < MAX_TRANSIENT_WRITE_FAILURES) {
    feedTaskWdt();
    vTaskDelay(pdMS_TO_TICKS(1));
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(750)) != pdTRUE) {
      ++lockTimeouts;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    const size_t wrote = output.write(sdBuffer + committed, requested - committed);
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    if (wrote == 0) {
      ++zeroWrites;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    committed += wrote;
  }
  if (committed == requested) {
    buffered = 0;
    return true;
  }
  if (committed > 0 && committed < requested) {
    memmove(sdBuffer, sdBuffer + committed, requested - committed);
    buffered = requested - committed;
  }
  if (lockTimeouts >= MAX_TRANSIENT_WRITE_FAILURES && committed == 0) {
    error = "storage_lock_timeout";
  } else {
    error = "sd_short_write_expected_" + String(requested) +
            "_got_" + String(committed);
  }
  return false;
}

bool writeCameraBytesToFile(WiFiClient &client, File &output, size_t count, size_t &written,
                            uint8_t *sdBuffer, size_t sdBufferSize, size_t &buffered,
                            String &error) {
  uint8_t buffer[1024];
  size_t remaining = count;
  unsigned long lastDataMs = millis();
  while (remaining > 0) {
    if (cachedStorageBatteryMv() < DURABLE_MEDIA_MIN_BATTERY_MV) {
      error = "low_battery_pause";
      return false;
    }
    const int available = client.available();
    if (available <= 0) {
      if (!client.connected() || millis() - lastDataMs > 15000UL) return false;
      cooperativeDelay(2);
      continue;
    }
    const size_t wanted = std::min(remaining,
                                   std::min(static_cast<size_t>(available), sizeof(buffer)));
    const int readCount = client.read(buffer, wanted);
    if (readCount <= 0) {
      cooperativeDelay(2);
      continue;
    }
    const size_t got = static_cast<size_t>(readCount);
    size_t copied = 0;
    while (copied < got) {
      const size_t space = sdBufferSize - buffered;
      const size_t take = std::min(space, got - copied);
      memcpy(sdBuffer + buffered, buffer + copied, take);
      buffered += take;
      copied += take;
      if (buffered == sdBufferSize &&
          !flushCameraSdBuffer(output, sdBuffer, buffered, error)) return false;
    }
    written += static_cast<size_t>(got);
    remaining -= static_cast<size_t>(got);
    durableTransferBytes = static_cast<uint32_t>(written);
    durableTransferLastProgressMs = millis();
    lastDataMs = millis();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}

bool durableStorageError(const String &error);

bool downloadCameraMediaToSd(const DurableMediaRecord &record, String &error, uint32_t &bytesWritten) {
  bytesWritten = 0;
  durableTransferBytes = 0;
  durableTransferExpectedBytes = record.expectedBytes;
  durableTransferStartedMs = millis();
  durableTransferLastProgressMs = durableTransferStartedMs;
  const String finalPath = durableMediaFilePath(record, false);
  const String partialPath = durableMediaFilePath(record, true);
  const bool isVideo = record.type == 2;
  const bool identityMatches = durableMediaIdentityMatches(finalPath, record);
  const uint32_t storedExpectedBytes = identityMatches ? readDurableMediaStoredSize(finalPath) : 0;
  const uint32_t knownExpectedBytes = record.expectedBytes > 0 ? record.expectedBytes : storedExpectedBytes;
  File existing = SD.open(finalPath, FILE_READ);
  if (existing) {
    const size_t size = existing.size();
    existing.close();
    if (identityMatches && size > 0 &&
        ((knownExpectedBytes > 0 && size == knownExpectedBytes) ||
         (!isVideo && knownExpectedBytes == 0 && validateJpegFileBasic(finalPath)))) {
      bytesWritten = size;
      error = "already_staged";
      return true;
    }
    if (identityMatches) {
      // Our own incomplete or corrupt copy of an item still on the camera.
      SD.remove(finalPath);
      SD.remove(durableMediaSizeSidecarPath(finalPath));
    } else {
      retireStaleStagedMedia(finalPath, record);
    }
  }
  // A partial file is only resumable if it was started from this exact camera
  // item. Partials are never the only copy of anything, so drop strangers.
  if (!identityMatches && SD.exists(partialPath)) {
    Serial.printf("[durable-job] discarding partial with unknown source %s\n", partialPath.c_str());
    SD.remove(partialPath);
  }
  size_t resumeOffset = 0;
  File partial = SD.open(partialPath, FILE_READ);
  if (partial) {
    resumeOffset = partial.size();
    partial.close();
    if (knownExpectedBytes > 0 && resumeOffset == knownExpectedBytes) {
      if ((isVideo && !validateMp4FileBasic(partialPath)) ||
          (!isVideo && !validateJpegFileBasic(partialPath))) {
        SD.remove(partialPath);
        SD.remove(durableMediaSizeSidecarPath(finalPath));
        resumeOffset = 0;
      } else {
        SD.remove(finalPath);
        if (SD.rename(partialPath, finalPath)) {
          invalidateTrailMediaUploadState(finalPath);
          bytesWritten = resumeOffset;
          writeDurableMediaStoredSize(finalPath, static_cast<uint32_t>(resumeOffset));
          error = "already_staged";
          return true;
        }
        error = "stage_rename_failed";
        return false;
      }
    }
    if (knownExpectedBytes > 0 && resumeOffset > knownExpectedBytes) {
      SD.remove(partialPath);
      resumeOffset = 0;
    }
  }
  // From this point onward the camera will supply at least part of the file,
  // so any terminal marker belonging to an older file at this path is stale.
  invalidateTrailMediaUploadState(finalPath);
  WiFiClient client;
  if (!client.connect(CAMERA_IP, CAMERA_HTTP_PORT)) {
    error = "camera_connect_failed";
    return false;
  }
  const String cameraPath = "/file/" + String(record.id) + (record.type == 1 ? "/JPG" : "/mp4");
  String request = "GET " + cameraPath + " HTTP/1.1\r\nHost: " + CAMERA_IP.toString() + ":" + String(CAMERA_HTTP_PORT) +
                   "\r\nUser-Agent: esp32-gardepro-durable/1\r\n";
  const bool requestRange = isVideo || resumeOffset > 0;
  if (requestRange) request += "Range: bytes=" + String(resumeOffset) + "-\r\n";
  request += "Connection: close\r\n\r\n";
  client.print(request);
  unsigned long waitStart = millis();
  while (!client.available() && client.connected() && millis() - waitStart < 10000UL) cooperativeDelay(10);
  if (!client.available()) {
    client.stop();
    error = "camera_header_timeout";
    return false;
  }
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  const int firstSpace = statusLine.indexOf(' ');
  const int statusCode = firstSpace >= 0 ? statusLine.substring(firstSpace + 1).toInt() : 0;
  int contentLength = -1;
  uint32_t contentRangeTotal = 0;
  bool chunked = false;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = line.substring(15).toInt();
    if (lower.startsWith("content-range:")) contentRangeTotal = parseContentRangeTotal(line);
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) chunked = true;
  }
  if (statusCode < 200 || statusCode >= 300) {
    client.stop();
    error = "camera_http_" + String(statusCode);
    return false;
  }
  const bool rangeAccepted = requestRange && statusCode == 206;
  if (resumeOffset > 0 && !rangeAccepted) {
    SD.remove(partialPath);
    resumeOffset = 0;
  }
  if (resumeOffset == 0 && !writeDurableMediaIdentity(finalPath, record)) {
    client.stop();
    error = "sd_identity_write_failed";
    return false;
  }
  uint32_t expectedTotalBytes = record.expectedBytes;
  if (expectedTotalBytes == 0 && contentRangeTotal > 0) {
    expectedTotalBytes = contentRangeTotal;
  }
  if (expectedTotalBytes == 0 && contentLength >= 0 && !chunked) {
    expectedTotalBytes = static_cast<uint32_t>((rangeAccepted ? resumeOffset : 0) + static_cast<size_t>(contentLength));
  }
  durableTransferBytes = static_cast<uint32_t>(resumeOffset);
  durableTransferExpectedBytes = expectedTotalBytes;
  durableTransferLastProgressMs = millis();
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    client.stop();
    error = "storage_busy";
    return false;
  }
  cameraSdTransferActive = true;
  File output = SD.open(partialPath, rangeAccepted ? FILE_APPEND : FILE_WRITE);
  if (!output) cameraSdTransferActive = false;
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  bool ok = static_cast<bool>(output);
  size_t written = resumeOffset;
  // Bound each synchronous SD write. 32 KiB bursts have taken multiple
  // watchdog periods on cards whose erase/program latency spikes.
  size_t sdBufferSize = 4096;
  uint8_t *sdBuffer = static_cast<uint8_t *>(heap_caps_malloc(sdBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (sdBuffer == nullptr) {
    sdBufferSize = 2048;
    sdBuffer = static_cast<uint8_t *>(malloc(sdBufferSize));
  }
  size_t buffered = 0;
  if (sdBuffer == nullptr) { ok = false; error = "sd_buffer_allocation_failed"; }
  if (ok && chunked) {
    while (true) {
      // Some GardePro firmware inserts an extra blank separator (observed
      // consistently after 47,104 bytes), and can also pause while reading its
      // card. Ignore blank lines within a bounded window; a non-empty malformed
      // chunk header remains a hard protocol error.
      String sizeLine;
      const unsigned long chunkHeaderStartedMs = millis();
      while (sizeLine.isEmpty() &&
             millis() - chunkHeaderStartedMs < 60000UL) {
        sizeLine = client.readStringUntil('\n');
        sizeLine.trim();
        if (sizeLine.isEmpty()) {
          if (!client.connected() && !client.available()) break;
          feedTaskWdt();
          cooperativeDelay(5);
        }
      }
      const long chunkSize = parseHttpChunkSize(sizeLine);
      if (chunkSize < 0) {
        ok = false;
        // Record what the camera actually sent so the recurring 47,104-byte
        // boundary can be diagnosed from job status.
        String hex;
        for (size_t i = 0; i < sizeLine.length() && i < 12; ++i) {
          char part[3];
          snprintf(part, sizeof(part), "%02x", static_cast<uint8_t>(sizeLine[i]));
          hex += part;
        }
        error = "invalid_chunk_len" + String(sizeLine.length()) + "_hex" + hex +
                (client.connected() ? "" : "_closed");
        Serial.printf("[durable-job] invalid chunk header at %u: %s\n",
                      static_cast<unsigned>(written), error.c_str());
        break;
      }
      if (chunkSize == 0) break;
      if (!writeCameraBytesToFile(client, output, static_cast<size_t>(chunkSize), written,
                                  sdBuffer, sdBufferSize, buffered, error)) {
        ok = false;
        if (error.isEmpty()) {
          error = "chunk_read_failed_size_" + String(chunkSize) + "_total_" + String(written);
        }
        break;
      }
      uint8_t chunkTerminator[2];
      if (client.readBytes(chunkTerminator, sizeof(chunkTerminator)) != sizeof(chunkTerminator)) {
        ok = false; error = "chunk_terminator_failed"; break;
      }
    }
  } else if (ok && contentLength >= 0) {
    ok = writeCameraBytesToFile(client, output, static_cast<size_t>(contentLength), written,
                                sdBuffer, sdBufferSize, buffered, error);
    if (!ok && error.isEmpty()) error = "body_read_failed";
  } else if (ok) {
    uint8_t buffer[2048];
    unsigned long lastDataMs = millis();
    while (client.connected() || client.available()) {
      if (cachedStorageBatteryMv() < DURABLE_MEDIA_MIN_BATTERY_MV) {
        ok = false;
        error = "low_battery_pause";
        break;
      }
      const int available = client.available();
      if (available <= 0) {
        if (millis() - lastDataMs > 15000UL) { ok = false; error = "body_timeout"; break; }
        cooperativeDelay(5); continue;
      }
      const size_t wanted = std::min(static_cast<size_t>(available), sizeof(buffer));
      const int got = client.read(buffer, wanted);
      if (got > 0) {
        size_t copied = 0;
        while (copied < static_cast<size_t>(got)) {
          const size_t space = sdBufferSize - buffered;
          const size_t take = std::min(space, static_cast<size_t>(got) - copied);
          memcpy(sdBuffer + buffered, buffer + copied, take);
          buffered += take;
          copied += take;
          if (buffered == sdBufferSize &&
              !flushCameraSdBuffer(output, sdBuffer, buffered, error)) {
            ok = false;
            if (error.isEmpty()) error = "sd_write_failed";
            break;
          }
        }
        if (!ok) break;
        written += got;
        durableTransferBytes = static_cast<uint32_t>(written);
        durableTransferLastProgressMs = millis();
        lastDataMs = millis();
        taskYIELD();
      }
    }
  }
  String finalFlushError;
  if (output && !flushCameraSdBuffer(output, sdBuffer, buffered, finalFlushError)) {
    ok = false;
    if (error.isEmpty()) error = finalFlushError;
    else error += ";" + finalFlushError;
  }
  if (sdBuffer != nullptr) free(sdBuffer);
  if (output) vTaskDelay(pdMS_TO_TICKS(1));
  if (output && (onboardStorageMutex == nullptr ||
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE)) {
    output.flush();
    output.close();
    cameraSdTransferActive = false;
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  // Never leave background SD users suspended after an error path. File's
  // destructor closes any handle that could not be closed above.
  cameraSdTransferActive = false;
  client.stop();
  // `written` counts bytes received from the camera, including bytes still
  // sitting in the SD buffer when a write failed. Only the file size on the
  // card is evidence of what was stored.
  const size_t onDisk = durableSdFileSize(partialPath);
  if (ok && written != onDisk) {
    ok = false;
    error = "sd_stored_size_mismatch_received_" + String(written) + "_stored_" + String(onDisk);
  }
  if (ok && onDisk == 0) {
    ok = false;
    error = "empty_body";
  }
  if (ok && expectedTotalBytes > 0 && onDisk != expectedTotalBytes) {
    ok = false;
    error = "size_mismatch_expected_" + String(expectedTotalBytes) + "_got_" + String(onDisk);
  }
  if (ok && isVideo && expectedTotalBytes == 0) {
    ok = false;
    error = "video_size_unknown";
  }
  if (ok && isVideo && !validateMp4FileBasic(partialPath)) {
    ok = false;
    error = "mp4_validation_failed";
    SD.remove(partialPath);
  }
  if (ok && !isVideo && !validateJpegFileBasic(partialPath)) {
    ok = false;
    error = "jpeg_validation_failed";
    SD.remove(partialPath);
  }
  if (ok) {
    SD.remove(finalPath);
    ok = SD.rename(partialPath, finalPath);
    if (!ok) error = "stage_rename_failed";
    else if (expectedTotalBytes > 0) writeDurableMediaStoredSize(finalPath, expectedTotalBytes);
  }
  // Storage faults must reach durableMediaItemFailed() unprefixed so they
  // trigger a remount; otherwise a dead card looked like endless progress.
  if (!ok && onDisk > resumeOffset && !durableStorageError(error) &&
      !error.endsWith("_validation_failed") &&
      (expectedTotalBytes == 0 || onDisk < expectedTotalBytes)) {
    error = "partial_progress_" + String(onDisk - resumeOffset) + ":" + error;
  }
  bytesWritten = static_cast<uint32_t>(onDisk);
  refreshSdCapacityCache();
  return ok;
}

bool galleryJsonContainsRecord(const String &galleryJson, const DurableMediaRecord &record) {
  int position = 0;
  while (true) {
    const int objectStart = galleryJson.indexOf('{', position);
    if (objectStart < 0) return false;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    int objectEnd = -1;
    for (int i = objectStart; i < static_cast<int>(galleryJson.length()); ++i) {
      const char c = galleryJson[i];
      if (escaped) { escaped = false; continue; }
      if (c == '\\' && inString) { escaped = true; continue; }
      if (c == '"') { inString = !inString; continue; }
      if (inString) continue;
      if (c == '{') ++depth;
      else if (c == '}' && --depth == 0) { objectEnd = i; break; }
    }
    if (objectEnd < 0) return false;
    const String objectJson = galleryJson.substring(objectStart, objectEnd + 1);
    if (jsonObjectIntField(objectJson, "id", -1) == static_cast<long>(record.id) &&
        jsonObjectIntField(objectJson, "type", -1) == static_cast<long>(record.type)) return true;
    position = objectEnd + 1;
  }
}

bool deleteCameraMediaRecord(const DurableMediaRecord &record, String &error) {
  String body;
  int statusCode = 0;
  const String ext = record.type == 1 ? "JPG" : "mp4";
  proxyCameraRequest("GET", "/cmd/delete/" + String(record.id) + "/" + ext, body, statusCode);
  if (statusCode >= 200 && statusCode < 300 && jsonObjectIntField(body, "code", -1) == 0) return true;
  proxyCameraRequest("GET", "/cmd/delete/" + String(record.type) + "/" + String(record.id), body, statusCode);
  if (statusCode >= 200 && statusCode < 300 && jsonObjectIntField(body, "code", -1) == 0) return true;
  String gallery;
  int galleryStatus = 0;
  // The listing is "IDs <= cursor, newest first", so a cursor at this ID
  // returns the item itself if it still exists.
  if (proxyCameraRequest("GET", "/list/detail/backward/" + String(record.id) + "/3", gallery, galleryStatus) &&
      galleryStatus >= 200 && galleryStatus < 300 && gallery.indexOf("\"data\"") >= 0 &&
      !galleryJsonContainsRecord(gallery, record)) {
    return true;
  }
  error = "delete_failed_http_" + String(statusCode);
  return false;
}

bool sha256SdFile(const String &path, String &hexDigest, uint32_t &fileBytes) {
  hexDigest = "";
  fileBytes = 0;
  trailMediaUploadBytesHashed = 0;
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  File file = SD.open(path, FILE_READ);
  if (!file) {
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }
  const uint64_t size = file.size();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  if (size == 0 || size > UINT32_MAX) {
    if (onboardStorageMutex == nullptr ||
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      file.close();
      if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    }
    return false;
  }
  fileBytes = static_cast<uint32_t>(size);
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  // Hash in large PSRAM-backed chunks. The old 512-byte/10 ms pacing added
  // more than three minutes of artificial delay to a 10 MB video.
  static const size_t HASH_CHUNK_BYTES = 16384;
  uint8_t *buffer = static_cast<uint8_t *>(
    heap_caps_malloc(HASH_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) buffer = static_cast<uint8_t *>(malloc(HASH_CHUNK_BYTES));
  if (buffer == nullptr) {
    mbedtls_sha256_free(&context);
    if (onboardStorageMutex == nullptr ||
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      file.close();
      if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    }
    return false;
  }
  bool readOk = true;
  while (trailMediaUploadBytesHashed < fileBytes) {
    if (trailMediaUploadAbortRequested || durableMediaJobIsActive()) {
      readOk = false;
      break;
    }
    const size_t remaining = fileBytes - trailMediaUploadBytesHashed;
    const size_t requested = remaining < HASH_CHUNK_BYTES ? remaining : HASH_CHUNK_BYTES;
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      readOk = false;
      break;
    }
    const int count = file.read(buffer, requested);
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    if (count <= 0) { readOk = false; break; }
    mbedtls_sha256_update(&context, buffer, static_cast<size_t>(count));
    trailMediaUploadBytesHashed += static_cast<uint32_t>(count);
    feedTaskWdt();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);
  free(buffer);
  if (onboardStorageMutex == nullptr ||
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    file.close();
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  } else {
    readOk = false;
  }
  char encoded[65];
  for (size_t i = 0; i < sizeof(digest); ++i) snprintf(encoded + i * 2, 3, "%02x", digest[i]);
  encoded[64] = '\0';
  hexDigest = encoded;
  return readOk && trailMediaUploadBytesHashed == fileBytes;
}

bool readOutboundHttpResponse(int sock, int &statusCode, String &body) {
  statusCode = 0;
  body = "";
  String raw;
  uint8_t buffer[512];
  const unsigned long started = millis();
  while (millis() - started < 15000UL) {
    const int count = recv(sock, buffer, sizeof(buffer), 0);
    if (count > 0) {
      raw.concat(reinterpret_cast<const char *>(buffer), count);
      continue;
    }
    if (count == 0) break;
    if (errno != EAGAIN && errno != EWOULDBLOCK) break;
    cooperativeDelay(5);
  }
  const int headerEnd = raw.indexOf("\r\n\r\n");
  if (headerEnd < 0) return false;
  const int firstLineEnd = raw.indexOf("\r\n");
  const String statusLine = raw.substring(0, firstLineEnd >= 0 ? firstLineEnd : headerEnd);
  const int firstSpace = statusLine.indexOf(' ');
  statusCode = firstSpace >= 0 ? statusLine.substring(firstSpace + 1).toInt() : 0;
  body = raw.substring(headerEnd + 4);
  return statusCode > 0;
}

bool putTrailMediaBlob(const String &uploadUrl, const String &mediaPath,
                       const String &contentType, const String &sha256,
                       uint32_t fileBytes, String &error,
                       UpstreamTransport transport) {
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) { error = "blob_socket_failed"; return false; }
  if (!bindSocketToTransport(sock, transport)) {
    close(sock); error = "blob_transport_bind_failed"; return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = inet_addr(UPSTREAM_API_HOST);
  address.sin_port = htons(UPSTREAM_API_PORT);
  int connectError = 0;
  if (!connectSocketWithTimeout(sock, address, 10000UL, connectError)) {
    close(sock); error = "blob_connect_failed_" + String(connectError); return false;
  }
  timeval timeout{};
  // Keep each blocking send shorter than the task watchdog interval. The
  // media sender retries transient backpressure for up to 90 seconds.
  timeout.tv_sec = 1;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  String requestPath = uploadUrl;
  if (!requestPath.startsWith(String(UPSTREAM_API_PREFIX) + "/")) requestPath = normalizeApiPath(requestPath);
  String headers = "PUT " + requestPath + " HTTP/1.1\r\n";
  headers += "Host: " + String(UPSTREAM_API_HOST) + ":" + String(UPSTREAM_API_PORT) + "\r\n";
  headers += "User-Agent: esp32-gardepro-media/1\r\n";
  headers += "Content-Type: " + contentType + "\r\n";
  headers += "Content-Length: " + String(fileBytes) + "\r\n";
  headers += "X-SHA256: " + sha256 + "\r\n";
  if (strlen(UPSTREAM_API_TOKEN) > 0) headers += "Authorization: Bearer " + String(UPSTREAM_API_TOKEN) + "\r\n";
  headers += "Accept: application/json\r\nConnection: close\r\n\r\n";
  if (!sendAll(sock, reinterpret_cast<const uint8_t *>(headers.c_str()), headers.length())) {
    close(sock); error = "blob_header_send_failed"; return false;
  }
  // Never allow HaLow backpressure to trap this task inside lwIP send() long
  // enough to trip the task watchdog. The media-specific sender below handles
  // EAGAIN/EWOULDBLOCK with bounded retries and explicit watchdog service.
  const int socketFlags = fcntl(sock, F_GETFL, 0);
  if (socketFlags < 0 || fcntl(sock, F_SETFL, socketFlags | O_NONBLOCK) != 0) {
    close(sock); error = "blob_nonblocking_setup_failed_" + String(errno); return false;
  }
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    close(sock); error = "blob_storage_busy"; return false;
  }
  File file = SD.open(mediaPath, FILE_READ);
  bool sent = static_cast<bool>(file);
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  // HaLow needs conservative submissions; normal 2.4 GHz Wi-Fi can safely use
  // larger chunks and should not inherit HaLow's throughput throttling.
  const size_t uploadChunkBytes = transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI ? 4096 : 512;
  const TickType_t uploadYieldTicks = pdMS_TO_TICKS(
    transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI ? 1 : 10);
  uint8_t *buffer = static_cast<uint8_t *>(malloc(uploadChunkBytes));
  if (buffer == nullptr) {
    if (file && (onboardStorageMutex == nullptr ||
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE)) {
      file.close();
      if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    }
    close(sock);
    error = "blob_buffer_alloc_failed";
    return false;
  }
  trailMediaUploadBytesSent = 0;
  while (sent && trailMediaUploadBytesSent < fileBytes) {
    if (trailMediaUploadAbortRequested || durableMediaJobIsActive()) {
      sent = false;
      error = "camera_job_preempted";
      break;
    }
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      sent = false;
      error = "blob_storage_busy";
      break;
    }
    const int count = file.read(buffer, uploadChunkBytes);
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    if (count <= 0 || !sendMediaChunkWithRetry(sock, buffer, static_cast<size_t>(count), error)) {
      sent = false;
      break;
    }
    trailMediaUploadBytesSent += static_cast<uint32_t>(count);
    feedTaskWdt();
    // Leave airtime and lwIP servicing capacity for the board's management
    // API and HaLow driver tasks. Saturating TX can starve the other core's
    // web response path and trip its idle-task watchdog.
    vTaskDelay(uploadYieldTicks);
  }
  free(buffer);
  if (file && (onboardStorageMutex == nullptr ||
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) == pdTRUE)) {
    file.close();
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  }
  if (!sent || trailMediaUploadBytesSent != fileBytes) {
    close(sock);
    if (error.isEmpty()) error = "blob_send_failed_" + String(trailMediaUploadBytesSent);
    else error += "_at_" + String(trailMediaUploadBytesSent);
    return false;
  }
  int statusCode = 0;
  String response;
  fcntl(sock, F_SETFL, socketFlags);
  const bool responseOk = readOutboundHttpResponse(sock, statusCode, response);
  close(sock);
  if (!responseOk || statusCode < 200 || statusCode >= 300) {
    error = "blob_http_" + String(statusCode);
    return false;
  }
  Serial.printf("[trail-upload] PUT complete bytes=%u status=%d transport=%s\n",
                fileBytes, statusCode,
                transport == UPSTREAM_TRANSPORT_TRANSFER_WIFI ? "wifi_2_4ghz" : "halow");
  return true;
}

bool loadTrailMediaUploadCursor(uint32_t jobId, uint32_t &position) {
  position = 0;
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  File file = SD.open(TRAIL_MEDIA_UPLOAD_CURSOR_PATH, FILE_READ);
  TrailMediaUploadCursorDisk state{};
  const bool readOk = file &&
                      file.read(reinterpret_cast<uint8_t *>(&state), sizeof(state)) == sizeof(state);
  if (file) file.close();
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  const uint32_t expectedCheck = state.magic ^ state.jobId ^ state.position ^ 0xA55A5AA5UL;
  if (!readOk || state.magic != TRAIL_MEDIA_UPLOAD_CURSOR_MAGIC ||
      state.jobId != jobId || state.check != expectedCheck) return false;
  position = state.position;
  return true;
}

bool saveTrailMediaUploadCursor(uint32_t jobId, uint32_t position) {
  TrailMediaUploadCursorDisk state{};
  state.magic = TRAIL_MEDIA_UPLOAD_CURSOR_MAGIC;
  state.jobId = jobId;
  state.position = position;
  state.check = state.magic ^ state.jobId ^ state.position ^ 0xA55A5AA5UL;
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  File file = SD.open(TRAIL_MEDIA_UPLOAD_CURSOR_PATH, "r+");
  if (!file) file = SD.open(TRAIL_MEDIA_UPLOAD_CURSOR_PATH, FILE_WRITE);
  bool writeOk = false;
  if (file) {
    file.seek(0);
    writeOk = file.write(reinterpret_cast<const uint8_t *>(&state), sizeof(state)) == sizeof(state);
    if (writeOk) file.flush();
    file.close();
  }
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  return writeOk;
}

bool findPendingTrailMedia(String &mediaPath, String &filename) {
  mediaPath = "";
  filename = "";
  // Completed durable jobs already contain an authoritative, bounded manifest.
  // Prefer it over walking the FAT directory, which can be very slow or wedge
  // on large cards with many sidecar/marker entries.
  if (durableMediaJob.manifestCount > 0) {
    std::vector<DurableMediaRecord> records;
    records.reserve(durableMediaJob.manifestCount);
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    File manifest = SD.open(DURABLE_JOB_MANIFEST_PATH, FILE_READ);
    if (manifest) {
      DurableMediaRecord record{};
      while (records.size() < durableMediaJob.manifestCount &&
             manifest.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) == sizeof(record)) {
        records.push_back(record);
      }
      manifest.close();
    }
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    Serial.printf("[trail-upload] manifest loaded records=%u expected=%u\n",
                  static_cast<unsigned>(records.size()), durableMediaJob.manifestCount);
    if (records.empty()) return false;
    std::sort(records.begin(), records.end(),
              [](const DurableMediaRecord &left, const DurableMediaRecord &right) {
      const uint32_t leftBytes = left.expectedBytes == 0 ? UINT32_MAX : left.expectedBytes;
      const uint32_t rightBytes = right.expectedBytes == 0 ? UINT32_MAX : right.expectedBytes;
      if (leftBytes != rightBytes) return leftBytes < rightBytes;
      // For equal/unknown sizes, try still images before videos.
      if (left.type != right.type) return left.type == 1;
      return left.id < right.id;
    });
    Serial.println("[trail-upload] manifest sorted by ascending size");
    if (!trailMediaUploadCursorLoaded ||
        trailMediaUploadCursorJobId != durableMediaJob.jobId) {
      trailMediaUploadCursorJobId = durableMediaJob.jobId;
      if (!loadTrailMediaUploadCursor(durableMediaJob.jobId,
                                      trailMediaUploadCursorPosition)) {
        trailMediaUploadCursorPosition = 0;
      }
      trailMediaUploadCursorLoaded = true;
      Serial.printf("[trail-upload] cursor restored job=%u position=%u\n",
                    trailMediaUploadCursorJobId, trailMediaUploadCursorPosition);
    }
    if (trailMediaUploadCursorPosition >= records.size()) {
      return false;
    }
    const DurableMediaRecord &record = records[trailMediaUploadCursorPosition];
    mediaPath = durableMediaFilePath(record);
    const int slash = mediaPath.lastIndexOf('/');
    filename = slash >= 0 ? mediaPath.substring(slash + 1) : mediaPath;
    return true;
  }
  uint64_t smallestBytes = UINT64_MAX;
  if (onboardStorageMutex != nullptr &&
      xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  File directory = SD.open(TRAIL_MEDIA_DIR, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    return false;
  }
  if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
  while (true) {
    if (onboardStorageMutex != nullptr &&
        xSemaphoreTake(onboardStorageMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    File file = directory.openNextFile(FILE_READ);
    if (!file) {
      directory.close();
      if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
      break;
    }
    String fullPath = file.name();
    if (!fullPath.startsWith("/")) fullPath = String(TRAIL_MEDIA_DIR) + "/" + fullPath;
    const int slash = fullPath.lastIndexOf('/');
    const String name = slash >= 0 ? fullPath.substring(slash + 1) : fullPath;
    const uint64_t bytes = file.isDirectory() ? 0 : file.size();
    String lowerName = name;
    lowerName.toLowerCase();
    const bool media = !file.isDirectory() &&
                       (lowerName.endsWith(".jpg") || lowerName.endsWith(".jpeg") ||
                        lowerName.endsWith(".mp4") || lowerName.endsWith(".avi"));
    file.close();
    if (media && bytes > 0 && bytes <= UINT32_MAX &&
        bytes < smallestBytes && !SD.exists(fullPath + ".uploaded") &&
        !SD.exists(fullPath + ".upload_failed")) {
      smallestBytes = bytes;
      mediaPath = fullPath;
      filename = name;
    }
    if (onboardStorageMutex != nullptr) xSemaphoreGive(onboardStorageMutex);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return !mediaPath.isEmpty();
}

void advanceTrailMediaUploadCursor() {
  if (!trailMediaUploadCursorLoaded ||
      trailMediaUploadCursorJobId != durableMediaJob.jobId) return;
  ++trailMediaUploadCursorPosition;
  trailMediaUploadCursorAttempts = 0;
  if (!saveTrailMediaUploadCursor(trailMediaUploadCursorJobId,
                                  trailMediaUploadCursorPosition)) {
    Serial.printf("[trail-upload] cursor save failed job=%u position=%u\n",
                  trailMediaUploadCursorJobId, trailMediaUploadCursorPosition);
  }
}

bool ensureTransferWifiConnected() {
  if (!TRANSFER_WIFI_UPLOAD_ENABLED) {
    transferWifiLastError = "automatic_upload_disabled_after_radio_panic";
    return false;
  }
  if (wifiRadioMutex != nullptr &&
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    transferWifiLastError = "wifi_radio_busy";
    return false;
  }
  refreshWifiState();
  if (transferWifiConnected) {
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
    return true;
  }
  if (strlen(TRANSFER_WIFI_SSID) == 0) {
    transferWifiLastError = "not_configured";
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
    return false;
  }
  ++transferWifiConnectAttempts;
  transferWifiLastError = "";
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  // Undo any 11b-only setting left from a camera session.
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  WiFi.setHostname(BOARD_HOSTNAME);
  WiFi.setSleep(false);
  cooperativeDelay(200);
  WiFi.begin(TRANSFER_WIFI_SSID, TRANSFER_WIFI_PASS);
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000UL) {
    if (trailMediaUploadAbortRequested || durableMediaJobIsActive()) {
      transferWifiLastError = "camera_job_preempted";
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      refreshWifiState();
      if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
      return false;
    }
    cooperativeDelay(100);
  }
  refreshWifiState();
  if (!transferWifiConnected) {
    transferWifiLastError = "connect_failed";
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    refreshWifiState();
    Serial.println("[trail-upload] transfer WiFi unavailable; falling back to HaLow");
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
    return false;
  }
  transferWifiSessionFiles = 0;
  transferWifiSessionStartedMs = millis();
  Serial.printf("[trail-upload] transfer WiFi connected ip=%s\n", WiFi.localIP().toString().c_str());
  if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
  return true;
}

void disconnectTransferWifi() {
  if (wifiRadioMutex != nullptr &&
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[trail-upload] WiFi radio busy; disconnect deferred");
    return;
  }
  refreshWifiState();
  if (!transferWifiConnected) {
    if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
    return;
  }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  transferWifiSessionFiles = 0;
  transferWifiSessionStartedMs = 0;
  refreshWifiState();
  Serial.println("[trail-upload] transfer WiFi disconnected");
  if (wifiRadioMutex != nullptr) xSemaphoreGiveRecursive(wifiRadioMutex);
}

void serviceTrailMediaUpload() {
  trailMediaUploadBlockedReason = "";
  if (trailMediaRequeueActive) { trailMediaUploadBlockedReason = "requeue_in_progress"; return; }
  if (cachedStorageBatteryMv() < MEDIA_UPLOAD_MIN_BATTERY_MV) {
    trailMediaUploadBlockedReason = "battery_below_upload_threshold";
    return;
  }
  if (trailMediaUploadPauseUntilMs != 0 &&
      static_cast<long>(trailMediaUploadPauseUntilMs - millis()) > 0) {
    trailMediaUploadBlockedReason = "maintenance_pause";
    return;
  }
  if (!sdReady) { trailMediaUploadBlockedReason = "sd_unavailable"; return; }
  if (!durableMediaJobLoaded) { trailMediaUploadBlockedReason = "durable_job_not_loaded"; return; }
  if (durableMediaJobIsActive()) { trailMediaUploadBlockedReason = "camera_job_active"; return; }
  trailMediaUploadAbortRequested = false;
  if (trailMediaUploadActive) return;
  if (isControlActionActive()) { trailMediaUploadBlockedReason = "control_action_active"; return; }
  if (streamSessionActive) { trailMediaUploadBlockedReason = "live_view_active"; return; }
  if (wifiConnected) { trailMediaUploadBlockedReason = "camera_wifi_active"; return; }
  if (trailMediaUploadLastAttemptMs != 0 &&
      millis() - trailMediaUploadLastAttemptMs < TRAIL_MEDIA_UPLOAD_RETRY_MS) return;
  trailMediaUploadLastAttemptMs = millis();
  String mediaPath;
  String filename;
  Serial.printf("[trail-upload] queue scan begin job=%u manifest=%u cursor=%u\n",
                durableMediaJob.jobId, durableMediaJob.manifestCount,
                trailMediaUploadCursorPosition);
  if (!findPendingTrailMedia(mediaPath, filename)) {
    Serial.printf("[trail-upload] queue scan found no candidate job=%u cursor=%u\n",
                  durableMediaJob.jobId, trailMediaUploadCursorPosition);
    disconnectTransferWifi();
    return;
  }
  if (durableMediaJobIsActive()) {
    trailMediaUploadBlockedReason = "camera_job_active";
    disconnectTransferWifi();
    return;
  }
  trailMediaUploadActive = true;
  trailMediaUploadFilename = filename;
  trailMediaUploadBytesSent = 0;
  trailMediaUploadFileBytes = 0;
  trailMediaUploadBytesHashed = 0;
  trailMediaUploadLastError = "";
  // Do not change either core's watchdog state here. Those APIs have
  // deadlocked this worker on the vendor dual-radio runtime. Hashing and
  // socket loops yield/feed normally, while the independent failsafe watches
  // byte progress and can restart a genuinely stalled transfer.
  Serial.printf("[trail-upload] selected file=%s\n", filename.c_str());
  bool transferRadioLeaseHeld = false;
  if (TRANSFER_WIFI_UPLOAD_ENABLED && strlen(TRANSFER_WIFI_SSID) > 0 &&
      wifiRadioMutex != nullptr) {
    transferRadioLeaseHeld =
      xSemaphoreTakeRecursive(wifiRadioMutex, pdMS_TO_TICKS(5000)) == pdTRUE;
    if (!transferRadioLeaseHeld) transferWifiLastError = "radio_lease_busy";
  }
  bool usingTransferWifi = transferRadioLeaseHeld && ensureTransferWifiConnected();
  trailMediaUploadPreferredTransport = usingTransferWifi ? "wifi_2_4ghz" : "halow";
  Serial.printf("[trail-upload] transport=%s file=%s\n",
                usingTransferWifi ? "wifi_2_4ghz" : "halow", filename.c_str());
  String sha256;
  uint32_t fileBytes = 0;
  bool ok = sha256SdFile(mediaPath, sha256, fileBytes);
  trailMediaUploadFileBytes = fileBytes;
  Serial.printf("[trail-upload] hash %s file=%s bytes=%u\n",
                ok ? "complete" : "failed", filename.c_str(), fileBytes);
  if (!ok) {
    trailMediaUploadLastError = (trailMediaUploadAbortRequested || durableMediaJobIsActive())
                                  ? "camera_job_preempted"
                                  : "sha256_failed";
  }
  String lowerFilename = filename;
  lowerFilename.toLowerCase();
  const String contentType = lowerFilename.endsWith(".mp4") ? "video/mp4" :
                             (lowerFilename.endsWith(".avi") ? "video/x-msvideo" : "image/jpeg");
  String metadata = "{";
  metadata += "\"scanner_host\":\"" + jsonEscape(SCANNER_HOST) + "\"";
  metadata += ",\"source\":\"trail_camera\"";
  metadata += ",\"media_type\":\"" + contentType + "\"";
  metadata += ",\"filename\":\"" + jsonEscape(filename) + "\"";
  metadata += ",\"recorded_at\":" + buildRecordedAtJsonValue();
  metadata += ",\"bytes\":" + String(fileBytes);
  metadata += ",\"sha256\":\"" + sha256 + "\"";
  metadata += ",\"metadata\":{\"trail_media_id\":" + String(filename.toInt()) + "}}";
  String response;
  int statusCode = 0;
  if (ok && !trailMediaUploadAbortRequested && !durableMediaJobIsActive()) {
    const UpstreamTransport uploadTransport = usingTransferWifi
                                                ? UPSTREAM_TRANSPORT_TRANSFER_WIFI
                                                : UPSTREAM_TRANSPORT_HALOW;
    ok = postJsonToUpstream("/api/board/media", metadata, response, statusCode,
                            uploadTransport);
    if (!ok && usingTransferWifi) {
      Serial.printf("[trail-upload] metadata failed over WiFi status=%d; retrying over HaLow\n",
                    statusCode);
      disconnectTransferWifi();
      usingTransferWifi = false;
      response = "";
      statusCode = 0;
      ok = postJsonToUpstream("/api/board/media", metadata, response, statusCode,
                              UPSTREAM_TRANSPORT_HALOW);
    }
    if (!ok) trailMediaUploadLastError = "metadata_http_" + String(statusCode);
  }
  bool wifiBlobAttempted = false;
  if (ok && !trailMediaUploadAbortRequested && !durableMediaJobIsActive()) {
    const String uploadUrl = jsonObjectStringField(response, "upload_url");
    if (!uploadUrl.isEmpty()) {
      wifiBlobAttempted = usingTransferWifi;
      ok = putTrailMediaBlob(uploadUrl, mediaPath, contentType, sha256, fileBytes,
                             trailMediaUploadLastError,
                             usingTransferWifi ? UPSTREAM_TRANSPORT_TRANSFER_WIFI
                                               : UPSTREAM_TRANSPORT_HALOW);
    } else {
      ok = false;
      trailMediaUploadLastError = "metadata_missing_upload_url";
    }
  }
  if (!ok && usingTransferWifi && wifiBlobAttempted) {
    const String wifiError = trailMediaUploadLastError;
    Serial.printf("[trail-upload] WiFi blob failed error=%s; retrying file over HaLow\n",
                  wifiError.c_str());
    disconnectTransferWifi();
    usingTransferWifi = false;
    trailMediaUploadPreferredTransport = "halow_fallback";
    response = "";
    statusCode = 0;
    ok = postJsonToUpstream("/api/board/media", metadata, response, statusCode,
                            UPSTREAM_TRANSPORT_HALOW);
    if (ok) {
      const String uploadUrl = jsonObjectStringField(response, "upload_url");
      if (!uploadUrl.isEmpty()) {
        trailMediaUploadLastError = "";
        ok = putTrailMediaBlob(uploadUrl, mediaPath, contentType, sha256, fileBytes,
                               trailMediaUploadLastError, UPSTREAM_TRANSPORT_HALOW);
      } else {
        ok = false;
        trailMediaUploadLastError = "halow_metadata_missing_upload_url";
      }
    } else {
      trailMediaUploadLastError = "wifi_" + wifiError +
                                  "_halow_metadata_http_" + String(statusCode);
    }
  }
  if (ok) {
    ++trailMediaUploadSuccessCount;
    ++transferWifiSessionFiles;
    trailMediaUploadLastSuccessMs = millis();
    advanceTrailMediaUploadCursor();
    // A successful item is not a retry. Continue draining the queue on the
    // next worker pass instead of imposing the failure backoff per file.
    trailMediaUploadLastAttemptMs = 0;
    Serial.printf("[trail-upload] complete file=%s bytes=%u\n", filename.c_str(), fileBytes);
  } else {
    ++trailMediaUploadFailureCount;
    if (trailMediaUploadCursorAttempts < UINT8_MAX) ++trailMediaUploadCursorAttempts;
    const uint8_t fileAttempts = trailMediaUploadCursorAttempts;
    if (fileAttempts >= TRAIL_MEDIA_MAX_UPLOAD_ATTEMPTS) {
      ++trailMediaUploadSkippedCount;
      trailMediaUploadLastSkipped = filename;
      advanceTrailMediaUploadCursor();
      Serial.printf("[trail-upload] skipping file=%s after %u attempts\n",
                    filename.c_str(), fileAttempts);
    }
    Serial.printf("[trail-upload] failed file=%s error=%s\n",
                  filename.c_str(), trailMediaUploadLastError.c_str());
  }
  trailMediaUploadActive = false;
  trailMediaUploadAbortRequested = false;
  const bool rotateTransferWifi = !ok ||
                                  transferWifiSessionFiles >= TRANSFER_WIFI_BATCH_FILES ||
                                  (transferWifiSessionStartedMs != 0 &&
                                   millis() - transferWifiSessionStartedMs >= TRANSFER_WIFI_SESSION_MAX_MS);
  if (rotateTransferWifi) disconnectTransferWifi();
  if (transferRadioLeaseHeld && wifiRadioMutex != nullptr) {
    xSemaphoreGiveRecursive(wifiRadioMutex);
  }
}

void trailMediaUploadTask(void *pvParameters) {
  (void)pvParameters;
  Serial.printf("[trail-upload] task started core=%d\n", xPortGetCoreID());
  while (true) {
    serviceTrailMediaUpload();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void startTrailMediaUploadTask() {
  if (trailMediaUploadTaskHandle != nullptr) return;
  const BaseType_t result = xTaskCreatePinnedToCore(trailMediaUploadTask,
                                                    "trail-upload",
                                                    8192,
                                                    nullptr,
                                                    1,
                                                    &trailMediaUploadTaskHandle,
                                                    TRAIL_UPLOAD_WORKER_CORE);
  Serial.printf("[trail-upload] task create result=%d handle=%p core=%d\n",
                static_cast<int>(result), trailMediaUploadTaskHandle,
                static_cast<int>(TRAIL_UPLOAD_WORKER_CORE));
}

void triggerFailsafeRestart(const char *reason) {
  const String nextReason = reason == nullptr ? "unknown" : String(reason);
  if (nextReason == failsafeLastReason) {
    ++failsafeConsecutiveSameReason;
  } else {
    failsafeConsecutiveSameReason = 1;
  }
  failsafeLastReason = nextReason;
  ++failsafeRestartCount;
  Preferences failsafePrefs;
  if (failsafePrefs.begin("runtime", false)) {
    failsafePrefs.putString("failsafe", failsafeLastReason);
    failsafePrefs.putUInt("failsafe_n", failsafeRestartCount);
    failsafePrefs.putUInt("failsafe_same", failsafeConsecutiveSameReason);
    failsafePrefs.end();
  }
  Serial.printf("[failsafe] restarting reason=%s count=%u uptime_ms=%lu\n",
                failsafeLastReason.c_str(), failsafeRestartCount, millis());
  delay(100);
  ESP.restart();
}

bool probeLocalHttpListener() {
  if (!httpServerStarted) return false;
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = inet_addr("127.0.0.1");
  address.sin_port = htons(BRIDGE_HTTP_PORT);
  int connectError = 0;
  if (!connectSocketWithTimeout(sock, address, 2000UL, connectError)) {
    close(sock);
    return false;
  }
  timeval timeout{};
  timeout.tv_sec = 2;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  const char request[] = "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (!sendAll(sock, reinterpret_cast<const uint8_t *>(request), sizeof(request) - 1)) {
    close(sock);
    return false;
  }
  char response[64] = {};
  const int count = recv(sock, response, sizeof(response) - 1, 0);
  close(sock);
  return count > 0 && strstr(response, " 200 ") != nullptr;
}

bool restartHttpServerFromFailsafe() {
  if (httpServiceMutex != nullptr &&
      xSemaphoreTake(httpServiceMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  server.close();
  vTaskDelay(pdMS_TO_TICKS(25));
  server.begin();
  lastHttpServerRestartMs = millis();
  ++httpServerRestartCount;
  httpServerLastRestartReason = "loopback_self_test_failed";
  if (httpServiceMutex != nullptr) xSemaphoreGive(httpServiceMutex);
  return true;
}

void failsafeTask(void *pvParameters) {
  (void)pvParameters;
  unsigned long uploadLastProgressMs = millis();
  uint32_t previousBytesHashed = 0;
  uint32_t previousBytesSent = 0;
  uint32_t previousSuccesses = 0;
  uint32_t previousFailures = 0;
  String previousFilename;
  unsigned long lastHttpSelfTestMs = 0;
  while (true) {
    const unsigned long now = millis();
    if (!bootMarkedOperational && setupStartedMs != 0 && now - setupStartedMs > 180000UL) {
      triggerFailsafeRestart("startup_timeout");
    }
    if (bootMarkedOperational && loopHeartbeatMs != 0 &&
        now - loopHeartbeatMs > 120000UL && !httpLongOperationActive) {
      triggerFailsafeRestart("main_loop_stalled");
    }
    if (halowDownSinceMs != 0 && now - halowDownSinceMs > 600000UL &&
        !otaInProgress && !otaRestartPending) {
      triggerFailsafeRestart("halow_down_10_minutes");
    }
    const bool healthyLongHttpOperation = httpLongOperationActive &&
                                          now - httpLongOperationLastProgressMs < FAILSAFE_LONG_HTTP_STALL_MS;
    const bool healthyMediaUpload = trailMediaUploadActive &&
                                    now - uploadLastProgressMs < FAILSAFE_UPLOAD_STALL_MS;
    if (!healthyLongHttpOperation && !healthyMediaUpload && lastHttpServiceMs != 0 &&
        now - lastHttpServiceMs > FAILSAFE_HTTP_STALL_MS) {
      triggerFailsafeRestart("http_service_stalled");
    }
    if (httpLongOperationActive &&
        now - httpLongOperationLastProgressMs >= FAILSAFE_LONG_HTTP_STALL_MS) {
      triggerFailsafeRestart("http_long_operation_stalled");
    }

    if (!httpLongOperationActive && httpServerStarted &&
        (lastHttpSelfTestMs == 0 || now - lastHttpSelfTestMs >= HTTP_SELF_TEST_INTERVAL_MS)) {
      lastHttpSelfTestMs = now;
      if (probeLocalHttpListener()) {
        ++httpSelfTestSuccessCount;
        httpSelfTestConsecutiveFailures = 0;
      } else {
        ++httpSelfTestFailureCount;
        ++httpSelfTestConsecutiveFailures;
        Serial.printf("[failsafe] HTTP loopback failed consecutive=%u total=%u\n",
                      httpSelfTestConsecutiveFailures, httpSelfTestFailureCount);
        if (httpSelfTestConsecutiveFailures == 2) restartHttpServerFromFailsafe();
        if (httpSelfTestConsecutiveFailures >= 4) triggerFailsafeRestart("http_listener_dead");
      }
    }

    const bool uploadProgressed = trailMediaUploadBytesHashed != previousBytesHashed ||
                                  trailMediaUploadBytesSent != previousBytesSent ||
                                  trailMediaUploadSuccessCount != previousSuccesses ||
                                  trailMediaUploadFailureCount != previousFailures ||
                                  trailMediaUploadFilename != previousFilename;
    if (!trailMediaUploadActive || uploadProgressed) uploadLastProgressMs = now;
    if (trailMediaUploadActive && now - uploadLastProgressMs > FAILSAFE_UPLOAD_STALL_MS) {
      triggerFailsafeRestart("trail_upload_stalled");
    }
    previousBytesHashed = trailMediaUploadBytesHashed;
    previousBytesSent = trailMediaUploadBytesSent;
    previousSuccesses = trailMediaUploadSuccessCount;
    previousFailures = trailMediaUploadFailureCount;
    previousFilename = trailMediaUploadFilename;
    vTaskDelay(pdMS_TO_TICKS(FAILSAFE_CHECK_INTERVAL_MS));
  }
}

void startFailsafeTask() {
  if (failsafeTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(failsafeTask,
                          "failsafe",
                          4096,
                          nullptr,
                          3,
                          &failsafeTaskHandle,
                          HTTP_SERVICE_CORE);
}

bool durableMediaJobIsActive() {
  return durableMediaJob.status == DURABLE_MEDIA_QUEUED ||
         durableMediaJob.status == DURABLE_MEDIA_RUNNING ||
         durableMediaJob.status == DURABLE_MEDIA_WAITING_RETRY;
}

bool queueDurableMediaJob(DurableMediaJobAction action, String &message) {
  if (!sdReady || !durableMediaJobLoaded) {
    message = "sd_unavailable";
    return false;
  }
  if (durableMediaJobIsActive()) {
    message = "durable_job_busy:" + String(durableMediaJob.jobId);
    return false;
  }
  durableMediaJob = {};
  durableMediaJob.magic = DURABLE_MEDIA_JOB_MAGIC;
  durableMediaJob.version = DURABLE_MEDIA_JOB_VERSION;
  durableMediaJob.action = action;
  durableMediaJob.status = DURABLE_MEDIA_QUEUED;
  durableMediaJob.jobId = persistentBootCount * 1000UL + (millis() % 1000UL);
  durableMediaJob.createdBoot = bootSessionId;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "queued");
  SD.remove(DURABLE_JOB_MANIFEST_PATH);
  const bool saved = saveDurableMediaJob();
  if (saved) trailMediaUploadAbortRequested = true;
  message = saved ? "queued:" + String(durableMediaJob.jobId) : "persist_failed";
  return saved;
}

void durableMediaJobRetry(const String &error) {
  durableMediaJob.status = DURABLE_MEDIA_WAITING_RETRY;
  ++durableMediaJob.retryCount;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "waiting_retry");
  setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError), error);
  durableMediaNextRetryMs = millis() + DURABLE_MEDIA_RETRY_MS;
  saveDurableMediaJob();
  Serial.printf("[durable-job] id=%u retry=%u error=%s\n",
                durableMediaJob.jobId, durableMediaJob.retryCount, error.c_str());
  requestCameraStandbyAndPowerDownWifi("durable_media_retry_wait");
}

void durableMediaLowBatteryHold(uint32_t batteryMv) {
  const bool entering = String(durableMediaJob.stage) != "low_battery_wait";
  durableMediaJob.status = DURABLE_MEDIA_WAITING_RETRY;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                    "low_battery_wait");
  setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                    "low_battery:" + String(batteryMv) + "mV");
  durableMediaNextRetryMs = millis() + DURABLE_MEDIA_LOW_BATTERY_RECHECK_MS;
  durableTransferBytes = 0;
  durableTransferExpectedBytes = 0;
  durableTransferStartedMs = 0;
  durableTransferLastProgressMs = 0;
  saveDurableMediaJob();
  if (entering) {
    Serial.printf("[durable-job] id=%u low-battery hold battery=%u recover=%u\n",
                  durableMediaJob.jobId, batteryMv,
                  DURABLE_MEDIA_RECOVER_BATTERY_MV);
    requestCameraStandbyAndPowerDownWifi("durable_low_battery_hold");
  }
}

void durableMediaCameraBringupFailed() {
  const String prefix = "camera_bringup_failed:attempt=";
  uint8_t attempt = 1;
  const String previousError(durableMediaJob.lastError);
  if (previousError.startsWith(prefix)) {
    const long previousAttempt = previousError.substring(prefix.length()).toInt();
    if (previousAttempt > 0 && previousAttempt < UINT8_MAX) {
      attempt = static_cast<uint8_t>(previousAttempt + 1);
    }
  }

  if (attempt >= DURABLE_MEDIA_CAMERA_BRINGUP_MAX_ATTEMPTS &&
      attempt < DURABLE_MEDIA_CAMERA_BRINGUP_GIVE_UP_ATTEMPTS) {
    // A marginal camera link misses a few bring-ups in a row routinely. Back
    // off instead of failing; the job must survive without a server to
    // resubmit it. The attempt count stays in lastError across the wait.
    durableMediaJob.status = DURABLE_MEDIA_WAITING_RETRY;
    ++durableMediaJob.retryCount;
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                      "camera_unavailable_backoff");
    setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                      prefix + String(attempt));
    durableMediaNextRetryMs = millis() + DURABLE_MEDIA_CAMERA_BACKOFF_MS;
    saveDurableMediaJob();
    requestCameraStandbyAndPowerDownWifi("durable_camera_backoff");
    return;
  }

  if (attempt >= DURABLE_MEDIA_CAMERA_BRINGUP_GIVE_UP_ATTEMPTS) {
    durableMediaJob.status = DURABLE_MEDIA_FAILED;
    ++durableMediaJob.retryCount;
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                      "camera_unavailable");
    setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                      "camera_bringup_failed_after_" + String(attempt));
    saveDurableMediaJob();
    Serial.printf("[durable-job] id=%u stopped after %u camera bring-up failures\n",
                  durableMediaJob.jobId, static_cast<unsigned>(attempt));
    requestCameraStandbyAndPowerDownWifi("durable_camera_unavailable");
    return;
  }

  durableMediaJobRetry(prefix + String(attempt));
}

bool durableStorageError(const String &error) {
  return error.startsWith("sd_") || error.startsWith("storage_") ||
         error.indexOf(";sd_") >= 0 || error.indexOf(":sd_") >= 0 ||
         error.indexOf(":storage_") >= 0 || error.indexOf("manifest_") >= 0;
}

// Remounts the card and reloads the durable job from it as one step under
// sdMountMutex, so loop()'s background recovery cannot interleave.
void durableMediaRecoverStorage(const String &error, const char *recoveredMessage) {
  cameraSdTransferActive = false;
  requestCameraStandbyAndPowerDownWifi("durable_sd_recovery");
  if (sdMountMutex != nullptr) xSemaphoreTakeRecursive(sdMountMutex, portMAX_DELAY);
  sdReady = false;
  sdLastMessage = "durable_job_storage_recovery";
  const bool mounted = mountSdCardWithRetry();
  const bool loaded = mounted && loadDurableMediaJob();
  if (sdMountMutex != nullptr) xSemaphoreGiveRecursive(sdMountMutex);
  if (loaded) {
    durableMediaJobRetry(String(recoveredMessage) + ":" + error);
    return;
  }
  sdLastMessage = mounted ? "durable_job_reload_failed" : "durable_job_remount_failed";
  durableMediaJob.status = DURABLE_MEDIA_WAITING_RETRY;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "waiting_for_sd");
  setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError), error);
  durableMediaNextRetryMs = millis() + DURABLE_MEDIA_RETRY_MS;
}

bool durableMediaItemFailed(DurableMediaRecord &record, const String &error) {
  if (durableStorageError(error)) {
    // A card/volume fault is not a bad camera item. Do not consume an item
    // attempt or eventually skip/delete the wrong thing. Close-out has already
    // happened in downloadCameraMediaToSd(), so it is safe to remount here.
    durableMediaRecoverStorage(error, "sd_recovered");
    return false;
  }
  if (record.reserved[0] < UINT8_MAX) ++record.reserved[0];
  if (!writeDurableManifestRecord(durableMediaJob.currentIndex, record)) {
    // The transfer error may have been network-related, but losing the
    // manifest write means storage is unhealthy. Restore the volume and retry
    // this same record without charging an attempt that cannot be persisted.
    durableMediaRecoverStorage("manifest_attempt_write_failed", "manifest_storage_recovered");
    return false;
  }
  if (record.reserved[0] < DURABLE_MEDIA_ITEM_MAX_ATTEMPTS) {
    durableMediaJobRetry(error);
    return false;
  }

  ++durableMediaJob.retryCount;
  ++durableMediaJob.skippedCount;
  ++durableMediaJob.currentIndex;
  durableMediaJob.currentMediaId = 0;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "item_skipped_after_failures");
  setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                    "skipped_after_3:" + error);
  durableTransferBytes = 0;
  durableTransferExpectedBytes = 0;
  durableTransferStartedMs = 0;
  durableTransferLastProgressMs = 0;
  saveDurableMediaJob();
  Serial.printf("[durable-job] id=%u skipped media=%u after %u attempts error=%s\n",
                durableMediaJob.jobId, record.id, record.reserved[0], error.c_str());
  return true;
}

void serviceDurableMediaJob() {
  if (automaticRecoverySuspended) return;
  if (!durableMediaJobLoaded || !durableMediaJobIsActive() || trailMediaUploadActive) return;
  // With the card absent the job is known only from the NVS mirror; loop()'s
  // background recovery remounts and reloads it.
  if (!sdReady) return;
  if (durableMediaJob.status == DURABLE_MEDIA_WAITING_RETRY &&
      static_cast<long>(millis() - durableMediaNextRetryMs) < 0) return;
  const bool recoveringFromLowBattery =
    String(durableMediaJob.stage) == "low_battery_wait";
  const uint32_t durableBatteryMv = cachedStorageBatteryMv();
  const uint32_t requiredBatteryMv = recoveringFromLowBattery
                                      ? DURABLE_MEDIA_RECOVER_BATTERY_MV
                                      : DURABLE_MEDIA_MIN_BATTERY_MV;
  if (durableBatteryMv < requiredBatteryMv) {
    durableMediaLowBatteryHold(durableBatteryMv);
    return;
  }
  if (recoveringFromLowBattery) {
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                      "resume_after_low_battery");
    setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError), "");
    saveDurableMediaJob();
  }
  durableMediaJob.status = DURABLE_MEDIA_RUNNING;
  if (!wifiConnected) {
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "camera_bringup");
    saveDurableMediaJob();
    if (!runBringupSequence()) {
      // Camera availability is a job-level prerequisite, not an individual
      // media failure.  Do not walk and skip an entire manifest when the
      // camera is absent; stop after a small bounded number of attempts.
      durableMediaCameraBringupFailed();
      return;
    }
    setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError), "");
    saveDurableMediaJob();
  }
  if (durableMediaJob.manifestCount == 0 && durableMediaJob.currentIndex == 0) {
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "building_manifest");
    saveDurableMediaJob();
    uint32_t totalCount = 0;
    uint32_t cursor = 900000;
    bool manifestOk = true;
    bool manifestComplete = false;
    for (uint16_t page = 0; page < 256; ++page) {
      String gallery;
      int statusCode = 0;
      const String galleryPath = "/list/detail/backward/" + String(cursor) + "/60";
      if (!proxyCameraRequest("GET", galleryPath, gallery, statusCode) ||
          statusCode < 200 || statusCode >= 300) {
        durableMediaJobRetry("gallery_request_failed_" + String(statusCode));
        return;
      }
      uint32_t pageCount = 0;
      uint32_t minimumId = UINT32_MAX;
      if (!appendDurableManifestPage(gallery, page == 0, pageCount, minimumId)) {
        manifestOk = false;
        break;
      }
      totalCount += pageCount;
      if (pageCount == 0 || minimumId <= 1) {
        manifestComplete = true;
        break;
      }
      if (minimumId == UINT32_MAX || minimumId >= cursor) {
        manifestOk = false;
        break;
      }
      cursor = minimumId - 1;
      cooperativeDelay(25);
    }
    if (!manifestOk || !manifestComplete) {
      durableMediaJobRetry(manifestOk ? "manifest_page_limit_reached" : "manifest_pagination_failed");
      return;
    }
    if (totalCount == 0) {
      // An empty gallery is only authoritative when the camera's independent
      // storage counters also say there is no media. This prevents a transient
      // empty page or wrong-session response from becoming false success.
      String storageInfo;
      int storageStatus = 0;
      if (!proxyCameraRequest("GET", "/cmd/info/3", storageInfo, storageStatus) ||
          storageStatus < 200 || storageStatus >= 300) {
        durableMediaJobRetry("empty_gallery_storage_check_failed_" + String(storageStatus));
        return;
      }
      const long photoCount = jsonObjectIntField(storageInfo, "photo", 0);
      const long videoCount = jsonObjectIntField(storageInfo, "video", 0);
      if (photoCount > 0 || videoCount > 0) {
        durableMediaJobRetry("empty_gallery_count_mismatch:photo=" + String(photoCount) +
                             ",video=" + String(videoCount));
        return;
      }
    }
    durableMediaJob.manifestCount = totalCount;
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), totalCount == 0 ? "complete" : "downloading");
    saveDurableMediaJob();
    if (totalCount == 0) {
      durableMediaJob.status = DURABLE_MEDIA_SUCCEEDED;
      saveDurableMediaJob();
      requestCameraStandbyAndPowerDownWifi("durable_media_complete_empty");
      return;
    }
  }
  if (durableMediaJob.currentIndex >= durableMediaJob.manifestCount) {
    const bool deleteIncomplete =
      durableMediaJob.action == DURABLE_MEDIA_ACTION_DOWNLOAD_AND_DELETE &&
      durableMediaJob.deletedCount != durableMediaJob.manifestCount;
    durableMediaJob.status = deleteIncomplete ? DURABLE_MEDIA_FAILED
                                              : DURABLE_MEDIA_SUCCEEDED;
    durableMediaJob.currentMediaId = 0;
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                      deleteIncomplete ? "incomplete_delete" : "complete");
    if (deleteIncomplete) {
      setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                        "incomplete_delete:deleted=" + String(durableMediaJob.deletedCount) +
                        ",total=" + String(durableMediaJob.manifestCount) +
                        ",skipped=" + String(durableMediaJob.skippedCount));
    } else {
      setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError), "");
    }
    durableTransferBytes = 0;
    durableTransferExpectedBytes = 0;
    durableTransferStartedMs = 0;
    durableTransferLastProgressMs = 0;
    saveDurableMediaJob();
    Serial.printf("[durable-job] id=%u terminal=%s downloaded=%u skipped=%u deleted=%u/%u bytes=%u\n",
                  durableMediaJob.jobId, durableMediaStatusName(durableMediaJob.status),
                  durableMediaJob.downloadedCount,
                  durableMediaJob.skippedCount, durableMediaJob.deletedCount,
                  durableMediaJob.manifestCount,
                  durableMediaJob.bytesDownloaded);
    requestCameraStandbyAndPowerDownWifi("durable_media_complete");
    return;
  }
  DurableMediaRecord record{};
  if (!readDurableManifestRecord(durableMediaJob.currentIndex, record)) {
    durableMediaJobRetry("manifest_read_failed");
    return;
  }
  durableMediaJob.currentMediaId = record.id;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "downloading");
  saveDurableMediaJob();
  String transferMessage;
  uint32_t bytesWritten = 0;
  if (!downloadCameraMediaToSd(record, transferMessage, bytesWritten)) {
    if (transferMessage == "low_battery_pause") {
      durableMediaLowBatteryHold(cachedStorageBatteryMv());
      return;
    }
    refreshWifiState();
    const bool linkFailure = !transferMessage.startsWith("partial_progress_") &&
                             (transferMessage == "camera_connect_failed" ||
                              transferMessage == "camera_header_timeout" ||
                              transferMessage.endsWith("_closed") || !wifiConnected);
    if (linkFailure && durableMediaLinkFailures < DURABLE_MEDIA_MAX_LINK_FAILURES) {
      // The camera link dropped (weak signal, beacon loss). That says nothing
      // about this media item, so re-run bring-up instead of charging one of
      // its three attempts. Bounded so a link that never carries data still
      // eventually falls through to the normal item accounting.
      ++durableMediaLinkFailures;
      durableMediaJobRetry("camera_link_lost_" + String(durableMediaLinkFailures) + ":" +
                           transferMessage);
      return;
    }
    durableMediaLinkFailures = 0;
    if (transferMessage.startsWith("partial_progress_")) {
      // The camera commonly ends a response after a fixed-size block. Since
      // the .part file advanced and Range resume is supported, this is useful
      // progress rather than a failed item attempt. Keep camera WiFi up and
      // resume quickly; the normal three-attempt limit still applies to
      // consecutive attempts that make no progress.
      durableMediaJob.status = DURABLE_MEDIA_WAITING_RETRY;
      ++durableMediaJob.retryCount;
      setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage),
                        "resuming_partial_download");
      setDurableJobText(durableMediaJob.lastError, sizeof(durableMediaJob.lastError),
                        transferMessage);
      durableMediaNextRetryMs = millis() + 1000UL;
      saveDurableMediaJob();
      return;
    }
    durableMediaItemFailed(record, transferMessage);
    return;
  }
  durableMediaLinkFailures = 0;
  if (record.reserved[1] == 0) {
    record.reserved[1] = transferMessage == "already_staged" ? 2 : 1;
    if (!writeDurableManifestRecord(durableMediaJob.currentIndex, record)) {
      durableMediaJobRetry("manifest_result_write_failed");
      return;
    }
  }
  if (durableMediaJob.action == DURABLE_MEDIA_ACTION_DOWNLOAD_AND_DELETE) {
    setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "deleting_verified_item");
    saveDurableMediaJob();
    String deleteError;
    if (!deleteCameraMediaRecord(record, deleteError)) {
      durableMediaItemFailed(record, deleteError);
      return;
    }
    record.reserved[2] = 1;
    if (!writeDurableManifestRecord(durableMediaJob.currentIndex, record)) {
      durableMediaJobRetry("manifest_delete_result_write_failed");
      return;
    }
    ++durableMediaJob.deletedCount;
  }
  // "skipped" is reserved for items left on the camera after repeated
  // failures. An item staged by an earlier attempt is still downloaded.
  ++durableMediaJob.downloadedCount;
  durableMediaJob.bytesDownloaded += record.expectedBytes > 0 ? record.expectedBytes : bytesWritten;
  ++durableMediaJob.currentIndex;
  durableMediaJob.currentMediaId = 0;
  setDurableJobText(durableMediaJob.stage, sizeof(durableMediaJob.stage), "item_complete");
  durableTransferBytes = 0;
  durableTransferExpectedBytes = 0;
  durableTransferStartedMs = 0;
  durableTransferLastProgressMs = 0;
  saveDurableMediaJob();
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

String extractJsonStringField(const String &body, const String &key) {
  const String needle = "\"" + key + "\"";
  int pos = body.indexOf(needle);
  if (pos < 0) {
    return "";
  }
  pos = body.indexOf(':', pos + needle.length());
  if (pos < 0) {
    return "";
  }
  pos = body.indexOf('"', pos + 1);
  if (pos < 0) {
    return "";
  }
  String value;
  bool escaped = false;
  for (int i = pos + 1; i < body.length(); ++i) {
    const char c = body[i];
    if (escaped) {
      value += c;
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      break;
    } else {
      value += c;
    }
  }
  return value;
}

void handleCameraTargetGet() {
  String payload = "{\"ok\":true,\"target\":";
  payload += buildCameraTargetJson();
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleCameraTargetPost() {
  if (durableMediaJobIsActive()) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"durable_job_active\",\"job_id\":" +
                String(durableMediaJob.jobId) + "}");
    return;
  }
  String id = server.hasArg("id") ? server.arg("id") : "";
  String bleMac = server.hasArg("ble_mac") ? server.arg("ble_mac") : "";
  String wifiSsid = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : "";
  String bleName = server.hasArg("ble_name") ? server.arg("ble_name") : "";

  if (server.hasArg("plain")) {
    const String body = server.arg("plain");
    if (id.isEmpty()) id = extractJsonStringField(body, "id");
    if (bleMac.isEmpty()) bleMac = extractJsonStringField(body, "ble_mac");
    if (wifiSsid.isEmpty()) wifiSsid = extractJsonStringField(body, "wifi_ssid");
    if (bleName.isEmpty()) bleName = extractJsonStringField(body, "ble_name");
  }

  id.trim();
  if (!id.isEmpty()) {
    for (size_t i = 0; i < sizeof(CAMERA_PROFILES) / sizeof(CAMERA_PROFILES[0]); ++i) {
      if (id == CAMERA_PROFILES[i].id) {
        bleMac = CAMERA_PROFILES[i].bleMac;
        bleName = CAMERA_PROFILES[i].bleName;
        wifiSsid = CAMERA_PROFILES[i].wifiSsid;
        break;
      }
    }
  }

  String error;
  const bool ok = setCameraTarget(bleMac, wifiSsid, bleName, error);
  if (!ok) {
    String payload = "{\"ok\":false,\"error\":\"invalid_request\",\"detail\":\"";
    payload += jsonEscape(error);
    payload += "\",\"target\":";
    payload += buildCameraTargetJson();
    payload += "}";
    server.send(400, "application/json", payload);
    return;
  }

  String payload = "{\"ok\":true,\"target\":";
  payload += buildCameraTargetJson();
  payload += "}";
  server.send(200, "application/json", payload);
}

void controlWorkerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    const ControlAction action = takePendingControlAction();
    if (action == CONTROL_ACTION_NONE) {
      serviceDurableMediaJob();
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
    } else if (action == CONTROL_ACTION_TRIGGER_PHOTO) {
      ok = runTriggerPhotoJob();
      message = ok ? "trigger_photo_complete" : lastStreamStartMessage.c_str();
      if (!ok && (message == nullptr || message[0] == '\0')) {
        message = "trigger_photo_failed";
      }
    }

    finishControlAction(action, ok, message);
  }
}

void startHttpServer() {
  if (httpServerStarted) {
    server.begin();
    startHttpServiceTask();
    return;
  }
  server.on("/healthz", HTTP_GET, handleHealthz);
  server.on("/system/http_restart", HTTP_POST, handleHttpRestart);
  server.on("/system/restart", HTTP_POST, handleSystemRestart);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/system/status", HTTP_GET, handleSystemStatus);
  server.on("/diagnostics/status", HTTP_GET, handleDiagnosticStatus);
  server.on("/diagnostics/log", HTTP_GET, handleDiagnosticLog);
  server.on("/recovery/resume", HTTP_POST, handleRecoveryResume);
  server.on("/halow/status", HTTP_GET, handleHaLowStatusHttp);
  server.on("/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/wifi/transfer_test", HTTP_POST, handleTransferWifiTest);
  server.on("/camera/status", HTTP_GET, handleCameraBridgeStatus);
  server.on("/camera/target", HTTP_GET, handleCameraTargetGet);
  server.on("/camera/target", HTTP_POST, handleCameraTargetPost);
  server.on("/timing/status", HTTP_GET, handleTimingStatus);
  server.on("/stream/status", HTTP_GET, handleStreamRuntimeStatus);
  server.on("/ble/status", HTTP_GET, handleBleStatus);
  server.on("/control/status", HTTP_GET, handleControlStatus);
  server.on("/control/summary", HTTP_GET, handleControlSummary);
  server.on("/trail-media/status", HTTP_GET, handleTrailMediaStatus);
  server.on("/trail-media/files", HTTP_GET, handleTrailMediaList);
  server.on(UriRegex("^\\/trail-media\\/file\\/([^/]+)$"), HTTP_GET, handleTrailMediaFile);
  server.on("/camera/raw", HTTP_GET, handleCameraRawGet);
  server.on("/camera/request", HTTP_GET, handleCameraRequest);
  server.on("/camera/request", HTTP_POST, handleCameraRequest);
  server.on("/control/bringup", HTTP_POST, handleControlBringup);
  server.on("/control/stream_start", HTTP_POST, handleControlStreamStart);
  server.on("/control/stream_stop", HTTP_POST, handleControlStreamStop);
  server.on("/jobs", HTTP_GET, handleJobsGet);
  server.on("/jobs", HTTP_POST, handleJobsPost);
  server.on("/session/lease", HTTP_POST, handleSessionLease);
  server.on("/session/release", HTTP_POST, handleSessionRelease);
  server.on("/session/status", HTTP_GET, handleSessionStatus);
  server.on("/battery/status", HTTP_GET, handleBatteryStatus);
  server.on("/onboard/status", HTTP_GET, handleOnboardCameraStatus);
  server.on("/onboard/reinit", HTTP_POST, handleOnboardCameraReinit);
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
  server.on("/upload/status", HTTP_GET, handleUploadStatus);
  server.on("/trail-media/requeue", HTTP_POST, handleTrailMediaRequeue);
  server.on("/sd/status", HTTP_GET, handleSdStatus);
  server.on("/sd/mount", HTTP_POST, handleSdMount);
  server.on("/upload/telemetry", HTTP_POST, handleUploadTelemetry);
  server.on("/upload/events", HTTP_POST, handleUploadEvents);
  server.on("/upload/all", HTTP_POST, handleUploadAll);
  server.on("/firmware/status", HTTP_GET, handleFirmwareStatus);
  server.on("/firmware/update", HTTP_POST, handleFirmwareUpdateFinished, handleFirmwareUploadStream);
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
  startHttpServiceTask();
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

void resetRtpTelemetry() {
  rtpPacketsReceived = 0;
  rtpPacketsForwarded = 0;
  rtpSequenceGaps = 0;
  rtpPacketsMissing = 0;
  rtpOutOfOrder = 0;
  rtpForwardFailures = 0;
  udpReceiveOverruns = 0;
  lastRtpSequence = 0;
  haveLastRtpSequence = false;
  rtpKeyframeSeen = false;
  rtpKeyframesObserved = 0;
  streamQueueHighWater = 0;
  streamQueueEnqueued = 0;
  streamQueueDequeued = 0;
  streamQueueDropped = 0;
  tunnelWriteLastMs = 0;
  tunnelWriteMaxMs = 0;
  tunnelWriteBlockedCount = 0;
}

void recordRtpTelemetry(const uint8_t *payload, size_t length) {
  if (length < 12 || (payload[0] >> 6) != 2) return;
  ++rtpPacketsReceived;
  ++rtpPacketsReceivedTotal;
  const uint16_t sequence = (static_cast<uint16_t>(payload[2]) << 8) | payload[3];
  if (haveLastRtpSequence) {
    const uint16_t expected = static_cast<uint16_t>(lastRtpSequence + 1);
    if (sequence != expected) {
      const uint16_t forward = static_cast<uint16_t>(sequence - expected);
      if (forward < 0x8000u) {
        ++rtpSequenceGaps;
        rtpPacketsMissing += forward;
        rtpPacketsMissingTotal += forward;
      } else {
        ++rtpOutOfOrder;
      }
    }
  }
  lastRtpSequence = sequence;
  haveLastRtpSequence = true;
  const uint8_t nalType = payload[12] & 0x1F;
  bool keyframe = nalType == 5;
  if (nalType == 28 && length >= 14) keyframe = (payload[13] & 0x1F) == 5;
  if (keyframe) {
    rtpKeyframeSeen = true;
    ++rtpKeyframesObserved;
  }
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

  int receiveBufferBytes = 65536;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
             &receiveBufferBytes, sizeof(receiveBufferBytes));
  socklen_t receiveBufferLength = sizeof(receiveBufferBytes);
  if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                 &receiveBufferBytes, &receiveBufferLength) != 0) {
    receiveBufferBytes = 0;
  }

  if (primary) {
    udpPrimaryReady = true;
    udpPrimaryReceiveBufferBytes = receiveBufferBytes;
  } else {
    udpSecondaryReady = true;
    udpSecondaryReceiveBufferBytes = receiveBufferBytes;
  }

  Serial.printf("UDP receiver listening on %u for tunnel stream_id=%u rcvbuf=%d\n",
                localPort,
                primary ? STREAM_ID_VIDEO_RTP : STREAM_ID_VIDEO_RTCP,
                receiveBufferBytes);

  while (true) {
    StreamPacket packet{};
    sockaddr_in srcAddr{};
    socklen_t srcLen = sizeof(srcAddr);
    int len = recvfrom(sock, packet.payload, sizeof(packet.payload), 0,
                       reinterpret_cast<sockaddr *>(&srcAddr), &srcLen);
    if (len <= 0) {
      delay(10);
      continue;
    }

    if (primary) {
      mediaPrimaryPackets++;
      mediaPrimaryBytes += len;
      udpPrimaryStats.packets++;
      udpPrimaryStats.bytes += len;
      udpPrimaryStats.lastSourceIp = IPAddress(srcAddr.sin_addr.s_addr);
      udpPrimaryStats.lastSourcePort = ntohs(srcAddr.sin_port);
      udpPrimaryStats.lastPacketLen = static_cast<size_t>(len);
      lastPrimaryPacketMs = millis();
    } else {
      mediaSecondaryPackets++;
      mediaSecondaryBytes += len;
      udpSecondaryStats.packets++;
      udpSecondaryStats.bytes += len;
      udpSecondaryStats.lastSourceIp = IPAddress(srcAddr.sin_addr.s_addr);
      udpSecondaryStats.lastSourcePort = ntohs(srcAddr.sin_port);
      udpSecondaryStats.lastPacketLen = static_cast<size_t>(len);
      lastSecondaryPacketMs = millis();
    }

    if (primary) recordRtpTelemetry(packet.payload, static_cast<size_t>(len));
    if (!streamForwardingEnabled || streamPacketQueue == nullptr) continue;

    packet.sessionId = streamSessionId;
    packet.length = static_cast<uint16_t>(len);
    packet.streamId = primary ? STREAM_ID_VIDEO_RTP : STREAM_ID_VIDEO_RTCP;
    packet.flags = primary ? 0x01 : 0x02;
    if (primary && len >= 2 && (packet.payload[1] & 0x80) != 0) packet.flags |= 0x04;
    if (xQueueSend(streamPacketQueue, &packet, 0) == pdTRUE) {
      ++streamQueueEnqueued;
      const UBaseType_t depth = uxQueueMessagesWaiting(streamPacketQueue);
      if (depth > streamQueueHighWater) streamQueueHighWater = depth;
    } else {
      ++streamQueueDropped;
      ++udpReceiveOverruns;
      ++udpReceiveOverrunsTotal;
    }
  }
}

void streamForwardTask(void *pvParameters) {
  (void)pvParameters;
  StreamPacket packet{};
  while (true) {
    if (streamPacketQueue == nullptr ||
        xQueueReceive(streamPacketQueue, &packet, portMAX_DELAY) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    ++streamQueueDequeued;
    if (!streamForwardingEnabled || packet.sessionId != streamSessionId) continue;
    if (sendTunnelMediaPacket(packet.streamId, packet.flags,
                              packet.payload, packet.length)) {
      if (packet.streamId == STREAM_ID_VIDEO_RTP) {
        ++rtpPacketsForwarded;
        ++rtpPacketsForwardedTotal;
      }
    } else if (packet.streamId == STREAM_ID_VIDEO_RTP) {
      ++rtpForwardFailures;
      ++rtpForwardFailuresTotal;
    }
  }
}

bool startStreamPacketPipeline() {
  if (streamPacketQueue == nullptr) {
    streamPacketQueue = xQueueCreate(STREAM_PACKET_QUEUE_DEPTH, sizeof(StreamPacket));
    if (streamPacketQueue == nullptr) {
      Serial.printf("[stream-queue] allocation failed depth=%u item_bytes=%u\n",
                    static_cast<unsigned>(STREAM_PACKET_QUEUE_DEPTH),
                    static_cast<unsigned>(sizeof(StreamPacket)));
      return false;
    }
  }
  if (streamForwardTaskHandle == nullptr) {
    if (xTaskCreatePinnedToCore(streamForwardTask,
                                "stream-forward",
                                6144,
                                nullptr,
                                3,
                                &streamForwardTaskHandle,
                                1) != pdPASS) {
      Serial.println("[stream-queue] forwarding task creation failed");
      return false;
    }
  }
  xTaskCreatePinnedToCore(udpForwardTask, "udp-primary", 4096,
                          reinterpret_cast<void *>(0), 4, nullptr, 0);
  xTaskCreatePinnedToCore(udpForwardTask, "udp-secondary", 4096,
                          reinterpret_cast<void *>(1), 4, nullptr, 0);
  Serial.printf("[stream-queue] ready depth=%u packet_bytes=%u\n",
                static_cast<unsigned>(STREAM_PACKET_QUEUE_DEPTH),
                static_cast<unsigned>(sizeof(StreamPacket)));
  return true;
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
  if (cmd == "http_restart") {
    restartHttpServer("serial_command");
    Serial.println(buildHttpServiceJson());
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
  if (cmd == "diagnostics") {
    Serial.println(buildDiagnosticStatusJson());
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
    if (mountSdCardWithRetry() && !loadDurableMediaJob()) {
      sdLastMessage = "durable_job_load_failed";
    }
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
  if (cmd == "upload_status") {
    Serial.println(buildUploadStatusJson());
    return;
  }
  if (cmd == "upload_requeue") {
    if (!sdReady || !durableMediaJobLoaded || durableMediaJobIsActive() ||
        trailMediaUploadActive || trailMediaRequeueActive) {
      Serial.println("[trail-upload] requeue rejected: media_not_ready_or_busy");
      return;
    }
    uint32_t removed = 0;
    bool storageBusy = false;
    clearTrailMediaUploadMarkers(removed, storageBusy);
    Serial.printf("[trail-upload] requeue %s markers_removed=%u\n",
                  storageBusy ? "incomplete" : "complete", removed);
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
  if (cmd.startsWith("camera_target")) {
    String args = cmd.substring(strlen("camera_target"));
    args.trim();
    if (!args.isEmpty()) {
      const int split = args.indexOf(' ');
      String mac = split >= 0 ? args.substring(0, split) : args;
      String ssid = split >= 0 ? args.substring(split + 1) : "";
      mac.trim();
      ssid.trim();
      mac.toLowerCase();
      cameraTargetBleMac = mac;
      cameraTargetBleName = "";
      if (!ssid.isEmpty()) {
        cameraTargetWifiSsid = ssid;
      } else {
        const String derivedSsid = cameraWifiSsidFromBleMac(mac);
        if (!derivedSsid.isEmpty()) {
          cameraTargetWifiSsid = derivedSsid;
        }
      }
      bleTargetDeviceFound = false;
    }
    Serial.printf("[camera-target] ble_mac=%s name=%s wifi_ssid=%s\n",
                  cameraTargetBleMac.c_str(),
                  cameraTargetBleName.c_str(),
                  cameraTargetWifiSsid.c_str());
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
  setupStartedMs = millis();
  Serial.begin(115200);
  // Keep the SD card deselected throughout early boot. GPIO15/GPIO16 are also
  // sampled for charger telemetry before SPI starts, so a floating CS can make
  // those transitions look like an invalid command sequence to a slow card.
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  delay(500);
  Serial.println("GardePro dual-radio bridge starting");
  WiFi.setHostname(BOARD_HOSTNAME);
  Serial.printf("[network] hostname=%s\n", BOARD_HOSTNAME);
  initBootIdentity();
  enqueueUploadEvent("boot", "startup", "{\"firmware\":\"" + jsonEscape(FIRMWARE_VERSION) + "\"}", true);

  pinMode(BAT_ADC_CTRL_PIN, OUTPUT);
  digitalWrite(BAT_ADC_CTRL_PIN, BAT_ADC_CTRL_IDLE_LEVEL);
  pinMode(BAT_CHRG_PIN, INPUT_PULLUP);
  pinMode(BAT_DONE_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  const uint32_t bootBatteryMv = readBatteryMilliVolts(16);
  if (batteryBootHoldRequired(bootBatteryMv)) {
    enterLowBatteryBootHold(bootBatteryMv);
    return;
  }
  Serial.printf("[battery] %s\n", buildBatteryJson().c_str());

  onboardFrameMutex = xSemaphoreCreateMutex();
  onboardStorageMutex = xSemaphoreCreateMutex();
  sdMountMutex = xSemaphoreCreateRecursiveMutex();
  onboardCameraMutex = xSemaphoreCreateMutex();
  wifiRadioMutex = xSemaphoreCreateRecursiveMutex();
  observationUploadMutex = xSemaphoreCreateMutex();
  httpServiceMutex = xSemaphoreCreateMutex();
  durableMediaJobMutex = xSemaphoreCreateMutex();
  loadPersistedCameraTarget();
  loadDurableMediaJobMirror();
  startFailsafeTask();
  initOnboardStorage();
  if (mountSdCardWithRetry()) {
    loadDurableMediaJob();
  }
  appendDiagnosticJournal("boot");
  loadOnboardConfig();
  if (onboardCaptureEnabled && onboardCaptureWindowActive()) {
    initOnboardCamera();
  } else {
    onboardCameraReady = false;
    onboardCameraPoweredDownBySchedule = onboardCaptureEnabled;
    onboardCameraPowerState = onboardCaptureEnabled ? "scheduled_window_inactive" : "scheduled_disabled";
    Serial.printf("[onboard-camera] boot init skipped power_state=%s clock_valid=%s local_minute=%d window=%s-%s\n",
                  onboardCameraPowerState.c_str(),
                  onboardClockValid() ? "yes" : "no",
                  onboardLocalMinuteOfDay(),
                  minuteOfDayToString(onboardCaptureStartMinute).c_str(),
                  minuteOfDayToString(onboardCaptureEndMinute).c_str());
  }
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
    markBootOperational();
    if (!halowConnected) {
      Serial.println("[HaLow] local serial mode boot did not reach control plane");
    }
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
    startTrailMediaUploadTask();
    startFailsafeTask();
    startStreamPacketPipeline();
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
    printSerialHelp();
    printRuntimeStatus();
  } else {
    Serial.println("Skipping camera HTTP self-test because camera WiFi is down");
  }
  connectHaLow();
  startHttpServer();
  markBootOperational();
  if (controlWorkerTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(controlWorkerTask,
                            "control-worker",
                            8192,
                            nullptr,
                            1,
                            &controlWorkerTaskHandle,
                            CONTROL_WORKER_CORE);
  }
  startTrailMediaUploadTask();
  startFailsafeTask();

  startStreamPacketPipeline();
}

void loop() {
  loopHeartbeatMs = millis();
  if (lowBatteryBootHold) {
    pollSerialConsole();
    serviceLowBatteryBootHold();
    return;
  }

  pollSerialConsole();
  servicePersistentDiagnostics();
  if (otaRestartPending && millis() >= otaRestartAtMs) {
    Serial.println("[ota] restarting after successful firmware update");
    delay(100);
    ESP.restart();
  }
  serviceSdRecovery();
  serviceHaLowRecovery();

  if (RUN_LOCAL_SERIAL_TEST) {
    halowConnected = (HaLow.status() == WL_CONNECTED);
    if (!httpServerStarted) {
      startHttpServer();
    }
    if (httpServiceTaskHandle == nullptr) {
      serviceBoardHttp();
    }
    maybeRestartHttpServerIdle();
    if (halowConnected && !onboardClockValid() &&
        (lastClockSyncAttemptMs == 0 || millis() - lastClockSyncAttemptMs > 60000UL)) {
      lastClockSyncAttemptMs = millis();
      syncBoardClockFromAirScan();
    }
    refreshWifiState();
    processCameraSessionLeaseExpiry();
    serviceStreamHealth();
    if (streamSessionActive && rtspSessionOpen && millis() - lastRtspKeepaliveMs > RTSP_KEEPALIVE_INTERVAL_MS) {
      sendRtspKeepalive();
    }
    if ((CAMERA_HTTP_KEEPALIVE_ENABLED != 0) &&
        wifiConnected &&
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
    if ((CAMERA_AUTO_RECOVERY_ENABLED != 0) && streamSessionActive && !wifiConnected) {
      recoverActiveStream("camera_wifi_lost");
    }
    if ((CAMERA_AUTO_RECOVERY_ENABLED != 0) &&
        streamSessionActive && wifiConnected &&
        ((lastPrimaryPacketMs > 0 && millis() - lastPrimaryPacketMs > STREAM_STALL_TIMEOUT_MS) ||
         (lastPrimaryPacketMs == 0 && msSince(streamSessionStartedMs) > STREAM_STALL_FAIL_MS))) {
      const unsigned long rtpGapMs = lastPrimaryPacketMs == 0
                                       ? msSince(streamSessionStartedMs)
                                       : msSince(lastPrimaryPacketMs);
      Serial.printf("[stream] primary RTP stalled for %lu ms\n", rtpGapMs);
      recoverActiveStream("rtp_stall");
    }
    if ((CAMERA_AUTO_RECOVERY_ENABLED != 0) &&
        !streamSessionActive && !wifiConnected && cameraWifiEverConnected && !isControlActionActive() &&
        !transferWifiConnected &&
        !durableMediaJobIsActive() &&
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

  if (httpServiceTaskHandle == nullptr) {
    serviceBoardHttp();
  }
  maybeRestartHttpServerIdle();
  refreshWifiState();
  processCameraSessionLeaseExpiry();
  serviceStreamHealth();

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
