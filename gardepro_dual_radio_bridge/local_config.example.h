#pragma once

// Copy this file to `local_config.h` and fill in your local network values.
// `local_config.h` is ignored by git.

#define HALOW_SSID "replace-with-your-halow-ssid"
#define HALOW_PASS "replace-with-your-halow-password"

// Upstream production/local API reached over HaLow.
#define BOARD_HOSTNAME "trail_esp32"
#define SCANNER_HOST BOARD_HOSTNAME
#define UPSTREAM_API_HOST "192.168.1.42"
#define UPSTREAM_API_PORT 80
#define UPSTREAM_API_PREFIX "/trail_cam"
// Optional bearer token. Leave empty when the local API does not require auth.
#define UPSTREAM_API_TOKEN ""
// Optional live media tunnel override. Defaults to UPSTREAM_API_HOST.
#define UPSTREAM_TUNNEL_HOST UPSTREAM_API_HOST
#define UPSTREAM_TUNNEL_PORT 6000

// Onboard local ESP camera defaults. Runtime updates through /onboard/config
// persist in NVS after first boot.
#define ONBOARD_CAPTURE_ENABLED 1
#define ONBOARD_CAPTURE_INTERVAL_MS 1800000UL
#define ONBOARD_CAPTURE_START_MINUTE 360
#define ONBOARD_CAPTURE_END_MINUTE 1080

// Build with FQBN option PSRAM=opi for UXGA 1600x1200 support.
