#!/usr/bin/env bash
set -euo pipefail

APK="${1:-com.zpszjs.gardepro.mobile_2.2.28.apk}"

if [[ ! -f "$APK" ]]; then
  echo "APK not found: $APK" >&2
  exit 1
fi

if ! command -v unzip >/dev/null 2>&1; then
  echo "unzip is required." >&2
  exit 1
fi

if ! command -v strings >/dev/null 2>&1; then
  echo "strings is required." >&2
  exit 1
fi

dex_strings() {
  strings -a apk_extract/classes*.dex 2>/dev/null
}

so_strings() {
  unzip -p "$APK" lib/arm64-v8a/libapp.so | strings -a
}

print_section() {
  echo
  echo "== $1 =="
}

print_section "DEX WiFi/BLE glue classes"
dex_strings | grep -E \
  'TrailerConnector|TrailerConnectorParam|BleManagerPlugin|BleManagerHandler|connectWiFiDevice|disconnectWiFiDevice|notifyBle|writeBle|wifiConnect|registerBroadcastReceiver' \
  | head -n 200 || true

print_section "Native BLE request/auth flow"
so_strings | grep -E \
  'createAuthRequest|createLoginRequest|createKeepAliveRequest|createATTransmitWeakUpRequest|sendBleWeakUp|sendAuth|requestLoginAuth|sendKeepAlive|loginDeviceByBleLevel0|addWiFiDevice|connectBleOk|connectBleFailed|btCommandAuthOk|BLE auth failed|BleAuthException|BleKeepAliveTimeoutException|authChallenge|step_5_checkPWD|pwdStr converted password|reqGetApn|reqGetApnInfo|reqSetApn|reqNetworkStatus|reqBaseRegcode' \
  | head -n 200 || true

print_section "Native BLE protocol framing"
so_strings | grep -E \
  'BleProtocolUtils|BleCommandHandler|BleMessage|BleMessageHeader|magicStart|magicEnd|msgType:0x|header.msgType|header.fragIndex|body length|Header data too short|Invalid magic number|BLE parseResponse|Ble response invalid magic|BleAuthException|BleKeepAliveTimeoutException|AuthResponse|DeviceCodeProtocolV1|DeviceCodeProtocolV1Obj|toByteStream|ByteOrder|Endian|TSS header.msgType:0x|isWeakUpCommand' \
  | head -n 200 || true

print_section "BLE UUIDs"
so_strings | grep -E \
  'serviceUuid|characteristicUuid|cccdUuid|6E400001|6E400002|6E400003|6E400004|0000ffb0|0000ffb1|00002902-0000-1000-8000-00805f9b34fb' \
  | head -n 200 || true

print_section "Password/Auth hints"
so_strings | grep -E \
  'Password consists of 4-digit|Enter camera password|password required|no password required|authorizationStatus|authHeaders|www-authenticate|authorization|proxy-authorization' \
  | head -n 200 || true

print_section "Offset-Ordered Flow Clues"
unzip -p "$APK" lib/arm64-v8a/libapp.so | strings -a -t d | grep -E \
  'addWiFiDevice|connectWiFiDevice|checkState ============ connectWiFiDevice|step_5_checkPWD|connect step step_5_checkPWD|loginDeviceByBleLevel0|connectBleOk|connectBleFailed|sendBleWeakUp|sendAuth|btCommandAuthOk|requestLoginAuth|sendKeepAlive|createAuthRequest|createLoginRequest|createATTransmitWeakUpRequest|createKeepAliveRequest|authChallenge|pwdStr converted password|calculateAuthMd5|reqGetApn|reqGetApnInfo|reqSetApn|reqNetworkStatus|reqBaseRegcode' \
  | head -n 200 || true
