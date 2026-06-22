# GardePro Protocol Findings

## Confirmed Camera Identity

- BLE MAC: `A4:6D:D4:9E:47:32`
- WiFi SSID: `CAM8Z8_A46DD49E4732`
- Hotspot IP: `192.168.8.1`
- HTTP control port: `8080`

The BLE identifier suffix `4732` matches the WiFi SSID suffix `4732`, so the app is clearly pairing the BLE and WiFi sides of the same camera.

## HTTP Control Plane

The app uses a loopback proxy on Android, but the real camera target is:

- `http://192.168.8.1:8080`

Confirmed HTTP requests:

- `GET /cmd/standby/reset`
- `GET /cmd/getParaSetting`
- `GET /cmd/info/1`
- `GET /cmd/info/2`
- `GET /cmd/info/3`
- `GET /cmd/info/4`
- `GET /cmd/info/5`
- `GET /cmd/info/6`
- `GET /list/detail/backward/900000/60`
- `GET /media/getIrStatus`

Observed responses:

- `/cmd/standby/reset` -> `{ "code": 0 }`
- `/cmd/info/1` -> brand/product/version
- `/cmd/info/2` -> temperature / voltage / ext_power
- `/cmd/info/3` -> storage usage / photo count / video count
- `/cmd/info/4` -> clock / timezone
- `/cmd/info/5` -> detailed firmware / SD / BLE version
- `/cmd/info/6` -> `{ "code": -1 }`
- `/list/detail/backward/900000/60` -> gallery item list
- `/media/getIrStatus` -> IR status / IR power

## Live Media Plane

Live view is not plain HTTP MJPEG.

Confirmed stream behavior:

- camera source UDP port: `49152`
- secondary camera UDP port: `49153`
- destination port on tablet is dynamic per session

Observed examples:

- `192.168.8.1:49152 -> 192.168.8.30:16140`
- `192.168.8.1:49152 -> 192.168.8.30:25748`
- `192.168.8.1:49153 -> 192.168.8.30:25749`

The stream is a sustained burst of near-MTU UDP packets, so the media plane is likely proprietary UDP or lightly wrapped RTP-like traffic.

## BLE Findings From APK / Logs

The app uses:

- `com.zopudt.ble_manager`
- `flutter_blue_plus_manager`

Relevant runtime/app strings:

- `TSS BLE _sendingCommandQueueAdd command:0x`
- `TSS BLE _parseResponseAndUpdate command:0x`
- `BLE _handleNotifyCommand :0x`
- `connectBleOk`
- `reqGetApn`
- `authChallenge`
- `pwdStr converted password`
- `cccdUuid`
- `serviceUuid`
- `characteristicUuid`

BLE UUIDs embedded in the app:

- `0000ffb0-0000-1000-8000-00805f9b34fb`
- `0000ffb1-0000-1000-8000-00805f9b34fb`
- `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- `6E400004-B5A3-F393-E0A9-E50E24DCCA9E`
- CCCD: `00002902-0000-1000-8000-00805f9b34fb`

Interpretation:

- `6E400001/2/3/4` is the only BLE data path confirmed on the real camera.
- `FFB0/FFB1` still appears in the app, but it should now be treated as a secondary hypothesis or legacy/shared-device support, not the leading candidate for this camera.
- The plugin clearly supports notify + descriptor writes + characteristic writes.

## BLE Runtime State Observed

From Android Bluetooth diagnostics:

- app package: `com.zpszjs.gardepro.mobile`
- BLE connection observed to `A4:6D:D4:9E:47:32`
- app performs active BLE scans and connects/disconnects during activation

Notable limitation:

- HCI snoop logging could not be made to emit files on this tablet build, so raw GATT PDUs are still missing.

## Most Likely Architecture

1. BLE is used for activation/bootstrap and probably stream-port negotiation.
2. WiFi HTTP on `192.168.8.1:8080` is used for camera info, settings, gallery, and runtime status.
3. Live video is then pushed over UDP from `49152/49153` to a client-selected port.

## Bridge Implication

A working bridge probably needs:

1. BLE bootstrap emulation or replay for activation/startup.
2. HTTP proxying for camera control and metadata.
3. UDP forwarding for live media.

If BLE startup cannot be reproduced, a fallback approach is:

- keep the Android device in the loop for BLE activation
- move only the WiFi HTTP + UDP transport off-device through the relay

## ESP32 BLE Recon

Confirmed directly from the ESP32:

- advertised name: `CAM8Z8_NoName_G_E6`
- advertised service: `6e000100-b5a3-f393-e0a9-e50e24dcca9e`
- discovered services:
  - `00001800-0000-1000-8000-00805f9b34fb`
  - `00001801-0000-1000-8000-00805f9b34fb`
  - `0000180a-0000-1000-8000-00805f9b34fb`
  - `1d14d6ee-fd63-4fa1-bfa4-8f47b42119f0`
  - `6e400001-b5a3-f393-e0a9-e50e24dcca9e`

Camera-specific data path on the real device:

- `6e400002-b5a3-f393-e0a9-e50e24dcca9e` props `WN`
- `6e400003-b5a3-f393-e0a9-e50e24dcca9e` props `T`
- `6e400004-b5a3-f393-e0a9-e50e24dcca9e` props `WNT`

Observations:

- notify registration on `6e400003` and `6e400004` succeeds
- no spontaneous notifications were observed after connect
- the `1d14d6ee-fd63-4fa1-bfa4-8f47b42119f0` service looks like a vendor helper/OTA service, not the main camera control path

Probes attempted from the ESP32 with no BLE response:

- raw ASCII `H` to `6e400002`
- raw ASCII `H` to `6e400004`
- raw ASCII `authChallenge`
- raw ASCII `reqGetApn`
- raw ASCII `getApnInfo`
- raw ASCII `1234`
- raw ASCII `1234567890`
- null-terminated `1234567890`
- framed hypothesis packet:
  - `[magicStart:0x55AA][command][length][payload][magicEnd:0x0D0A]`
  - test command `0x0001`
  - test payload `0x48`
- helper service writes on `984227f3-34fc-4045-a5d0-2c581f81a153`:
  - raw ASCII `H`
  - raw ASCII `authChallenge`
  - raw ASCII `loginDeviceByBleLevel0`
  - raw ASCII `reqGetApn`
- helper service writes on `f7bf3564-fb6d-4e53-88a4-5e37e0326063`:
  - raw ASCII `H`
  - raw ASCII `authChallenge`
- all of the main `6e400002` and `6e400004` ASCII probes above repeated as write-with-response instead of write-without-response

Live retest summary on the Heltec `HT-HC33` board:

- connection to the real camera is stable and reproducible
- service/characteristic discovery exactly matches the previously observed layout
- notify registration on `6e400003` and `6e400004` succeeds every run
- `6e400002`, `6e400004`, `984227f3`, and `f7bf3564` all accept writes without transport errors
- no notifications, no short `OK`, and no WiFi JSON have been observed from any tested sequence so far

Interpretation update from the live retest:

- the camera is not using a trivial ASCII command surface on any currently tested writable characteristic
- the current framed hypotheses (`0x55AA...0x0D0A` and simple type-length-payload packets) are still too naive or structurally wrong
- write-with-response is not the missing ingredient by itself
- the missing piece is more likely the real serialized `BleMessage` format and/or a challenge-derived auth payload rather than plaintext command names

Additional native BLE protocol clues from `libapp.so`:

- `BleProtocolUtils`
- `BleCommandHandler`
- `BleMessage`
- `BleMessageHeader`
- `BleMessageHeader.fromBytes`
- `BleMessageHeader{magic: 0x`
- `magicStart`
- `magicEnd`
- `BLE parseResponse:`
- `Ble response invalid magic`
- `TSS BLE result length:`
- `TSS wakeUpWifi`
- `TSS connectBle wakeUpWifi value:H`
- `BleAuthException`
- `BLE auth failed`
- `Handling auth event:`
- `AuthResponse`
- `BleKeepAliveTimeoutException`

Explicit BLE request builders found in the Flutter snapshot:

- `createAuthRequest`
- `createLoginRequest`
- `createKeepAliveRequest`
- `createATTransmitWeakUpRequest`

Additional named command families found in the Flutter snapshot:

- `reqBaseRegcode`
- `reqCellularInfo`
- `reqCmdVersionInfo`
- `reqGetApn`
- `reqSetApn`
- `reqNetworkStatus`
- `reqGetApnInfo`

Interpretation update:

- the BLE application protocol has a named serializer and command set, not ad-hoc ASCII commands
- `createAuthRequest` and `createLoginRequest` strongly suggest auth is at least a two-step flow
- `createKeepAliveRequest` implies the BLE session stays active after login and may time out without periodic traffic
- `createATTransmitWeakUpRequest` and `sendBleWeakUp` are consistent with a dedicated BLE wake/bootstrap command before WiFi handoff

## TrailerConnector Flow Recovered From `classes5.dex`

The Java/Kotlin side is no longer a black box. The key `TrailerConnector` behavior is:

- `writeBle(...)` sends `param.getCmdToWakeUpBT()` directly as raw UTF-8 bytes to `param.getSvrUuid()` + `param.getControlUUID()`
- `connect$2.onConnectSuccess(...)` sleeps about `110ms`, then immediately does:
  - `notifyBle(bleDevice)`
  - `writeBle(bleDevice)`
- `notifyBle(...)` registers notify on both:
  - `controlUUID`
  - `notifyUUID`
- `TrailerConnector` hard-coded timing/constants:
  - `connectTimesMax = 5`
  - `writeBleTimesMax = 5`
  - `timeoutToConnectGreaterThanAndroidQ = 6000`
  - `timeoutToAddGreaterThanAndroidQ = 8000`

Modern camera behavior for non-`CAM8Z6_`:

- `writeBle$1.onWriteSuccess(...)` does not stop after one wake write
- instead it keeps re-writing the same raw wake token on roughly a `1s` cadence until the internal counter reaches the `writeBleTimesMax * 2` threshold
- with the current constants, that means the app's modern path is effectively an app-style wake loop of about `11` successful writes before it gives up waiting for notify-driven data and triggers WiFi handoff

Legacy camera behavior for `CAM8Z6_`:

- older cameras use a shorter repeated-write loop and then a delayed WiFi handoff path

Notify semantics are also explicit now:

- either notify callback (`controlUUID` or `notifyUUID`) treats a short UTF-8 `OK` reply as success
- or it parses UTF-8 JSON containing:
  - `ssid`
  - `bssid`
  - `pwd`
- either condition can trigger `connectWifiDevice(...)`

Important implication:

- the Android side does **not** add a second Java-side binary envelope around `cmdWakeup`
- for this bind path, the most likely remaining gap is the exact raw wake/auth token passed from Flutter for `CAM8Z8_` / `DEVICE_BINDV3`, plus the write timing/cadence
- our earlier ESP wake probes were not fully app-like because they did not match the native repeated write-with-callback cadence closely enough

## Live Validation Of Native Write Cadence

The ESP32 recon sketch now has explicit app-style wake commands that mirror the recovered Android timing more closely:

- control characteristic only: `6e400002`
- raw UTF-8 token
- write-with-response enabled
- `11` attempts
- `1000ms` spacing

Live results against `CAM8Z8_NoName_G_E6`:

- `wakeapp` (`H` on `6e400002`) -> `0` notifications, no short `OK`, no WiFi JSON
- `wakeapp_tc` (`TCWAKEUP` on `6e400002`) -> `0` notifications, no short `OK`, no WiFi JSON

Interpretation update:

- matching the app's repeated-write cadence did **not** unlock this camera
- the remaining blocker is therefore much more likely the actual `DEVICE_BINDV3` token/payload selection than simple timing mismatch

## Additional V3/Auth Recovery

The Flutter snapshot exposes more BLE login stages than the earlier notes captured:

- `loginDeviceByBleLevel0`
- `_loginDeviceByBleLevel1_g5a6r7p8r9o`
- `_loginDeviceByBleLevel2_g5a6r7p8r9o`
- `_loginDeviceByBleLevel3_g5a6r7p8r9o`

Recovered auth-related anchors:

- `pwdStr converted password`
- `authChallenge`
- `MD5V3`
- `MD5V4`
- `MD5V4 str:`
- `calculateAuthMd5`
- `createAuthRequest`
- `createLoginRequest`
- `createKeepAliveRequest`

Bind/version clustering remains important:

- `MD5V3` is closer to the `loginDeviceByBleLevel0` cluster
- `calculateAuthMd5` is clustered with `DEVICE_BINDV3`
- `MD5V4 str:` appears in a separate neighborhood from `MD5V3`
- `DEVICE_BINDV3` remains the strongest current candidate for the `CAM8Z8_...` family

One useful negative result:

- the parser/envelope class names do **not** appear as Java/DEX package paths
- names such as `BleMessageHeader`, `BleProtocolUtils`, and `BleCommandHandler` appear to come from the Flutter/native snapshot in `libapp.so`, not from ordinary Java classes we can cheaply inspect in the DEX files

Current working model:

- `CAM8Z8_...` likely follows a `DEVICE_BINDV3` path
- that path probably uses a level-based auth sequence instead of a bare universal wake token
- the remaining likely missing piece is a V3 auth/login payload derived from password/challenge state, not just `H` or `TCWAKEUP`

## Cheap DEX Scan Confirmation

A direct `strings` scan of the DEX files confirmed the Android-side WiFi bind glue without needing a full decompile:

- `com/zopudt/wifi/bind/TrailerConnector`
- `com/zopudt/wifi/bind/TrailerConnectorParam`
- `com.zopudt.ble_manager.BleManagerPlugin`
- `com.zopudt.BleManagerHandler`

Confirmed `TrailerConnector`-side method/state names:

- `connectWiFiDevice`
- `disconnectWiFiDevice`
- `notifyBle`
- `writeBle`
- `wifiConnect`
- `registerBroadcastReceiver`
- `unregisterBroadcastReceiver`
- `wifiConnectedLessAndroidQ`
- `wifiConnectingLessAndroidQ`
- `writeBleFailedTimes`
- `writeBleTimes`
- `writeBleTimesMax`

Interpretation update:

- `TrailerConnector` is the concrete Android/Kotlin WiFi-bind state machine for this camera family.
- It is not WiFi-only; it explicitly owns BLE notify/write retries as part of the bind flow.
- The WiFi path is broadcast-driven on Android, which matches the presence of register/unregister receiver hooks and `wifiConnect*` state handling.
- The `writeBleFailedTimes >= writeBleTimesMax` string confirms the BLE stage has explicit retry/error handling before WiFi setup is considered complete.

## Packet-Hypothesis Matrix

The current APK/native strings support testing multiple binary envelopes rather than a single guessed frame:

1. `DeviceCodeProtocolV1`
   - Related strings: `DeviceCodeProtocolV1Obj`, `toByteStream`, `ByteOrder`, `Endian`, `littleEndian`
   - Probe shape:
     - `[type:1][length:2][payload:N]`
   - Variants worth testing:
     - little-endian length
     - big-endian length

2. `BleMessageHeader`
   - Related strings: `BleMessageHeader`, `BleMessageHeader.fromBytes`, `magicStart`, `magicEnd`, `_bigEndianToInt`, `_intToBigEndian`
   - Probe shapes:
     - little-endian framed:
       - `[magicStart:2][command:2][length:2][payload:N][magicEnd:2]`
     - big-endian framed:
       - `[magicStart:2][command:2][length:2][payload:N][magicEnd:2]`

3. TSS-style message header
   - Related strings: `TSS header.msgType:0x`, `TSS connectBle wakeUpWifi value:H`
   - Probe shape:
     - `[magicStart:2][msgType:1][length:2][payload:N][magicEnd:2]`

4. Wake/auth/login sequencing
   - Related strings:
     - `createATTransmitWeakUpRequest`
     - `createAuthRequest`
     - `createLoginRequest`
     - `createKeepAliveRequest`
     - `sendBleWeakUp`
   - Best current inference:
     - wake-up likely carries the literal payload `H`
     - auth/login/keepalive may be distinct message types rather than plaintext command names
     - the wake-up packet likely precedes APN/network-status requests

Operational implication:

- The next live BLE session should prioritize binary families in this order:
  1. TSS-style big-endian `msgType` frames
  2. big-endian framed command packets
  3. `DeviceCodeProtocolV1` short type-length-payload packets
- Plain ASCII probes should now be treated as control tests, not the main path.

## Latest Live ESP32 Results

Additional live testing on the real camera after the packet-matrix update:

- the ESP32 sketch was extended to test:
  - TSS-style `msgType` packets
  - big-endian framed packets
  - write-with-response variants of TSS / framed / proto packets
- the sketch was also updated to remove the automatic startup wake probe so manual testing is faster and more controlled

Confirmed live outcomes:

- BLE connection remains stable
- notify registration on `6e400003` and `6e400004` succeeds every run
- all tested binary writes are accepted at the transport level on both:
  - `6e400002`
  - `6e400004`
- tested write-with-response wake packets still produce:
  - no notifications
  - no short `OK`
  - no WiFi JSON
  - no visible change in session state

Explicit live-tested response-enabled packets with no result:

- TSS-style wake packet carrying `H`
  - `55 AA 01 00 01 48 0D 0A`
- big-endian framed wake packet carrying `H`
  - `55 AA 00 01 00 01 48 0D 0A`
- short proto-be wake packet carrying `H`
  - `01 00 01 48`

Interpretation update:

- transport acceptance is no longer the blocker
- the missing piece is now much more likely the actual serialized payload/body for auth/login/wakeup, not write mode or characteristic choice alone
- the real request builders probably populate non-empty fields that are still absent from the current hand-built probes

Useful native string anchors in `libapp.so` for the next serializer-focused pass:

- `TSS header.msgType:0x` at decimal offset `745891`
- `DeviceCodeProtocolV1Obj` at `1863092`
- `toByteStream` at `1863125`
- `sendBleWeakUp` at `1878450`
- `BleMessageHeader.fromBytes` at `2385509`
- `createLoginRequest` at `2642284`
- `_intToBigEndian@1469065673` at `2707739`
- `_bigEndianToInt@1469065673` at `2740988`
- `createATTransmitWeakUpRequest` at `2829334`
- `createAuthRequest` at `2927400`

## Offset-Ordered Flow Clues

An offset-ordered pass over the native snapshot gives a clearer view of the app-side state flow, even though it does not yet reveal the actual serialized payload structure:

- `addWiFiDevice` at `529237`
- `checkState ============ connectWiFiDevice checkState state 2:` at `555791`
- `BleConfigCameraStatus.connectBleOk` at `685429`
- `BleConfigCameraStatus.reqGetApn commandStatus.status:` at `686735`
- `checkState ============ connectWiFiDevice checkState 1` at `801193`
- `reqNetworkStatus` at `870781`
- `loginDeviceByBleLevel0` at `918125`
- `step_5_checkPWD ==== 1 - no password required` at `1239433`
- `loginDeviceByBleLevel0. bleMac` at `1322723`
- `init:reqSetApn` at `1376647`
- `step_5_checkPWD ==== 2 - password required` at `1403612`
- `=========== connectWiFiDevice checkState state:` at `1486233`
- `reqSetApn` at `1543214`
- `addWiFiDevice >>>>bleMac:` at `1586133`
- `sendBleWeakUp` at `1878450`
- `step_5_checkPWD` at `1943617`
- `BleConfigCameraStatus.reqSetApn commandStatus.status:` at `1949981`
- `init:reqNetworkStatus` at `1954462`
- `init:reqBaseRegcode` at `2021487`
- `init:reqGetApn` at `2135496`
- `reqGetApnInfo` at `2198093`
- `connectBleOk` at `2216147`
- `connect step step_5_checkPWD` at `2525804`
- `createLoginRequest` at `2642284`
- `reqGetApn` at `2694644`
- `connect step step_5_checkPWD_OK_` at `2741977`
- `createATTransmitWeakUpRequest` at `2829334`
- `createAuthRequest` at `2927400`
- `createKeepAliveRequest` at `2965771`
- `reqBaseRegcode` at `3115652`

Interpretation update:

- the UI password gate (`step_5_checkPWD`) is definitely a separate app state, not a direct proof that the BLE packet itself is plaintext password
- `sendBleWeakUp` is part of the BLE-to-WiFi handoff path, but the string ordering alone does not prove whether it happens before or after auth/login on the wire
- the serializer/request-builder names remain present but isolated in the snapshot string table, so offset order alone is not sufficient to recover packet field layout
- the strongest next reverse-engineering target is still the actual body format used by:
  - `createAuthRequest`
  - `createLoginRequest`
  - `createATTransmitWeakUpRequest`
  - `createKeepAliveRequest`

## Generic BLE vs STK Split

Further native string analysis shows the protocol layer is shared across at least two families:

Generic BLE / WiFi-trailcam relevant:

- `sendBleWeakUp`
- `wakeUpWifi`
- `loginDeviceByBleLevel0`
- `loginDeviceByBleLevel1`
- `connectBleOk`
- `addWiFiDevice`
- `connectWiFiDevice`
- `createAuthRequest`
- `createLoginRequest`
- `createKeepAliveRequest`
- `createLogoutRequest`
- `createVersionInfoRequest`
- `createBatteryStatusRequest`
- `createFactoryResetRequest`
- `createSetBleNameRequest`
- `createSetAdvancedInfoRequest`
- `createGetAdvancedInfoRequest`
- `BleMessageHeader`
- `BleMessage`
- `BleProtocolUtils`
- `BleCommandHandler`

Likely cellular / STK-specific and not primary for this WiFi trailcam:

- `package:flutter_blue_plus_manager/src/protocol/stk_command_handler.dart`
- `createATTransmitWeakUpRequest`
- `createATTransmitRequest`
- `createSetApnRequest`
- `createGetApnInfoRequest`
- `createBaseRegCodeRequest`
- `createNetworkStatusRequest`
- `createCellularInfoRequest`
- `createSetPinInfoRequest`
- `createSetMqttAddrRequest`
- `AT+QSTK...`
- `TSS STK Network Event`
- `TSS STK Connection Event`

Interpretation update:

- earlier emphasis on `createATTransmitWeakUpRequest` was probably mixing in the cellular/STK path
- the WiFi trailcam work should stay centered on the generic `BleMessage` / `BleMessageHeader` path and the generic auth/login/keepalive builders

## Bind-Version Clues

Additional bind/version-related strings found in the snapshot:

- `DEVICE_BINDV2` at `583205`
- `TSS ble.BleManager.connectWiFiDevice delay timeoutToWaitConnectWifi_BLEV0_9 8000` at `586333`
- `hasBind` at `706743`
- `DEVICE_BINDV2 _tips_g5a6r7p8r9o =` at `1633979`
- `init:DEVICE_BINDV2` at `1838669`
- `_checkBindResultTips...` at `2671006`
- `toBind` at `2740347`
- `DEVICE_BINDV3` at `3024083`
- `init:DEVICE_BINDV3` at `3085734`

Interpretation update:

- there is visible support for at least two bind modes: `DEVICE_BINDV2` and `DEVICE_BINDV3`
- there is no visible `DEVICE_BINDV4` string in the current snapshot
- `MD5V3` / `MD5V4` likely refer to auth digest variants, but they do not map cleanly enough yet to assert which bind version this camera uses
- `loginDeviceByBleLevel1` plus `calculateAuthMd5` is still the strongest clue for the password-protected path

Additional Dart-side flow names and status strings found in the Flutter snapshot:

- `loginDeviceByBleLevel0`
- `addWiFiDevice`
- `connectBleOk`
- `BleConfigCameraStatus.connectBleOk`
- `checkState ============ connectWiFiDevice checkState 1`
- `checkState ============ connectWiFiDevice checkState state 2:`
- `============ connectWiFiDevice checkState state:`

Best current inference for the Dart-side BLE/WiFi sequence:

1. `loginDeviceByBleLevel0`
2. `createAuthRequest`
3. `createLoginRequest`
4. `sendBleWeakUp` / `createATTransmitWeakUpRequest`
5. `connectBleOk`
6. `addWiFiDevice` / `connectWiFiDevice`
7. follow-up status/config reads such as:
   - `reqGetApn`
   - `reqGetApnInfo`
   - `reqSetApn`
   - `reqNetworkStatus`
   - `reqBaseRegcode`
   - `reqCmdVersionInfo`

Interpretation:

- the Flutter layer appears to own a higher-level BLE session state machine above the Android GATT transport
- `connectWiFiDevice` is probably not the whole BLE protocol; it is the transport handoff point after enough BLE state has already been established
- the current camera may accept the plain wake trigger `H`, but the full app flow almost certainly includes auth/login/session commands before or around that handoff

## Android BLE Glue Confirmed In DEX

The Android/Kotlin side does not build the BLE protocol itself, but it does confirm how the app wires the transport:

- `com.zopudt.wifi.bind.TrailerConnector`
- `com.zopudt.BleManagerHandler`
- `com.zopudt.ble_manager.BleManagerPlugin`

Confirmed `TrailerConnector` BLE methods:

- `bleConnect`
- `notifyBle`
- `writeBle`
- `connectWifiDevice`
- `disconnect`
- `destroyBle`

Confirmed `TrailerConnectorParam` fields relevant to BLE/WiFi bootstrap:

- `getBleMacAddressToConnect`
- `getWifiMacAddressToConnect`
- `getWifiPassword`
- `getWifiPrefix`
- `getSvrUuid`
- `getControlUUID`
- `getNotifyUUID`
- `getCmdToWakeUpBT`

Interpretation:

- the app passes service UUID, control UUID, notify UUID, and wake-up command text/data into the Android BLE layer
- the Android side owns GATT connection, notify registration, descriptor writes, and characteristic writes
- the Flutter snapshot still appears to own the higher-level BLE serializer/parser (`BleMessageHeader`, auth/login/keepalive builders)

Recovered Flutter -> Android method-call keys for `connectWiFiDevice`:

- `bleMac`
- `wifiSSID`
- `wifiPrefix`
- `wifiPassword`
- `bssid`
- `serviceUuid`
- `characteristicUuid`
- `characteristicNotifyUuid`
- `cmdWakeup`

Recovered Flutter log clue:

- `TSS connectBle wakeUpWifi value:H`

Interpretation:

- the Flutter layer appears to pass a wake-up command separately from the framed serializer/builder logic
- the logged wake-up value is very likely plain ASCII `H` for at least one connection path

## Android BLE Transport Behavior Recovered

Recovered from decompiling `classes5.dex`:

- after BLE connect, the app enumerates GATT services/characteristics for logging
- it then calls `notifyBle(...)` and `writeBle(...)`
- `writeBle(...)` writes `cmdToWakeUpBT` as UTF-8 bytes to `controlUUID` on `svrUuid`
- `notifyBle(...)` subscribes to both `controlUUID` and `notifyUUID` on `svrUuid`

Recovered `writeBle(...)` behavior:

- write target service: `param.svrUuid`
- write target characteristic: `param.controlUUID`
- write payload: `param.cmdToWakeUpBT.getBytes(UTF_8)`
- current camera family retry constants from `TrailerConnector`:
  - `connectTimesMax = 5`
  - `writeBleTimesMax = 5`
  - BLE watchdog timer: `90000 ms`
  - notify watchdog timer: `50000 ms`
  - WiFi watchdog timer: `85000 ms`
  - Android Q+ WiFi handoff delay: `6000 ms`

Recovered notification behavior:

- notification callback is registered on `param.controlUUID`
- a second notification callback is registered on `param.notifyUUID`
- both callbacks parse the notification bytes as UTF-8 text
- both callbacks look for either:
  - short text containing `OK`
  - or JSON with keys `ssid`, `bssid`, and `pwd`

Recovered state-machine behavior:

- on short `OK` replies, the app starts a timer and then proceeds with WiFi connection flow
- on JSON replies containing `ssid`, `bssid`, and `pwd`, the app immediately calls `connectWifiDevice(...)`
- for current cameras, BLE notifications appear to be part of how the app learns or confirms WiFi credentials
- there is a special-case branch for SSIDs starting with `CAM8Z6_`, treated as an older camera path
- for non-`CAM8Z6_` cameras, a successful wake write causes the app to write the same wake payload again after about `1000 ms`
- that repeat-write loop continues until `writeBleTimes` reaches `writeBleTimesMax * 2`, so the current camera path may send the wake payload about 10 times
- on write failure before any successful BLE progress, the app may:
  - retry the write after about `1000 ms`
  - reconnect BLE if the error text contains `not connect`
  - or reset the write callback and re-issue the write after about `500 ms`

Bridge implication:

- the camera-side BLE wake-up payload accepted by the Android layer is plain UTF-8 at the GATT transport boundary
- however, the higher-level Flutter protocol still appears to define framed auth/login/keepalive requests for newer logic paths
- one plausible split is:
  - Android glue: plain-text wake trigger over a configured characteristic
  - Flutter logic: structured/framed messages for auth/session commands on the same BLE transport
- for the current `CAM8Z8_...` camera, the most app-faithful ESP32 test is now:
  - subscribe on both notify characteristics
  - write ASCII `H` to the control characteristic
  - repeat that write up to about 10 times with roughly 1 second spacing
  - watch for either short `OK` or JSON containing `ssid`, `bssid`, and `pwd`

## ESP32 Probe Sets

The recon sketch now includes named higher-level probe sequences intended to approximate the inferred Dart flow:

- `seq_login2`
  - repeated wake writes with `H`
  - then UTF-8 probes for:
    - `authChallenge`
    - `loginDeviceByBleLevel0`
    - `reqGetApn`
    - `reqGetApnInfo`
    - `reqNetworkStatus`
    - `reqCmdVersionInfo`
- `seq_wake_status`
  - repeated wake writes with `H`
  - then status/config probes for:
    - `reqGetApn`
    - `reqGetApnInfo`
    - `reqNetworkStatus`
    - `reqBaseRegcode`
    - `reqCmdVersionInfo`
- `seq_alt4`
  - a short wake/auth probe against `6e400004` instead of `6e400002`
- `seq_frame_auth`
  - sends hypothesized `BleMessageHeader`-style framed packets on `6e400002`
  - current trial command IDs:
    - `0x0001` wake with payload `0x48`
    - `0x0002` auth with ASCII payload `auth`
    - `0x0003` login with ASCII payload `login`
- `seq_proto_le`
  - sends `DeviceCodeProtocolV1Obj`-style trial packets with little-endian length fields
  - current trial types:
    - `0x01` wake
    - `0x02` APN
    - `0x03` network
    - `0x04` version
- `seq_proto_be`
  - same as `seq_proto_le`, but big-endian length fields

Important limitation:

- these sequence payloads are still heuristic UTF-8 probes based on Dart symbol names and log strings
- they are useful for testing whether the camera accepts plain-text command labels after wake-up
- they are not evidence that the real Flutter auth/login serializer uses those literal payloads on the wire
- the new binary probe sets are even more speculative:
  - command IDs are guessed placeholders
  - payload contents are minimal markers, not recovered protocol fields
  - they are intended to quickly detect whether the camera reacts to any framed/proto family at all

Current working BLE model:

- the transport is definitely the `6e400001/2/3/4` service family on this camera
- the BLE application protocol is framed, not plain text
- the app likely performs a sequence like wake-up -> auth -> login -> keepalive -> WiFi/APN/status commands

## Android Plugin Bridge Confirmed

Recovered from `classes5.dex` with narrow `androguard` disassembly:

- `BleManagerPlugin.onMethodCall(...)` does not interpret WiFi/BLE calls itself
- it forwards every non-`requestTurnOnBluetooth` call into `MessageSender.sendMessage(...)`
- `MessageSender.sendMessage(...)` runs a `LocalSocketRunnable`
- `MessageSender$LocalSocketRunnable.run()` serializes the Flutter method name and arguments as JSON and sends them to:
  - local socket: `com.zpszjs.gardepro.mobile.wifisrv.sock`
- that means the Android BLE/WiFi glue is effectively a local socket service, not a direct in-process method-channel handler

Recovered `LocalSocketRunnable.run()` behavior:

- if Flutter arguments are already a map/JSON object:
  - it inserts:
    - `plugin`
    - `method`
- if Flutter arguments are null/non-map:
  - it builds a wrapper object with:
    - `plugin`
    - `method`
    - `arguments`
- it then writes the JSON request to the local socket and returns the socket response string back to Flutter

Recovered `BleManagerHandler.handMessage(...)` dispatch:

- `connectWiFiDevice` is handled by Android-side service code, not by Flutter directly
- `handMessage(...)` explicitly dispatches:
  - `startScan`
  - `stopScan`
  - `refresh`
  - `isScanning`
  - `isConnected`
  - `disconnect`
  - `getScanResults`
  - `connectWiFiDevice`
  - `getConnectionStep`
  - `cancelConnectWiFiDevice`
  - `disconnectWiFiDevice`
  - `bindNetwork`

Recovered `BleManagerHandler.connectWiFiDevice(...)` argument extraction:

- exact required argument keys:
  - `bleMac`
  - `wifiSSID`
  - `wifiPrefix`
  - `wifiPassword`
  - `bssid`
  - `serviceUuid`
  - `characteristicUuid`
  - `characteristicNotifyUuid`
  - `cmdWakeup`
- these values are passed directly into `TrailerConnectorParam(...)`
- the Android service then:
  - sets connection step to `BleConnecting`
  - creates a new `TrailerConnector`
  - calls `init(param)`
  - calls `start()`
  - blocks on a `CountDownLatch` until one of the connection callbacks advances the state

Interpretation:

- the Flutter side does not appear to synthesize the GATT transport layout dynamically inside Android
- instead, Flutter supplies the exact service UUID, write UUID, notify UUID, and wake string to the Android BLE service
- this further supports the current conclusion that:
  - the initial wake trigger is plain UTF-8 `cmdWakeup` data written to `characteristicUuid`
  - structured auth/login logic likely lives above this bridge, in Flutter/native snapshot logic, not in the Android `TrailerConnector`

## Live Validation After Android Bridge Recovery

Reflashed the current `gardepro_ble_recon` sketch and re-ran the wake path against the attached camera.

Confirmed live console state:

- BLE link is up
- discovered characteristic handles are still:
  - `6e400002`
  - `6e400003`
  - `6e400004`
  - helper chars:
    - `984227f3-34fc-4045-a5d0-2c581f81a153`
    - `f7bf3564-fb6d-4e53-88a4-5e37e0326063`

Confirmed app-faithful wake probe result:

- command used:
  - repeated ASCII `H`
  - 10 writes on `6e400002`
  - then 5 fallback writes on `6e400004`
- result:
  - no notifications
  - no short `OK`
  - no WiFi JSON

Confirmed write-with-response check:

- manual probes:
  - repeated `send2r H`
- result:
  - writes succeed
  - still no notifications
  - still no semantic reply

Interpretation:

- the Android-side wake model is now verified well enough that simple transport mistakes are becoming less likely
- either:
  - the real `cmdWakeup` for this camera is not actually `H` on the current bind version
  - the app is using different service/characteristic UUID values than the obvious Nordic path for this specific family
  - or the wake trigger alone is insufficient and the camera expects an immediate follow-up auth/login packet family on the same transport

Helper-UUID cross-check:

- searched APK/native strings for:
  - `984227f3`
  - `f7bf3564`
- result:
  - not present in `libapp.so`
- searched native strings for:
  - `6E400001`
  - `6E400002`
  - `6E400003`
  - `6E400004`
  - `0000ffb0`
  - `0000ffb1`
- result:
  - all present

Interpretation:

- the helper UUIDs currently exposed by the camera do not appear to be referenced by the app bundle
- the Nordic/FFB0 families remain the strongest app-backed candidate transport paths

## New Wake-String Lead: `TCWAKEUP`

Recovered a new concrete string from the `DEVICE_BINDV2` neighborhood in `libapp.so`:

- `TCWAKEUP`

Important context:

- the Flutter snapshot also contains:
  - `TSS connectBle wakeUpWifi value:H`
  - `TSS wakeUpWifi`
  - `DEVICE_BINDV2`
  - `DEVICE_BINDV3`
- this suggests there may be more than one wake-string family depending on bind version or camera family

Interpretation:

- `H` is likely a real wake value for at least one WiFi bind path
- `TCWAKEUP` is now a second real candidate, likely associated with a `DEVICE_BINDV2` path or an older/newer branch
- current evidence no longer supports assuming a single universal wake string across all WiFi cameras

## Live Validation of `TCWAKEUP`

Tested directly on the attached camera without changing the sketch:

- `send2 TCWAKEUP` repeated on `6e400002`
- `send2r TCWAKEUP` on `6e400002`
- `send4 TCWAKEUP` on `6e400004`
- `send4r TCWAKEUP` on `6e400004`

Observed result:

- all writes succeeded at transport level
- no notifications were emitted
- no short `OK`
- no WiFi JSON

Interpretation:

- `TCWAKEUP` is a real app string, but it does not appear to be sufficient for the current `CAM8Z8_...` camera on the observed Nordic path
- the camera may:
  - use a different bind-version wake string
  - require a wake string plus immediate auth/login framing
  - or use a UUID/value tuple selected by Flutter that we still have not recovered from the snapshot object graph

## Native Snapshot Split: V2 Wake Cluster vs V3 Auth Cluster

Ran a tighter offset scan over `libapp.so` to compare the relative placement of the bind/version and request-builder strings.

Recovered offsets:

- `TSS wakeUpWifi` at `537514`
- `wakeUpWifi` at `537518`
- `DEVICE_BINDV2` at `583205`
- `TCWAKEUP` at `584418`
- `sha256HmacHex` at `694740`
- `TSS connectBle wakeUpWifi value:H` at `775552`
- `sendBleWeakUp` at `1878450`
- `wakeUpWifi` at `2424891`
- `createLoginRequest` at `2642284`
- `createAuthRequest` at `2927400`
- `createKeepAliveRequest` at `2965771`
- `createLogoutRequest` at `3021469`
- `calculateAuthMd5` at `3022668`
- `DEVICE_BINDV3` at `3024083`
- `init:DEVICE_BINDV3` at `3085734`

Additional nearby native symbols/strings still visible in the same overall auth path:

- `step_1_connectBle`
- `connectBleOk`
- `connectBleFailed`
- `loginDeviceByBleLevel0`
- `_loginDeviceByBleLevel1_g5a6r7p8r9o`
- `_loginDeviceByBleLevel2_g5a6r7p8r9o`
- `_loginDeviceByBleLevel3_g5a6r7p8r9o`
- `authChallenge`
- `pwdStr converted password`

Interpretation:

- the wake-focused strings are clustered much earlier in the image:
  - `DEVICE_BINDV2`
  - `TCWAKEUP`
  - `TSS wakeUpWifi`
  - `TSS connectBle wakeUpWifi value:H`
  - `sendBleWeakUp`
- the request-builder strings are clustered much later and are tightly grouped with:
  - `calculateAuthMd5`
  - `DEVICE_BINDV3`
  - `init:DEVICE_BINDV3`
- `sha256HmacHex` is present, but it is not co-located with the V3 builder cluster the way `calculateAuthMd5` is

Current best model:

- `DEVICE_BINDV2` likely owns the simpler wake-string path:
  - `TCWAKEUP`
  - `H`
  - `sendBleWeakUp`
- `DEVICE_BINDV3` likely owns the staged auth/session path:
  - `connectBle`
  - `calculateAuthMd5`
  - `createAuthRequest`
  - `createLoginRequest`
  - `createKeepAliveRequest`
- for the current `CAM8Z8_...` camera, the evidence now favors a V3 auth/login flow over a pure wake-string flow

Practical consequence:

- more blind ESP32 wake-token variants are low-value now
- the next useful breakthrough has to come from recovering the V3 serialized auth/login body shape

## Native Object Model Still Visible in Flutter Snapshot

Another pass over `libapp.so` confirms the Flutter side still exposes the generic BLE parser/builder model even though the actual Dart code is AOT-compiled.

Recovered strings:

- `BleMessageHeader`
- `BleMessageHeader.fromBytes`
- `BleMessageHeader{magic: 0x`
- `magicStart`
- `magicEnd`
- `msgType:0x`
- `TSS header.msgType:0x`
- `_intToBigEndian`
- `_bigEndianToInt`
- `BleProtocolUtils`
- `DeviceCodeProtocolV1`
- `DeviceCodeProtocolV1Obj`
- `DeviceCodeProtocolV1Obj{type: `
- `sendKeepAlive`
- `CommandStatus`
- `CommandStatusType`
- `BleConfigCameraStatus`
- `BleConfigCameraStatus.connectBleOk`
- `BleConfigCameraStatus.connectBleFailed`
- `BleConfigCameraStatus.reqGetApn commandStatus.status:`
- `BleConfigCameraStatus.reqSetApn commandStatus.status:`
- `BleConfigCameraStatus.commandStatusTypeError commandStatus.status:`
- `BleConfigCameraStatus.commandStatusTypeDisconnected:`
- `BleKeepAliveTimeoutException`
- `get:onValueReceived`
- `TSS connectBle onValue:`

Interpretation:

- the native snapshot still strongly supports a real binary protocol layer on top of BLE notifications:
  - explicit header object
  - explicit `magicStart` / `magicEnd`
  - explicit `msgType`
  - explicit big-endian helpers
- `DeviceCodeProtocolV1` and `BleMessageHeader` appear to coexist in the same app build:
  - one older/device-code framing family
  - one generic header/message family
- `sendKeepAlive` plus `BleKeepAliveTimeoutException` reinforces that the successful path is session-based rather than fire-and-forget
- the status names suggest the BLE path is expected to surface structured command outcomes, not just a raw wake string response

Current best next target:

- recover the serialized V3 `createAuthRequest` / `createLoginRequest` / `createKeepAliveRequest` shape closely enough to implement those builders on the ESP32 side

## Staged V3 Auth/Login/KeepAlive Signal

Another targeted native scan recovered a tighter session sequence than we had before.

Recovered auth/session strings:

- `_handleAuthResponse`
- `BLE auth failed`
- `TSS sendAuth finished`
- `sendAuth`
- `btCommandAuthOk`
- `requestLoginAuth`
- `TSS requestLoginAuth finished`
- `_sendLogin`
- `sendKeepAlive`
- `BLE keep alive timeout, please auth again4`
- `keep alive timeout`
- `BleAuthException`
- `BleKeepAliveTimeoutException`
- `AuthResponse`
- `Handling auth event:`
- `, auth: `
- `, authenticatedSignedWrites:`
- `authenticated_signed_writes`

Recovered offsets:

- `_handleAuthResponse` at `471108`
- `BLE auth failed` at `507670`
- `TSS sendAuth finished` at `771511`
- `Ble response invalid magic` at `817497`
- `header.fragIndex` at `975288`
- `, auth: ` at `1149752`
- `AuthResponse` at `1638236`
- `sendAuth` at `1728164`
- `btCommandAuthOk` at `1786240`
- `Handling auth event:` at `1925659`
- `requestLoginAuth` at `2263807`
- `BLE keep alive timeout, please auth again4` at `2525059`
- `keep alive timeout` at `2525063`
- `TSS requestLoginAuth finished` at `2897905`

Interpretation:

- the V3/native path now looks explicitly staged:
  1. `sendAuth`
  2. `_handleAuthResponse`
  3. `btCommandAuthOk`
  4. `_sendLogin` / `requestLoginAuth`
  5. `sendKeepAlive`
- this is much stronger evidence that the current camera is not waiting for a standalone wake token
- `AuthResponse`, `, auth: `, and `authenticated_signed_writes` suggest the auth response is parsed into structured fields rather than treated as a plain string
- `header.fragIndex` plus the invalid-magic / short-header errors reinforce that the reply parser expects a framed binary response with at least:
  - magic
  - msgType
  - fragment index
  - body length

Current best model:

- `sendAuth` likely emits a V3-framed auth request whose body is derived from:
  - `authChallenge`
  - `pwdStr converted password`
  - `calculateAuthMd5`
- a successful auth produces an `AuthResponse`
- only after that does the app send the login request
- a live session then requires periodic keepalive traffic and will force re-auth on timeout

Practical consequence:

- the next ESP32 implementation step should be a V3 session builder scaffold, not more wake-string probes
- the minimum useful builder set is now:
  - auth
  - login
  - keepalive

## Password Gate Signal Around Connect Flow

The password-check path is also visible as named status/log strings in the native snapshot.

Recovered strings and offsets:

- `BleConfigCameraStatus.connectBleOk` at `685429`
- `BleConfigCameraStatus.reqGetApn commandStatus.status:` at `686735`
- `step_5_checkPWD ==== 1 - no password required` at `1239433`
- `step_5_checkPWD ==== 2 - password required` at `1403612`
- `step_5_checkPWD` at `1943617`
- `BleConfigCameraStatus.reqSetApn commandStatus.status:` at `1949981`
- `BleConfigCameraStatus.connectBleFailed` at `2477658`
- `connect step step_5_checkPWD` at `2525806`
- `connect step step_5_checkPWD_OK_` at `2741979`

Interpretation:

- the app clearly has an explicit password gate in the WiFi bind flow
- there are at least two branches:
  - no password required
  - password required
- this gate still sits in the same broad connect/auth/status neighborhood as:
  - `connectBleOk`
  - `connectBleFailed`
  - `reqGetApn`
  - `reqSetApn`

Current caution:

- this does not yet prove whether `createAuthRequest` is only used for the password-required branch or for all `DEVICE_BINDV3` sessions
- but it does make it more plausible that the exact auth/login body depends on whether the camera reports password protection as enabled

## Flutter BLE Manager Package Layout

Recovered explicit AOT-retained file paths from the Flutter BLE manager package inside `libapp.so`.

Protocol and model files:

- `package:flutter_blue_plus_manager/src/bluetooth_manager.dart`
- `package:flutter_blue_plus_manager/src/models/bluetooth_command.dart`
- `package:flutter_blue_plus_manager/src/models/bluetooth_command_v1.dart`
- `package:flutter_blue_plus_manager/src/models/bluetooth_command_v2.dart`
- `package:flutter_blue_plus_manager/src/models/bluetooth_result.dart`
- `package:flutter_blue_plus_manager/src/models/command_status.dart`
- `package:flutter_blue_plus_manager/src/models/command_status_type.dart`
- `package:flutter_blue_plus_manager/src/protocol/base_response.dart`
- `package:flutter_blue_plus_manager/src/protocol/ble_command_handler.dart`
- `package:flutter_blue_plus_manager/src/protocol/ble_message.dart`
- `package:flutter_blue_plus_manager/src/protocol/ble_message_header.dart`
- `package:flutter_blue_plus_manager/src/protocol/ble_protocol.dart`
- `package:flutter_blue_plus_manager/src/protocol/ble_protocol_utils.dart`
- `package:flutter_blue_plus_manager/src/protocol/log.dart`
- `package:flutter_blue_plus_manager/src/protocol/models/ble_exception.dart`
- `package:flutter_blue_plus_manager/src/protocol/models/ble_models.dart`
- `package:flutter_blue_plus_manager/src/protocol/models/stk_menu.dart`
- `package:flutter_blue_plus_manager/src/protocol/stk_command_handler.dart`
- `package:flutter_blue_plus_manager/src/protocol/wakeup_command_handler.dart`

Recovered model/debug strings:

- `BluetoothCommand`
- `BluetoothCommandV1`
- `BluetoothCommandV1{data: `
- `BluetoothCommandV2`
- `BluetoothCommandV2{data: `
- `BluetoothResult{data: `
- `BaseResponse`
- `BaseResponseWithUrl`
- `BleBaseResponse`
- `BleBaseResponse.fromJson`
- `CommandStatus{status: `

Useful offset alignment:

- `ble_message_header.dart` at `607033`
- `ble_protocol_utils.dart` at `681703`
- `bluetooth_command_v1.dart` at `1168719`
- `bluetooth_command_v2.dart` at `1221772`
- `bluetooth_command.dart` at `1738460`
- `stk_command_handler.dart` at `1756576`
- `wakeup_command_handler.dart` at `2123189`
- `ble_command_handler.dart` at `2301581`
- `createLoginRequest` at `2642284`
- `createAuthRequest` at `2927400`
- `createKeepAliveRequest` at `2965771`
- `DEVICE_BINDV3` at `3024083`

Interpretation:

- the app’s BLE layer is intentionally split into:
  - low-level message/header/protocol utilities
  - versioned command models (`V1`, `V2`)
  - separate handlers for:
    - wakeup
    - generic BLE commands
    - STK commands
- the current evidence fits a model where:
  - `sendBleWeakUp` belongs to the wakeup handler / older bind family
  - auth/login/keepalive belong to the generic BLE command path
- the presence of both `BluetoothCommandV1` and `BluetoothCommandV2` means the next key question is not whether there are versions, but which command-model version backs `DEVICE_BINDV3`

Current best inference:

- `DEVICE_BINDV3` is more likely to use the newer generic command model path than the wakeup-only path
- `BluetoothCommandV2` is currently the strongest candidate command family for the staged auth/login/keepalive session

## Command Family Correlation: `BluetoothCommandV2` vs `DeviceCodeProtocolV1`

Ran a tighter correlation pass over the surviving model/debug strings and offsets.

Recovered offsets:

- `DeviceCodeProtocolV1` at `524459`
- `BleMessage` at `525046`
- `BluetoothCommand` at `838646`, `915705`
- `BluetoothCommandV1.` at `930254`
- `BleMessageHeader{magic: 0x` at `932483`
- `BluetoothCommandV1` at `1283495`
- `BluetoothCommandV2.` at `1519622`
- `BaseResponseWithUrl` at `1553725`
- `BleProtocolUtils` at `1573568`
- `BleBaseResponse.fromJson` at `1818517`
- `DeviceCodeProtocolV1Obj` at `1863092`
- `BluetoothCommandV2{data: ` at `2124343`
- `BleConfigCameraEvent{status: ` at `2124369`
- `BluetoothCommandV1{data: ` at `2371336`
- `BleMessageHeader.fromBytes` at `2385509`
- `DeviceCodeProtocolV1Obj{type: ` at `2744129`
- `BluetoothCommandV2` at `2920905`
- `createAuthRequest` at `2927400`
- `createKeepAliveRequest` at `2965771`
- `DeviceCodeProtocolV1` at `3107969`

Most important alignment:

- `BluetoothCommandV2` at `2920905`
- `createAuthRequest` at `2927400`

Those two are separated by only about `6495` bytes in the retained string table, while:

- `BluetoothCommandV1{data: ` is much earlier at `2371336`
- `DeviceCodeProtocolV1Obj{type: ` is later but still in a different neighborhood at `2744129`

Response-side strings in the same generic model family:

- `BaseResponseWithUrl`
- `BleBaseResponse.fromJson`
- `BluetoothResult{data: `
- `BleConfigCameraEvent{status: `
- `CommandStatus{status: `

Interpretation:

- the generic BLE command path appears to have:
  - command models
  - typed results/responses
  - typed event/status objects
- this matches the staged V3 auth/login/keepalive session much better than the older `DeviceCodeProtocolV1` path
- `BluetoothCommandV2` is now the strongest current candidate for the concrete request model behind:
  - `createAuthRequest`
  - `createLoginRequest`
  - `createKeepAliveRequest`

Current best model:

- `DeviceCodeProtocolV1` is likely an older or parallel binary format still retained in the app
- `BluetoothCommandV1` may back an older generic BLE path
- `BluetoothCommandV2` is the most likely command family for the current `DEVICE_BINDV3` WiFi bind/auth session

Practical consequence:

- the next ESP32-side builder hypothesis should center on:
  - `BleMessageHeader`
  - `BluetoothCommandV2{data: ...}`
  - parsed `BleBaseResponse` / `AuthResponse`
- `DeviceCodeProtocolV1` should remain a fallback branch, not the primary one

## Tentative V3 Schema Leakage

Another string pass recovered a few more surviving field names from the command/response model layer.

Recovered field/debug fragments:

- `BleMessageHeader{magic: 0x`
- `, msgType: 0x`
- `header.fragIndex`
- `,body length:`
- `BluetoothCommandV2{data: `
- `BluetoothCommandV1{data: `
- `{cmd:`
- `cmdIndex`
- `{cmdIndex=`
- `BluetoothResult{data: `
- `BleConfigCameraEvent{status: `
- `CommandStatus{status: `
- `AuthResponse.fromJson`
- `, auth: `
- `, authenticatedSignedWrites:`

Recovered offsets:

- `cmdIndex` at merged-string offset `5949086`
- `{cmdIndex=` at merged-string offset `6387689`
- `{cmd:` at `1396766`
- `BluetoothCommandV2{data: ` at `2124343`
- `BleConfigCameraEvent{status: ` at `2124369`
- `, authenticatedSignedWrites:` at `2182074`
- `BluetoothCommandV1{data: ` at `2371336`
- `AuthResponse.fromJson` at `2385156`
- `CommandStatus{status: ` at `2465606`

Tentative model-level reading:

- header layer:
  - `magic`
  - `msgType`
  - `fragIndex`
  - `body length`
- command layer:
  - `data`
  - likely nested command fields including:
    - `cmd`
    - `cmdIndex`
- response/event layer:
  - `status`
  - auth-specific response fields including:
    - `auth`
    - `authenticatedSignedWrites`

Current best tentative V3 envelope:

1. `BleMessageHeader`
   - framed binary header
   - big-endian helpers are present elsewhere in the snapshot
2. command body
   - likely a serialized `BluetoothCommandV2`
   - carrying at least a nested `cmd` selector and payload `data`
   - may also carry `cmdIndex` for request/response correlation or sequencing
3. response body
   - parsed into typed response/event models:
     - `AuthResponse`
     - `BluetoothResult`
     - `BleConfigCameraEvent`
     - `CommandStatus`

Current best auth-session hypothesis:

- auth request:
  - `BluetoothCommandV2`
  - `cmd` = auth-related opcode
  - `data` = challenge/password-derived payload
  - maybe `cmdIndex`
- login request:
  - same family, different `cmd`
- keepalive request:
  - same family, likely small or empty `data`

Practical consequence:

- the next ESP32-side experiment should stop varying the outer family and instead vary:
  - `msgType`
  - `cmd`
  - presence/absence of `cmdIndex`
  - empty vs non-empty `data`

## JSON Encoding Clue Near `BluetoothCommandV2`

The last missing question was whether the `BluetoothCommandV2` body is likely a packed binary struct or JSON text. Another native scan materially strengthened the JSON-text interpretation.

Recovered nearby strings:

- `BluetoothCommandV2.` at `1519622`
- `BaseResponseWithUrl` at `1553725`
- `BleBaseResponse.fromJson` at `1818517`
- `BluetoothResult{data: ` at `2042205`
- `jsonEncode` at `2106587`
- `ATTransmit.fromJson` at `2115108`
- `BluetoothCommandV2{data: ` at `2124343`
- `BleConfigCameraEvent{status: ` at `2124369`
- `AuthResponse.fromJson` at `2385156`

Interpretation:

- `jsonEncode` sits in the same broad neighborhood as:
  - `BluetoothCommandV2`
  - typed BLE response/event models
  - multiple `fromJson` parsers
- that makes it much more plausible that the generic `BluetoothCommandV2` path serializes request bodies as JSON text before framing them inside the BLE message/header layer
- this fits the surviving debug field names well:
  - `cmd`
  - `cmdIndex`
  - `data`
  - `status`
  - `auth`
  - `authenticatedSignedWrites`

Current best refined V3 model:

1. outer frame:
   - `BleMessageHeader`
   - big-endian style framing
   - `magic`
   - `msgType`
   - `fragIndex`
   - `body length`
2. inner request body:
   - likely JSON text encoding of a `BluetoothCommandV2`
   - likely fields:
     - `cmd`
     - optional `cmdIndex`
     - `data`
3. inner response body:
   - JSON-decoded into:
     - `AuthResponse`
     - `BluetoothResult`
     - `BleConfigCameraEvent`
     - `CommandStatus`

Practical consequence:

- the next ESP32 probe family should include framed JSON bodies, not only raw binary payload guesses

## `ATTransmit` Branch Clues

Another native pass exposed a stronger wake/AT branch than before.

Recovered strings:

- `reqATTransmit`
- `ATTransmit`
- `ATTransmit.fromJson`
- `createATTransmitWeakUpRequest`
- `createATTransmitRequest`
- `sendBleWeakUp`
- `isWeakUpCommand`
- `, isWeakUpCommand: `
- `AT+WAKEPULSE=50`
- `AT+WAKEPULSE=10`
- `AT+QSTKSTATE?`
- `AT+QSTKGI=`
- `AT+QSTKGI=33`
- `AT+QSTKGI=36`
- `AT+QSTKGI=37`
- `AT+QSTKRSP=`
- `AT+QSTKRSP=253,0,`

Recovered offsets:

- `reqATTransmit` at `503412`
- `isWeakUpCommand` at `791431`
- `ATTransmit` at `1316667`
- `, isWeakUpCommand: ` at `1641918`
- `sendBleWeakUp` at `1878450`
- `AT+WAKEPULSE=50` at `1995893`
- `ATTransmit.fromJson` at `2115108`
- `init:reqATTransmit` at `2236816`
- `createATTransmitWeakUpRequest` at `2829334`
- `createATTransmitRequest` at `2838471`

Interpretation:

- the wake path likely has a dedicated request model, not just a hardcoded single-character token
- `ATTransmit.fromJson` strongly suggests this branch also travels through typed request/response models
- `isWeakUpCommand` suggests the parser/dispatcher explicitly classifies some commands as wake commands
- the retained AT strings imply the wake request may carry an AT command body, potentially via the `ATTransmit` model

## Live Test: Raw `AT+WAKEPULSE` Strings

Tested direct raw ASCII writes against the live camera:

- `send2r AT+WAKEPULSE=50`
- `send2r AT+WAKEPULSE=10`
- `send4r AT+WAKEPULSE=50`

Observed result:

- all writes succeeded on transport
- no notifications
- no short `OK`
- no WiFi JSON

Interpretation:

- the `ATTransmit` branch is probably real, but the raw AT strings alone are still not sufficient on the observed Nordic path
- this weakens the simple guess that the wake request is just a plain-text `AT+WAKEPULSE=...` write
- if `ATTransmit` is the right branch, it likely still requires:
  - the correct outer framing
  - the correct command opcode
  - and possibly a structured wrapper/body rather than naked ASCII

## Live Test: Mixed `ATTransmit`-Inside-`BluetoothCommandV2` JSON

Used the new manual `v2json*` commands to test a hybrid hypothesis:

- outer frame:
  - current TSS-style big-endian frame guess
- inner body:
  - `BluetoothCommandV2`-style JSON
- `data`:
  - raw AT wake strings such as `AT+WAKEPULSE=50`

Tested on `6e400002`:

- `v2json2 01 01 AT+WAKEPULSE=50`
- `v2json2i 01 01 01 AT+WAKEPULSE=50`
- `v2json2 02 01 AT+WAKEPULSE=50`
- `v2json2i 02 01 01 AT+WAKEPULSE=50`
- `v2json2 01 01 AT+WAKEPULSE=10`
- `v2json2i 01 01 01 AT+WAKEPULSE=10`
- `v2json2 03 10 AT+WAKEPULSE=50`
- `v2json2i 03 10 01 AT+WAKEPULSE=50`

Observed result:

- all writes succeeded with response enabled
- no notifications
- no short `OK`
- no WiFi JSON
- post-test status remained:
  - connected
  - `notify_count=0`
  - all expected characteristics present

Interpretation:

- the hybrid guess is also too simple:
  - `BluetoothCommandV2` JSON body carrying raw AT text is not sufficient under the current frame/opcode assumptions
- this weakens another large branch of easy explanations:
  - wake is not just a raw AT string
  - and not just that same AT string placed inside a naive V2 JSON command wrapper

Current best remaining unknowns:

- the real opcode table:
  - `msgType`
  - command id / `cmd`
- whether `ATTransmit` has its own typed JSON shape distinct from the generic `{cmd,data}` guess
- whether auth/login requires a challenge-derived payload before any wake/status command is honored

## Typed Event/Response Families Around the Generic BLE Path

A final model sweep exposed more of the parsed response/event ecosystem, even though it still did not leak the numeric opcode table.

Recovered typed parsers/models:

- `RandomResponse.fromJson`
- `ATTransmit.fromJson`
- `RegCode.fromJson`
- `AuthResponse.fromJson`
- `ApnSetting.fromJson`
- `MqttAddr.fromJson`
- `VersionInfo.fromJson`
- `FactoryReset.fromJson`
- `TestHardwareResponse.fromJson`
- `BleConfigCameraEvent`
- `BleConfigCameraEvent{status: `
- `BluetoothResult{data: `

Recovered related command/event strings:

- `createBaseRegCodeRequest`
- `createVersionInfoRequest`
- `createFactoryResetRequest`
- `createSetMqttAddrRequest`
- `requestBaseRegCode`
- `requestCmdVersionInfo`
- `requestFactoryReset`
- `reqSetMqttAddr`
- `reqCmdVersionInfo`
- `reqFactoryReset`
- `btCommandSetMqttAddressOk`
- `btCommandRegCodeOk`
- `factoryResetOK`
- `BLE received message for ntfyBaseRegcode:`
- `TSSS requestBaseRegCode finished`
- `TSSS requestCmdVersionInfo finished`
- `TSS _handleRandomResponse 1`
- `TSS _handleRandomResponse 2`

Interpretation:

- the generic BLE path appears to have a fairly broad typed command table, not just auth/login/keepalive:
  - auth/session
  - APN/settings
  - regcode
  - MQTT address
  - version info
  - factory reset
  - random/fallback responses
- this strengthens the existing model that:
  - `BleMessageHeader` + generic command model + typed JSON responses is the main modern path
- it also suggests the wake branch may eventually feed into the same typed event/result layer rather than being a completely separate one-off transport

Current practical conclusion:

- we still do not have the numeric opcode mapping
- but the app clearly expects a structured command/event protocol with:
  - named request builders
  - typed success markers (`btCommand...Ok`)
  - typed parsed responses (`...fromJson`)

## First Request/Response Family Matrix

Collected the named request builders, typed response models, and success/finished markers into one coarse family table.

Request builders observed:

- auth/session:
  - `createAuthRequest` at `2927400`
  - `createLoginRequest` at `2642284`
  - `createKeepAliveRequest` at `2965771`
  - `createLogoutRequest` at `3021469`
- AT/wake:
  - `createATTransmitWeakUpRequest` at `2829334`
  - `createATTransmitRequest` at `2838471`
- APN/settings:
  - `createSetApnRequest` at `462033`
  - `createGetApnInfoRequest` at `778880`
  - `createSetAdvancedInfoRequest` at `2794965`
  - `createGetAdvancedInfoRequest` at `3025287`
  - `createSetBleNameRequest` at `1554865`
  - `createSetPinInfoRequest` at `2143312`
  - `createGetPinLockStatusRequest` at `823526`
  - `createSetMqttAddrRequest` at `3113387`
- device/system:
  - `createBaseRegCodeRequest` at `867032`
  - `createBatteryStatusRequest` at `989732`
  - `createVersionInfoRequest` at `1555101`
  - `createFactoryResetRequest` at `2010767`
  - `createNetworkStatusRequest` at `2240549`
  - `createCellularInfoRequest` at `1962885`
  - `createGetOtherNetworkInfoRequest` at `2906778`

Typed parsed responses observed:

- `AuthResponse.fromJson` at `2385156`
- `ATTransmit.fromJson` at `2115108`
- `ApnSetting.fromJson` at `2604278`
- `RegCode.fromJson` at `1624412`
- `VersionInfo.fromJson` at `3004320`
- `FactoryReset.fromJson` at `2955161`
- `RandomResponse.fromJson` at `1526485`
- `BleConfigCameraEvent{status: ` at `2124369`
- `BluetoothResult{data: ` at `2042205`

Success / completion markers observed:

- auth/session:
  - `btCommandAuthOk` at `1786240`
  - `TSS requestLoginAuth finished` at `2897905`
- network/system:
  - `TSSS requestNetworkStatus finished` at `2226685`
  - `TSSS requestBatteryStatus finished` at `2600505`
  - `TSSS requestBaseRegCode finished` at `2728352`
  - `TSSS requestCmdVersionInfo finished` at `3116802`
- config:
  - `btCommandSetMqttAddressOk` at `1988160`
  - `btCommandRegCodeOk` at `2779659`
  - `factoryResetOK`
- fallback/random:
  - `TSS _handleRandomResponse 1`
  - `TSS _handleRandomResponse 2`

Coarse family mapping:

- auth/session family:
  - requests:
    - auth
    - login
    - keepalive
    - logout
  - likely responses/events:
    - `AuthResponse`
    - `BleConfigCameraEvent`
    - `BluetoothResult`
  - success markers:
    - `btCommandAuthOk`
    - `requestLoginAuth finished`

- AT/wake family:
  - requests:
    - `createATTransmitWeakUpRequest`
    - `createATTransmitRequest`
  - likely response/event path:
    - `ATTransmit.fromJson`
    - maybe `RandomResponse`
  - likely dispatcher hints:
    - `isWeakUpCommand`
    - `sendBleWeakUp`

- APN/settings family:
  - requests:
    - get/set APN
    - advanced info
    - BLE name
    - PIN info
    - MQTT addr
  - likely responses/events:
    - `ApnSetting.fromJson`
    - `BleConfigCameraEvent`
    - `BluetoothResult`
  - success markers:
    - `btCommandSetMqttAddressOk`

- device/system family:
  - requests:
    - regcode
    - battery
    - version info
    - factory reset
    - network status
    - cellular info
    - other network info
  - likely responses/events:
    - `RegCode.fromJson`
    - `VersionInfo.fromJson`
    - `FactoryReset.fromJson`
    - `RandomResponse.fromJson`
    - `BleConfigCameraEvent`
  - success markers:
    - `btCommandRegCodeOk`
    - `requestNetworkStatus finished`
    - `requestBatteryStatus finished`
    - `requestBaseRegCode finished`
    - `requestCmdVersionInfo finished`

Interpretation:

- we still lack opcode values, but the app now clearly looks like it has a typed command table broken into a handful of semantic families
- for the current WiFi bind problem, the two highest-priority families remain:
  - auth/session
  - AT/wake
- the broad system/config families are useful mostly because they confirm the generic command path is real and extensive, not because they are immediate bind blockers

Current best next step:

- derive a tiny candidate opcode matrix only for:
  - auth/session
  - AT/wake
- keep the rest as lower-priority validation traffic once the session path is unlocked

## Live Test: Framed JSON `BluetoothCommandV2` Hypothesis

Updated and reflashed [gardepro_ble_recon/gardepro_ble_recon.ino](/home/matheau/esp32_camera/gardepro_ble_recon/gardepro_ble_recon.ino) with a new manual/sequence probe family that builds:

- JSON body:
  - `{"cmd":N,"data":"..."}`
  - or `{"cmd":N,"cmdIndex":I,"data":"..."}`
- wrapped inside the existing TSS-style big-endian frame hypothesis:
  - `[magicStart][msgType][length][json-body][magicEnd]`

New console commands added:

- `seq_v2json`
- `seq_v2json_idx`
- `v2json2 <msgTypeHex> <cmdHex> <text data>`
- `v2json2i <msgTypeHex> <cmdHex> <cmdIndexHex> <text data>`
- `v2json4 <msgTypeHex> <cmdHex> <text data>`
- `v2json4i <msgTypeHex> <cmdHex> <cmdIndexHex> <text data>`

Build/flash status:

- unrestricted Arduino compile completed successfully
- sketch flashed successfully to the attached ESP32-S3 on `/dev/ttyUSB0`

Live results against the camera:

1. `seq_v2json` on `6e400002`
   - sent:
     - `msgType=0x02 body={"cmd":2,"data":""}`
     - `msgType=0x03 body={"cmd":3,"data":""}`
     - `msgType=0x04 body={"cmd":4,"data":""}`
   - result:
     - all writes succeeded
     - `notify_count=0`
     - no short `OK`
     - no WiFi JSON

2. `seq_v2json_idx` on `6e400002`
   - sent:
     - `msgType=0x02 body={"cmd":2,"cmdIndex":1,"data":""}`
     - `msgType=0x03 body={"cmd":3,"cmdIndex":2,"data":""}`
     - `msgType=0x04 body={"cmd":4,"cmdIndex":3,"data":""}`
   - result:
     - all writes succeeded
     - `notify_count=0`
     - no short `OK`
     - no WiFi JSON

3. manual wake-style JSON probes on `6e400002`
   - `v2json2 01 01 H`
   - `v2json2i 01 01 01 H`
   - `v2json2 02 02 H`
   - `v2json2i 02 02 02 H`
   - result:
     - all writes succeeded
     - no notifications
     - no semantic reply

4. quick cross-check on `6e400004`
   - `v2json4 01 01 H`
   - `v2json4i 01 01 01 H`
   - result:
     - writes succeeded
     - no notifications
     - no semantic reply

Interpretation:

- the transport still accepts the framed JSON `BluetoothCommandV2` hypothesis on both Nordic-path write characteristics
- but this exact combination is still wrong in at least one important way:
  - wrong outer frame
  - wrong `msgType`
  - wrong `cmd`
  - wrong request ordering
  - or wrong `data` derivation for auth
- the negative result is still useful because it weakens the simpler guess:
  - `TSS-style frame` + `JSON body` + trivial `cmd` numbering + optional `cmdIndex`

Current best remaining gap:

- not whether the body might be JSON, but the exact opcode mapping and auth payload derivation
- especially:
  - real `msgType`
  - real `cmd`
  - whether `cmdIndex` is mandatory or semantic
  - actual `authChallenge` response handling / derived auth value

## Tightened Native Offset Matrix

Ran one narrower offset scan over `libapp.so` to reduce the current blind space to the smallest auth/wake neighborhoods.

Most useful offsets from that pass:

- auth/session cluster:
  - `sendAuth` at `1728164`
  - `btCommandAuthOk` at `1786240`
  - `requestLoginAuth` at `2263807`
  - `AuthResponse.fromJson` at `2385156`
  - `createLoginRequest` at `2642284`
  - `BluetoothCommandV2` at `2920905`
  - `createAuthRequest` at `2927400`
  - `createKeepAliveRequest` at `2965771`
  - `calculateAuthMd5` at `3022668`

- wake / AT cluster:
  - `ATTransmit.fromJson` at `2115108`
  - `createATTransmitWeakUpRequest` at `2829334`
  - `createATTransmitRequest` at `2838471`

- framing / body-shape clues:
  - `{cmd:` at `1396766`
  - `jsonEncode` at `2106587`
  - `, authenticatedSignedWrites: ` at `2182074`
  - `header.fragIndex` at `975288`
  - `authChallenge` at `566415`
  - `sha256HmacHex` at `694740`

Interpretation:

- the auth/session path is still the tightest and strongest modern branch:
  - `sendAuth`
  - `requestLoginAuth`
  - `BluetoothCommandV2`
  - `createAuthRequest`
  - `createKeepAliveRequest`
  - `calculateAuthMd5`
- `createLoginRequest` sits earlier than the main `BluetoothCommandV2` / `createAuthRequest` cluster, so string offset alone does not prove request order or opcode order
- the wake path still looks like a separate typed branch:
  - `ATTransmit.fromJson`
  - `createATTransmitWeakUpRequest`
  - `createATTransmitRequest`
- `jsonEncode` + `{cmd:` + `BluetoothCommandV2{data: ...}` still keeps the JSON-body interpretation alive, but only as a body hypothesis, not as proof of the current outer frame or opcode table
- `sha256HmacHex` is present, but it still sits far away from the tight V3 builder cluster; `calculateAuthMd5` remains the stronger immediate auth-derivation clue for the current bind path

Updated practical conclusion:

- the next productive work should stay focused on the auth/session branch first
- more wake-only probing is low value until one of these is recovered more concretely:
  - auth request body shape
  - login request body shape
  - candidate `cmd` values for auth/login/keepalive

## Minimal Runtime Pipeline View

Another ordered string pass helps separate runtime verbs from builder/model names.

Most relevant runtime-side markers:

- `TSS BLE _sendingCommandQueueAdd command:0x` at `543984`
- `TSS sendAuth finished` at `771511`
- `_sendLogin` at `870983`
- `TSS _sendLogin` at `1079148`
- `sendKeepAlive` at `1230522`
- `sendAuth` at `1728164`
- `sendBleWeakUp` at `1878450`
- `sendLogout` at `1936422`
- `TSS BLE _sendingCommandQueueRemove command:0x` at `2812302`
- `sendRaw` at `2978360`

Related builder/model markers in the same broad branch:

- `CommandStatus{status: ` at `2465606`
- `createLoginRequest` at `2642284`
- `createATTransmitWeakUpRequest` at `2829334`
- `createATTransmitRequest` at `2838471`
- `BluetoothCommandV2` at `2920905`
- `createAuthRequest` at `2927400`
- `createKeepAliveRequest` at `2965771`
- `createLogoutRequest` at `3021469`

Interpretation:

- there is clearly a queue-driven generic command pipeline:
  - add command
  - send command
  - parse/update status
  - remove command
- auth/session traffic still looks like the center of that generic queue-driven path
- `sendBleWeakUp` is present, but the AT/wake builder names remain separated enough that wake should still be treated as its own request family rather than assumed to be just one more generic auth/session `cmd`
- `sendRaw` appearing near `createKeepAliveRequest` is interesting, but not enough by itself to overturn the current JSON-body hypothesis; it only means there is at least one lower-level raw-send path inside the same implementation area

Current working rule:

- do not spend more live probes on wake-only strings
- prioritize recovering one generic queued request shape for:
  - auth
  - login
  - keepalive
- only come back to the AT/wake family after the generic session shape is less speculative

## Live Test: Object-Data and Raw-Body Variants

The previous `BluetoothCommandV2` helper only supported:

- `{"cmd":N,"data":"..."}`

That was too restrictive once the native strings started pointing toward structured models on both the request and response sides. Updated [gardepro_ble_recon/gardepro_ble_recon.ino](/home/matheau/esp32_camera/gardepro_ble_recon/gardepro_ble_recon.ino) to add:

- object-data probes:
  - `seq_v2obj`
  - `seq_v2obj_idx`
  - `v2obj2`
  - `v2obj2i`
  - `v2obj4`
  - `v2obj4i`
- raw-body probes:
  - `v2body2`
  - `v2body4`

These keep the same current TSS-style frame hypothesis, but allow:

- `{"cmd":N,"data":{...}}`
- or a completely raw JSON body under the frame

The updated sketch compiled cleanly and was flashed successfully to the attached Heltec `HT-HC33`.

Live results against the camera:

1. `seq_v2obj` on `6e400002`
   - sent:
     - `msgType=0x02 body={"cmd":2,"data":{}}`
     - `msgType=0x03 body={"cmd":3,"data":{}}`
     - `msgType=0x04 body={"cmd":4,"data":{}}`
   - result:
     - all writes succeeded
     - `notify_count=0`
     - no short `OK`
     - no WiFi JSON

2. `seq_v2obj_idx` on `6e400002`
   - sent:
     - `msgType=0x02 body={"cmd":2,"cmdIndex":1,"data":{}}`
     - `msgType=0x03 body={"cmd":3,"cmdIndex":2,"data":{}}`
     - `msgType=0x04 body={"cmd":4,"cmdIndex":3,"data":{}}`
   - result:
     - all writes succeeded
     - `notify_count=0`
     - no short `OK`
     - no WiFi JSON

3. raw-body auth checks on `6e400002`
   - `v2body2 02 {}`
   - `v2body2 02 {"auth":""}`
   - result:
     - writes succeeded
     - `notify_count=0`
     - no short `OK`
     - no WiFi JSON

Interpretation:

- the earlier failure was not just caused by treating `data` as a quoted string
- the current hypothesis is still wrong at a more fundamental level:
  - wrong outer frame
  - wrong `msgType`
  - wrong `cmd`
  - wrong request ordering
  - or auth requires derived non-empty request material before the camera will emit any reply
- the raw-body check also weakens the easy alternative:
  - that auth is simply a minimal JSON object under the current TSS-style frame

Updated practical cutoff:

- the next useful progress is unlikely to come from more live JSON-shape variations alone
- the highest-value remaining target is still recovery of:
  - real opcode mapping
  - real auth request field set
  - real ordering between auth, login, and wake/session maintenance

## Generic Builder Split: `_createRequest` vs `_createRequestWithData`

Another native pass exposed two generic request-builder helpers in `libapp.so`:

- `_createRequest` at `1138248`
- `_createRequestWithData` at `1683238`

Related package/model markers around that same broad area:

- `package:flutter_blue_plus_manager/src/models/bluetooth_command_v1.dart` at `1168719`
- `package:flutter_blue_plus_manager/src/models/bluetooth_command_v2.dart` at `1221772`
- `BluetoothCommandV1` at `1283495`
- `BluetoothCommandV2.` at `1519622`
- `BleConfigCameraEvent.` at `1530720`
- `BaseResponseWithUrl` at `1553725`
- `CommandStatus` at `1586384`

Higher-level request builders still relevant to the bind path:

- `createLoginRequest` at `2642284`
- `createATTransmitWeakUpRequest` at `2829334`
- `createATTransmitRequest` at `2838471`
- `BluetoothCommandV2` at `2920905`
- `createAuthRequest` at `2927400`
- `createKeepAliveRequest` at `2965771`
- `createLogoutRequest` at `3021469`
- `calculateAuthMd5` at `3022668`

Interpretation:

- the generic command family now looks even more likely to have two builder modes:
  - bodyless request
  - request-with-data
- this is a better fit for the observed request mix than treating every command as structurally identical

Current best body-shape classification:

- likely body-bearing:
  - `createAuthRequest`
  - `createLoginRequest`
  - `createATTransmitWeakUpRequest`
  - `createATTransmitRequest`
  - most `set*` requests
- likely bodyless or near-bodyless:
  - `createKeepAliveRequest`
  - `createLogoutRequest`
  - most status/info `get*` / `request*` commands

Why this matters:

- it weakens the current failed ESP32 pattern where auth/login/keepalive were all tested as peers with similarly trivial empty JSON bodies
- a more realistic model is:
  - auth/login go through `_createRequestWithData`
  - keepalive/logout may go through `_createRequest`
- if that split is correct, keepalive is a better candidate for a minimal/bodyless command than auth/login

Updated practical next step:

- stop treating auth, login, and keepalive as equally likely to accept the same body shape
- prioritize recovering:
  - auth request data fields
  - login request data fields
- keepalive can remain the leading candidate for a bodyless generic command once opcode mapping is less speculative

## Live Test: Builder-Split Matrix

Used the new builder split to run a tighter live matrix on `6e400002` instead of broad JSON-shape fuzzing.

Bodyless / near-bodyless candidates:

- `v2body2 04 {}`
- `v2body2 05 {}`

Data-bearing auth/login candidates using the strongest surviving field names:

- `v2obj2 02 02 {"auth":"","authenticatedSignedWrites":false}`
- `v2obj2 03 03 {"authChallenge":"","password":""}`
- `v2body2 02 {"auth":"","authenticatedSignedWrites":false}`
- `v2body2 03 {"authChallenge":"","password":""}`

Result:

- all writes succeeded with response enabled
- `status` afterward still showed:
  - `connected=yes`
  - `notify_count=0`
  - `ok=no`
  - `json=no`
- no short `OK`
- no WiFi JSON
- no notifications at all

Interpretation:

- treating keepalive/logout as bodyless under the current `msgType` guess did not unlock any response
- treating auth/login as data-bearing with the most obvious recovered field names also did not unlock any response
- that means the remaining unknown is now even less likely to be just:
  - string-vs-object-vs-raw JSON body shape
  - or bodyless-vs-body-bearing split by itself

Practical conclusion from this pass:

- the current TSS-style framing and/or `msgType`/`cmd` mapping is still probably wrong
- alternatively, the field names alone are insufficient because auth/login require non-empty derived values from logic we have not yet reconstructed
- more live testing should now be gated on recovering:
  - real opcode mapping
  - or the actual auth derivation path

## Auth-Derivation Tightening

Another auth-focused native scan materially tightened the likely derivation chain, even though it still does not reveal the exact formula.

Relevant offsets:

- `pwdStr converted password ` at `467379`
- `authChallenge` at `566415`
- `_hexEncode` at `612256`
- `sha256HmacHex` at `694740`
- `loginDeviceByBleLevel0` at `918125`
- `MD5V4` at `938315`
- `_loginDeviceByBleLevel2` at `1010006`
- `step_5_checkPWD ==== 1 - no password required` at `1239433`
- `MD5V3` at `1321576`
- `step_5_checkPWD ==== 2 - password required` at `1403612`
- `authenticated_signed_writes` at `1413936`
- `_hexEncode` at `1870508`
- `token: ` at `526573`
- `MD5V4 str:` at `2243889`
- `pwdStr` at `2251274`
- `calculateAuthMd5` at `3022668`

Interpretation:

- the app’s auth path still looks like:
  - start from `pwdStr`
  - normalize / convert password
  - combine with challenge state
  - compute an MD5-family auth value
  - hex-encode or stringify it into a token-like value
- the presence of both:
  - `MD5V3`
  - `MD5V4`
  - `MD5V4 str:`
  suggests multiple digest variants or formatting branches, not one universal auth formula
- `authenticated_signed_writes` is still present near the auth/session area, which fits a structured auth response carrying capability/state, not just a boolean success flag
- `sha256HmacHex` still exists, but it remains less tightly coupled to the WiFi bind/auth builder cluster than:
  - `calculateAuthMd5`
  - `MD5V3`
  - `MD5V4`

Important distinction from local repo context:

- [gardepro_dual_radio_bridge/README.md](/home/matheau/esp32_camera/gardepro_dual_radio_bridge/README.md) records hotspot password `1234567890` for SSID `CAM8Z8_A46DD49E4732`
- that hotspot credential should not currently be treated as evidence for the camera-auth payload itself
- the app strings still point to a separate local camera password concept:
  - 4-digit if enabled
  - otherwise password protection disabled

Current best auth model:

- WiFi hotspot join credential:
  - `1234567890`
- separate camera auth input:
  - empty / disabled
  - or 4-digit camera password
- derived BLE auth token:
  - likely produced from normalized password + challenge + MD5V3/MD5V4 branch + hex/string conversion

Updated practical next step:

- do not spend more live probes on raw hotspot-password reuse
- prioritize recovering the exact:
  - challenge source
  - password normalization
  - MD5V3 vs MD5V4 branch condition
  - final token field placement in auth/login requests

## No-BLE WiFi/HTTP Reality Check

Ran a dedicated WiFi-only ESP32 test to answer the practical question: does anything on the camera’s WiFi/HTTP side work before BLE bootstrap.

Method:

- flashed [gardepro_dual_radio_bridge/gardepro_dual_radio_bridge.ino](/home/matheau/esp32_camera/gardepro_dual_radio_bridge/gardepro_dual_radio_bridge.ino) in a WiFi-only self-test mode
- skipped the HaLow side entirely
- had the ESP:
  - scan for SSID `CAM8Z8_A46DD49E4732`
  - try to join with hotspot password `1234567890`
  - if joined, probe the known HTTP endpoints on `192.168.8.1:8080`
  - if not joined, rescan periodically and report hotspot visibility

Observed result with no BLE interaction first:

- initial connect attempt:
  - `Camera WiFi connect timed out`
- first rescan after boot:
  - `Target SSID CAM8Z8_A46DD49E4732 still not visible`

Interpretation:

- the limiting factor was not HTTP auth
- the camera hotspot itself was not visible to the ESP32 during the no-BLE test window
- so, for this camera in its current state:
  - no BLE bootstrap
  - no visible WiFi hotspot
  - therefore no reachable HTTP control plane

Practical conclusion:

- for this device, BLE is not just an optional extra for live view negotiation
- BLE bootstrap appears to be required earlier, at or before hotspot availability, at least in the current power/state flow we tested
- the previous question can now be answered more sharply:
  - it is not that HTTP exists but is merely unauthenticated
  - in the tested no-BLE state, WiFi never came up far enough to reach HTTP at all

## Exact BLE Traffic Recovered From Android HCI Snoop

Recovered a real HCI snoop from the attached Android tablet via bugreport. The bugreport contained:

- `FS/data/misc/bluetooth/logs/BT_HCI_2026_0420_185425.cfa.curf`

That file is a valid:

- `BTSnoop version 1, HCI UART (H4)`

The actual app traffic to `CAM8Z8_NoName_G_E6` / `a4:6d:d4:9e:47:32` is now confirmed for the first wake phase.

Observed ATT sequence:

1. service discovery confirms:
   - `6e400001` service
   - characteristic handles:
     - `0x0019` -> `6e400002`
     - `0x001b` -> `6e400003`
     - `0x001e` -> `6e400004`
   - CCCDs:
     - `0x001c` for `6e400003`
     - `0x001f` for `6e400004`

2. app enables notifications by writing:
   - handle `0x001c`
   - handle `0x001f`

3. app then writes three times to:
   - handle `0x001e`
   - UUID `6e400004-b5a3-f393-e0a9-e50e24dcca9e`
   - using ATT `Write Request`

Exact payload written each time:

- ASCII:
  - `AT+WAKEPULSE=10\r\n`
- hex:
  - `41 54 2B 57 41 4B 45 50 55 4C 53 45 3D 31 30 0D 0A`

Observed camera responses as notifications on the same handle:

1. first response:
   - `ERROR\r\n`
   - hex: `45 52 52 4F 52 0D 0A`

2. second response:
   - `OK\r\n`
   - hex: `4F 4B 0D 0A`

3. third response:
   - `OK\r\n`
   - hex: `4F 4B 0D 0A`

Timing from the snoop:

- write #1 at `31.851414`
- notify `ERROR` at `31.904365`
- write #2 at `32.196276`
- notify `OK` at `32.284230`
- write #3 at `32.573557`
- notify `OK` at `32.644586`

Interpretation:

- this is the first exact app-originated BLE command recovered from the real camera session
- the app is using:
  - `6e400004` as the write path for this phase
  - not `6e400002`
- line endings matter:
  - the real payload includes `\r\n`
- `AT+WAKEPULSE=10` is not a guess anymore; it is directly confirmed from the app session
- the earlier ESP tests missed at least these important details:
  - exact write characteristic
  - exact line ending
  - exact repetition cadence

Current best next step:

- replay this exact recovered wake sequence from the ESP32:
  - notify enable on `6e400003` and `6e400004`
  - write `AT+WAKEPULSE=10\r\n` to `6e400004`
  - repeat 3 times with roughly the observed cadence
- then keep the connection open and watch for what happens next

## Exact Replay Added To ESP32 Recon

The Heltec recon sketch now includes a dedicated exact-replay command:

- `seq_realwake`

That command replays the recovered app wake phase as closely as the current sketch allows:

- target characteristic: `6e400004`
- write mode: write-with-response
- payload: `AT+WAKEPULSE=10\r\n`
- attempts: `3`
- spacing: about `350ms`
- post-write hold: about `2s`

Purpose:

- verify that the ESP32 can reproduce the same immediate BLE behavior observed in the Android HCI snoop
- check whether the camera returns the same `ERROR\r\n`, `OK\r\n`, `OK\r\n` sequence when the exact payload, characteristic, and rough cadence are matched

## Live Replay Result On ESP32

The exact replay was compiled, flashed, and tested live on the attached Heltec.

Command used:

- `seq_realwake`

Observed ESP32 result:

- connection stable
- notify enabled on both:
  - `6e400003`
  - `6e400004`
- wrote `AT+WAKEPULSE=10\r\n` to `6e400004` three times using write-with-response
- received three notifications on `6e400004`
- each notification was:
  - `OK\r\n`

ESP-side summary:

- `notify_count=3`
- `ok=yes`
- `json=no`
- `last="OK"`

Interpretation update:

- the ESP32 can now reproduce the app's real wake phase on the correct characteristic with the correct CRLF-terminated payload
- the camera is no longer treating these writes as unknown/no-op traffic
- the remaining gap is now strictly what comes after wake:
  - additional BLE commands
  - hotspot appearance timing
  - WiFi/HTTP handoff

One small difference from the captured Android session:

- the Android snoop showed `ERROR\r\n`, `OK\r\n`, `OK\r\n`
- the ESP replay produced `OK\r\n`, `OK\r\n`, `OK\r\n`

That difference may just be timing/session-state dependent, but it is no longer a blocker; the wake command itself is confirmed working from the ESP side.

## Bridge Handoff Retest After Exact Wake

The dual-radio bridge sketch was updated to try a minimal BLE wake before any WiFi scan:

- scan for BLE MAC `a4:6d:d4:9e:47:32`
- discover the Nordic service
- register notify on:
  - `6e400003`
  - `6e400004`
- write `AT+WAKEPULSE=10\r\n` to `6e400004`
- then wait up to `60s` for hotspot `CAM8Z8_A46DD49E4732` to appear

Observed bridge-side result:

- `Target SSID CAM8Z8_A46DD49E4732 not visible yet after 8394 ms`
- `...`
- `Target SSID CAM8Z8_A46DD49E4732 not visible yet after 55224 ms`
- `[WiFi] hotspot visibility after BLE wake: no`
- `Camera WiFi connect timed out`
- runtime state:
  - `ble_attempted=yes`
  - `ble_ok=no`
  - `notify_count=0`
  - `last=`

Interpretation update:

- the standalone recon sketch can replay the exact wake command and get `OK\r\n`
- the current minimal BLE port inside the dual-radio bridge does **not** yet reproduce that same wake result
- so the present bridge failure does **not** prove that wake is insufficient by itself; it only proves that this first bridge-side BLE implementation is not equivalent to the proven recon path yet

Practical next step:

- factor the exact working BLE wake path from `gardepro_ble_recon.ino` into the bridge more faithfully instead of relying on the first minimal port
- only after that retest should the WiFi-hotspot dependency question be treated as settled

## End-To-End BLE-To-WiFi Retest Succeeded

After tightening the bridge-side BLE path to match the proven recon flow more closely, the bridge self-test succeeded end to end.

Key bridge-side changes that mattered:

- BLE warmup before init
- longer active scan with retries
- same scan interval/window style as the recon sketch
- `MTU=517`
- notify registration on both:
  - `6e400003`
  - `6e400004`
- exact wake replay on `6e400004` with:
  - `AT+WAKEPULSE=10\r\n`
  - write-with-response
  - three attempts
- keep the BLE session open while waiting for the hotspot to appear

Observed run:

- BLE stage:
  - found advertisement `a4:6d:d4:9e:47:32`
  - connected
  - discovered `5` services
  - received three `OK` notifications on `6e400004`
  - bridge summary:
    - `notify_count=3`
    - `ok=yes`
    - `last=OK`
    - `stage=wake_ok`

- hotspot timing:
  - not visible at `3385 ms`
  - visible at `11755 ms`
  - SSID:
    - `CAM8Z8_A46DD49E4732`

- WiFi:
  - connected successfully
  - bridge STA IP:
    - `192.168.8.30`

- HTTP self-test after BLE wake:
  - `/cmd/standby/reset` -> success
  - `/cmd/getParaSetting` -> success
  - `/cmd/info/1` -> success
  - `/cmd/info/2` -> success
  - `/cmd/info/3` -> success
  - `/cmd/info/4` -> success
  - `/cmd/info/5` -> success
  - `/cmd/info/6` -> `{ "code": -1 }`
  - `/list/detail/backward/900000/60` -> success
  - `/media/getIrStatus` -> success

Conclusion:

- yes, BLE bootstrap is needed for this camera in the tested state
- once the exact BLE wake is sent and the session is kept alive long enough, the hotspot comes up and the HTTP control plane becomes reachable
- the earlier “no hotspot after BLE wake” result was caused by an incomplete bridge-side BLE implementation, not by the wake command being insufficient

## Local Serial Test Mode

The bridge sketch now has a local serial-driven mode instead of bringing HaLow into the loop immediately.

Behavior in this mode:

- performs the proven BLE wake
- waits for hotspot visibility
- joins `CAM8Z8_A46DD49E4732`
- runs the HTTP self-test
- starts local UDP listeners on:
  - `25748`
  - `25749`
- stays up on serial for manual testing

Available serial commands:

- `help`
- `status`
- `selftest`
- `http <path>`
- `wake`

Verified serial command example:

- `http /cmd/info/1`
  - returned HTTP `200`
  - body included:
    - `brand: GardePro`
    - `product: E6`
    - `ver: V6.2.122 MCU V156`

Current runtime status in local serial mode:

- WiFi up on `192.168.8.30`
- BLE wake confirmed:
  - `notify_count=3`
  - `last=OK`
  - `ble_stage=wake_ok`
- UDP listeners active on:
  - `25748`
  - `25749`
- no UDP packets observed yet in this mode, which is expected until a live-view negotiation path targets the board

## Longer Android HCI Session Result

A longer Android tablet session was captured that included:

- repeated connect attempts
- eventual successful camera connection
- entry into live view
- holding the session open for a longer interval

The extracted HCI artifact was still:

- `FS/data/misc/bluetooth/logs/BT_HCI_2026_0420_185425.cfa.curf`

The new session appears later in the append-only log around frame `41581+` / time `4603s+`.

Recovered BLE sequence for that later session:

- notify enable on:
  - `0x001c`
  - `0x001f`
- three write requests to:
  - `0x001e` / `6e400004`
- exact payload each time:
  - `AT+WAKEPULSE=10\r\n`
- responses:
  - `OK\r\n`
  - `OK\r\n`
  - `OK\r\n`

Important negative result:

- no additional BLE writes or notifications were recovered after the three wake pulses
- no auth/login/keepalive-looking BLE traffic appeared in this later app session
- there was no second command family on another handle in the captured ATT window

Interpretation update:

- for this camera/app path, BLE appears to be used primarily for wake/bootstrap
- the app's later connection/live-view work is not visible as additional BLE GATT traffic in the captured session
- the next useful capture target is therefore not "more BLE after wake" unless another device/app build behaves differently
- the next useful target is the WiFi-side live-view start and any port/session negotiation on the camera hotspot side

## RTSP Live-View Bring-Up On The Bridge

The bridge-side RTSP path was then exercised directly against the camera after the proven BLE wake and hotspot join.

Confirmed RTSP discovery:

- `DESCRIBE rtsp://192.168.8.1/live.sdp` returns `200 OK`
- server banner:
  - `Server: rtsp_demo`
- SDP body:
  - aggregate control:
    - `a=control:rtsp://192.168.8.1/live.sdp`
  - media:
    - `m=video 0 RTP/AVP 96`
    - `a=rtpmap:96 H264/90000`
    - `a=control:rtsp://192.168.8.1/live.sdp/track1`

Important intermediate cutoff:

- `SETUP rtsp://192.168.8.1/live.sdp` failed with:
  - `461 Unsupported Transport`
- `SETUP rtsp://192.168.8.1/live.sdp/track1` succeeded with:
  - `Session: 12345678`
  - `Transport: RTP/AVP;ssrc=22345678;unicast;client_port=25748-25749;server_port=49152-49153`

Critical implementation detail recovered on the bridge:

- opening a fresh TCP socket per RTSP request was wrong for this camera/server
- `PLAY` returned:
  - `455 Method Not Valid In This State`
- once `DESCRIBE -> SETUP -> PLAY` were sent on one persistent RTSP TCP connection, `PLAY` returned `200 OK`

Live media result:

- RTP immediately arrived on the bridge UDP listener pair after successful `PLAY`
- primary media:
  - destination port `25748`
  - source port `49152`
  - packet sizes mostly around `1456` plus shorter tail fragments
- RTCP/control:
  - destination port `25749`
  - source port `49153`
  - first observed packet length `28`

Observed payload shape:

- primary packets show normal RTP-looking headers:
  - `80 60 ...`
  - `80 E0 ...`
- SDP codec declaration matches the packet stream:
  - H.264 payload type `96`

Current practical conclusion:

- BLE bootstrap: solved
- hotspot bring-up: solved
- HTTP control: solved
- RTSP discovery: solved
- RTSP session setup: solved
- live RTP delivery to board-selected UDP ports: solved

Remaining work is now transport/productization work, not protocol existence:

- depacketize or forward the H.264 RTP stream cleanly
- keep the session alive for longer runs if needed
- reintroduce HaLow only after the local serial live-view path is stable

## HaLow Tunnel Prototype Status

The bridge was then moved from "UDP forwarder idea" to a concrete first-pass tunnel implementation.

Current sketch behavior in `gardepro_dual_radio_bridge/gardepro_dual_radio_bridge.ino`:

- HaLow credentials:
  - SSID from local config
  - password from local config
- static HaLow address:
  - board IP `192.168.1.30`
  - gateway `192.168.1.1`
  - subnet `255.255.255.0`
- tunnel target:
  - Pi server `192.168.1.39:6000`
- media transport:
  - single outbound TCP connection from board to Pi
  - custom framed protocol with magic `GPRT`
  - control frames:
    - `START`
    - `STOP`
  - media frames:
    - stream `0` = primary RTP
    - stream `1` = secondary RTCP/control

Important design point:

- the ESP32 is still not repackaging into MP4/TS/WebRTC/etc.
- it forwards the camera-delivered RTP/RTCP payloads with only a thin framing layer
- this is intentional to keep board-side CPU/RAM pressure low

Pi-side receiver added locally:

- file:
  - `gardepro_tunnel_server.py`
- current behavior:
  - listens on `0.0.0.0:6000`
  - logs `START` / `STOP` metadata
  - fans primary media back out to local UDP `127.0.0.1:5004`
  - fans secondary media back out to local UDP `127.0.0.1:5005`
- syntax check:
  - `python3 -m py_compile gardepro_tunnel_server.py` passed

Bridge flashing status:

- updated sketch compiled successfully
- updated sketch was flashed successfully to the attached `HT-HC33` on `/dev/ttyUSB0`

Current blocker is no longer firmware build/flash.
The next proof step after reboot is operational:

1. start the Pi receiver:
   - `python3 /home/matheau/esp32_camera/gardepro_tunnel_server.py --verbose`
2. reconnect to the board serial console
3. run:
   - `stream_start`
4. confirm on the Pi:
   - `START` metadata frame received
   - RTP/RTCP packet counters rising
   - local UDP traffic present on `127.0.0.1:5004/5005`

## Post-Reboot Tunnel Validation and Local Playback

After reboot, the tunnel path was validated end to end on the Pi:

- the ESP32 reconnected over HaLow and delivered:
  - `START` metadata
  - sustained RTP/RTCP packet flow
- the Pi receiver wrote a local SDP file:
  - `/tmp/gardepro_live.sdp`
- `ffplay` could decode the forwarded stream from that SDP

Current practical playback command on the Pi:

```bash
ffplay -protocol_whitelist file,udp,rtp /tmp/gardepro_live.sdp
```

Observed result:

- live video is now working through:
  - BLE wake
  - camera hotspot
  - RTSP live view
  - ESP32 TCP tunnel
  - Pi UDP fanout
  - local `ffplay`

Remaining issue is stream quality, not basic functionality:

- playback still shows occasional:
  - RTP packet loss
  - H.264 decode corruption / concealment
  - short disconnects

## Current Tunnel Quality Diagnosis

One concrete bug was identified on the ESP32 tunnel writer:

- multiple tasks could write framed data to the same TCP tunnel socket concurrently
  - primary RTP task
  - secondary RTCP task
  - control-frame path
- because those writes were not serialized, frame headers and payloads could interleave on the TCP byte stream
- that is the strongest current explanation for:
  - receiver-side `bad_frames`
  - occasional decode corruption despite otherwise valid RTP

Mitigations now added to the ESP32 sketch:

- RTSP control socket kept open after `PLAY`
- periodic RTSP keepalive during active stream
- reduced per-packet serial logging pressure
- automatic tunnel reconnect attempts when local stream state is still active but the TCP tunnel socket is gone
- mutex-protected tunnel writes so only one task writes a framed tunnel message at a time

Current best next validation target:

1. flash the mutex/keepalive/reduced-logging build
2. run the Pi receiver with:
   - `python3 /home/matheau/esp32_camera/gardepro_tunnel_server.py --verbose --verbose-packets`
3. start the stream on the ESP32
4. verify whether:
   - receiver `bad_frames` drops to zero or near-zero
   - `ffplay` corruption frequency drops
   - session duration improves

## April 21, 2026 Runtime Update

The `bad_frames` issue was materially improved:

- a later Pi receiver run reported:
  - `bad_frames: 0`
  - `rtp_packets: 3679`
  - `rtcp_packets: 13`
  - `bytes_forwarded_primary: 4119022`
- local `ffplay` playback still worked, but the stream later stopped

New dominant failure mode from the ESP32 serial log:

- the board kept `tunnel_connected=yes`
- then the board reported `wifi=down`
- after that, repeated rescans no longer saw `CAM8Z8_A46DD49E4732`

Interpretation:

- the framing mutex fix appears to have removed the earlier TCP frame interleaving problem
- the remaining hard stop is the camera-side 2.4 GHz link disappearing mid-stream
- downstream `ffplay` decode errors after that point are secondary damage from missed RTP, not the primary cause

Additional mitigations added after this observation:

- disable normal WiFi power-save on the camera STA before connect
- avoid idle hotspot rescans while a stream is active
- track time since last primary / secondary RTP packet on the ESP32
- automatically recover the stream if camera WiFi disappears or primary RTP stalls for several seconds

## April 21, 2026 Recovery Validation

The automatic recovery path now works end to end.

Observed behavior on a later run:

- the receiver saw a clean `STOP` with:
  - `reason: "camera_wifi_lost"`
- the ESP32 immediately:
  - re-opened the BLE wake session
  - re-sent the known-good `AT+WAKEPULSE=10` wake token
  - waited for hotspot visibility
  - rejoined `CAM8Z8_A46DD49E4732`
  - re-ran RTSP `DESCRIBE` / `SETUP` / `PLAY`
  - reconnected the tunnel and emitted a fresh `START`

Receiver-side implication:

- the tunnel pipeline no longer dies permanently when the camera hotspot disappears
- instead, the session now tears down cleanly and comes back on its own

Strong current hypothesis:

- the camera hotspot still appears to expire on an internal timer of roughly one minute
- this does not look like TCP tunnel corruption anymore
- a periodic BLE wake pulse was tried during active stream, but it did not obviously prevent the next `camera_wifi_lost` event

Additional cleanup after that test:

- removed `/cmd/standby/reset` from the automatic HTTP self-test path so normal bring-up no longer mutates camera state unnecessarily

## April 21, 2026 HaLow Addressing And Server Control

The Heltec HaLow static-IP path is not currently trustworthy enough to depend on.

Observed behavior:

- the sketch requests static HaLow `192.168.1.30/24`
- the live associated HaLow address still comes up as `192.168.1.157`
- the actual live HaLow MAC for that address is:
  - `78:72:64:E4:57:00`

Interpretation:

- `HaLow.config(...)` is either advisory or broken in this library stack
- the server should not assume the ESP32 will actually keep the requested static IP

Implemented fallback architecture:

- on every successful HaLow association, the ESP32 now sends a `REGISTER` control frame to the Pi receiver
- that registration contains:
  - `halow_ip`
  - `halow_mac`
  - `halow_bssid`
  - `halow_rssi`
  - `halow_ssid`
  - `halow_gateway`
- the receiver persists the latest board registration to:
  - `/tmp/gardepro_board_registration.json`

Server-side control path now implemented:

- the ESP32 HTTP bridge on port `18080` exposes:
  - `GET /status`
  - `POST /control/bringup`
  - `POST /control/stream_start`
  - `POST /control/stream_stop`
- the helper script:
  - `gardepro_server_control.py`
  - resolves the current board IP from `/tmp/gardepro_board_registration.json`
  - then calls the board HTTP endpoint automatically

This means the server no longer needs a fixed ESP32 HaLow IP in order to control the board.
The practical current options are:

- best operational path:
  - add a DHCP reservation for `78:72:64:E4:57:00`
- current code path already works without that:
  - rely on `REGISTER` + `gardepro_server_control.py`

## April 21, 2026 Board HTTP Control Surface Reworked For Server Use

The original board HTTP control path was still too synchronous for a real server:

- `POST /control/bringup` blocked while BLE wake, hotspot wait, and camera WiFi join ran
- server-side callers could time out before the ESP32 finished
- only a small fixed set of camera endpoints was exposed

That is now reworked into a better server-facing shape.

Board-side changes:

- `POST /control/bringup`
- `POST /control/stream_start`
- `POST /control/stream_stop`

now queue work and return immediately with `202 Accepted`.

A background worker task on the ESP32 performs the actual action.

`GET /status` now reports control-task state:

- `control_busy`
- `control_pending`
- `control_action`
- `control_last_action`
- `control_last_ok`
- `control_last_message`
- `control_active_ms`
- `control_last_finished_ms`

Board-side camera routes also expanded:

- generic JSON-capable camera request proxy:
  - `GET /camera/request?path=/cmd/...`
  - `POST /camera/request?method=POST&path=/cmd/...&content_type=application/json`
- raw camera fetch passthrough:
  - `GET /camera/raw?path=/...`
- added fixed info routes:
  - `/camera/info/1`
  - `/camera/info/2`
  - `/camera/info/3`
  - `/camera/info/4`
  - `/camera/info/5`
  - `/camera/info/6`

Python integration changes:

- added reusable module:
  - `gardepro_server_api.py`
- `gardepro_server_control.py` now wraps that module instead of duplicating ad hoc logic
- the Python API now:
  - resolves board IP from `/tmp/gardepro_board_registration.json`
  - polls `/status` to wait for queued bringup / stream actions
  - exposes helpers for:
    - `status()`
    - `bringup()`
    - `stream_start()`
    - `stream_stop()`
    - `get_settings()`
    - `get_gallery()`
    - `get_info(index)`
    - `camera_request_json(method, path)`
    - `download_to_file(path, dest)`

Additional app-derived HTTP path inventory now recorded separately:

- [GARDEPRO_CAMERA_HTTP_CANDIDATES.md](/home/matheau/esp32_camera/GARDEPRO_CAMERA_HTTP_CANDIDATES.md)

That file keeps a hard distinction between:

- confirmed camera paths tested on the real device
- candidate paths extracted from `apk_extract/lib/arm64-v8a/libapp.so`

App-derived candidate camera paths now include:

- `/cmd/getSetting`
- `/cmd/setSetting`
- `/cmd/delete/`
- `/cmd/format/start`
- `/cmd/format/result`
- `/cmd/reboot`
- `/cmd/resetFact`
- `/cmd/setGmtClock`
- `/cmd/upgrade/start`
- `/cmd/upgrade/result`
- `/cmd/standby/now`
- `/media/pic/take`
- `/media/pic/result`
- `/media/setDayNightMode`
- `/media/video/start`
- `/media/video/stop?`
- `/file/`

Additional live validation completed after that first note:

- `/cmd/getSetting` is confirmed live and returns a full settings object
- `/cmd/standby/now` is confirmed live and returns `{ "code": 0 }`
- `/media/pic/take` is confirmed live
- `/media/pic/result` is confirmed live
- `/media/video/start` is confirmed live
- `/media/video/stop` is confirmed live
- `/cmd/reboot` is confirmed live
- `/cmd/delete/1/114` is confirmed live
- `/cmd/delete/114/1` is confirmed live

Observed side effects from those live tests:

- `take-picture` created new gallery item `111`
- `/cmd/info/3` photo count incremented from `110` to `111`
- gallery list updated with the new item at the top
- delete syntax findings:
  - plain id or uid forms were rejected:
    - `/cmd/delete/114` -> `wrong file para`
    - `/cmd/delete/982b6007` -> `wrong file para`
  - structured forms with `type` and `id` succeeded:
    - `/cmd/delete/1/114`
    - `/cmd/delete/114/1`
  - uid-based structured forms still failed:
    - `/cmd/delete/1/982b6007` -> `file does not exist`
    - `/cmd/delete/982b6007/1` -> `wrong file para`
  - extension-based delete is now confirmed live:
    - `/cmd/delete/128/JPG` -> `success`
    - `/cmd/delete/128/mp4` -> `success`
  - the legacy type/id form still also works for video:
    - `/cmd/delete/2/128` -> `success`
  - after the successful delete tests, gallery item `114` disappeared and photo count dropped from `114` to `112`
- `/cmd/reboot` caused a longer outage than ordinary hotspot expiry:
  - the first post-reboot automated `bringup` failed with `bringup_failed`
  - a later manual wake + `bringup` succeeded
  - this suggests camera reboot recovery should be treated as a separate path from ordinary hotspot-loss recovery

Settings-write probing outcome:

- `setGmtClock` is real but current query guesses are wrong:
  - `/cmd/setGmtClock`
  - `/cmd/setGmtClock?tz=US/Eastern`
  - `/cmd/setGmtClock?time_zone=US/Eastern`
  all returned `code: -3`
- `setSetting` is real but current query guesses are wrong:
  - `/cmd/setSetting`
  - `/cmd/setSetting?date_format=1`
  - `/cmd/setSetting?time_format=0`
  - `/cmd/setSetting?standby_timeout=300`
  all returned `code: -2`
- confirmed working write syntax is `POST` JSON through the board proxy:
  - `POST /cmd/setSetting` with `{"data":{"date_format":0}}` returned `code: 0`
  - `/cmd/getSetting` afterward showed `date_format: 0`
  - `POST /cmd/setSetting` with `{"data":{"date_format":1}}` returned `code: 0`
  - `/cmd/getSetting` afterward showed `date_format: 1`
  - `POST /cmd/setGmtClock` with `{"data":"2026-04-21 20:05:00"}` returned `code: 0, desc: success`

Current file-download status:

- app strings still point to a `/file/` camera path family
- the exact shape is now partially confirmed live
- examples tried:
  - `/file/111`
  - `/file/111.JPG`
  - `/file/2f1a892d`
  - `/file/114`
  - `/file/1/115`
  - `/file/115/1`
  - `/file/2/112`
  - `/thumb/126/JPG`
  - `/thumb/112/JPG`
  - `/file/126/JPG`
  - `/file/112/MP4`
  - `/file/112/mp4`
- observed results:
  - camera-side `500` for `/file/114`
  - camera-side `500` for `/file/1/115`
  - connection resets for `/file/111.JPG`, `/file/2f1a892d`, `/file/115/1`, and `/file/2/112`
  - `/thumb/126/JPG` downloaded as a valid `320x180` JPEG thumbnail
  - `/thumb/112/JPG` downloaded as a valid `320x180` JPEG thumbnail for a video item
  - `/file/126/JPG` downloaded as a valid `2560x1440` JPEG
  - `/file/112/MP4` failed
  - `/file/112/mp4` downloaded as a valid MP4 file

Interpretation:

- photo download is confirmed at `/file/<id>/JPG`
- thumbnail download is confirmed at `/thumb/<id>/JPG`
- video download is confirmed at `/file/<id>/mp4`
- extension-based delete is the cleanest confirmed canonical form:
  - photo delete: `/cmd/delete/<id>/JPG`
  - video delete: `/cmd/delete/<id>/mp4`
- legacy type/id delete should be treated as compatibility fallback, not the primary form
- extension case matters for video download: uppercase `MP4` failed while lowercase `mp4` succeeded
- the board raw relay had to be fixed to dechunk camera responses before full-file downloads became valid

Idle hotspot keepalive update:

- the ESP32 sketch now sends periodic `GET /cmd/standby/reset` while camera WiFi is up, even when no stream is active
- this replaced the earlier non-stream idle behavior where no HTTP keepalive was sent unless live view was active
- live serial observation after flashing the new build showed repeated successful idle keepalive cycles:
  - `Camera GET /cmd/standby/reset -> 200, 14 bytes`
  - `[http] keepalive ok=yes status=200 bytes=14`
  - status stayed `wifi=up ip=192.168.8.30` across the full watch window
- this does not fully prove all long-run stability yet, but it materially improves the non-stream session hold-open path and aligns with the external `gardepro-fetcher` findings
- additional soak result after explicitly bringing HaLow up during the same idle session:
  - camera WiFi stayed up as `192.168.8.31`
  - HaLow stayed up as `192.168.1.157`
  - repeated board `/status` calls succeeded remotely while idle keepalives continued
  - observed HaLow RSSI stayed around `-58` to `-59`
- practical conclusion: the earlier non-stream instability appears to have been substantially reduced for at least the short-to-medium idle window, provided HaLow is actually brought up on the local-serial-mode build

Build state:

- Python modules compile cleanly
- the ESP32 sketch compiles cleanly
- the updated sketch was flashed to the attached board

What is still not finished:

- camera settings write endpoints are not yet mapped to friendly API methods
- camera media download/delete routes still need confirmed camera-side paths
- after the reflash, the board was not immediately reachable on HaLow for a final live HTTP verification, so this section records the compiled/flashed interface rather than a fresh end-to-end runtime proof

## Local Desktop / RDP State Before Reboot

This came up while preparing to test the Pi receiver after reboot, so it is worth recording here.

Observed state:

- local desktop default was Wayland:
  - active session showed `Type=wayland`
  - LightDM was configured for:
    - `user-session=rpd-labwc`
    - `autologin-session=rpd-labwc`
- xrdp itself was healthy:
  - service running
  - listener on TCP `3389`
- xrdp login succeeded but the Pi `rpd-x` desktop session died immediately
- forcing plain `openbox` through `~/.xsession` allowed xrdp to connect, but only to a blank desktop

Configuration changes made:

- LightDM was switched to X11 defaults in `/etc/lightdm/lightdm.conf`:
  - `greeter-session=pi-greeter`
  - `user-session=rpd-x`
  - `autologin-session=rpd-x`
- a temporary xrdp test override now exists:
  - `~/.xsession`
  - contents:
    - `exec /usr/bin/openbox`

Implication for after reboot:

- the machine should come up using X11 by default instead of Wayland
- but xrdp may still land in the temporary plain-openbox session until `~/.xsession` is removed or replaced
- if the goal after reboot is normal Pi X11 desktop over xrdp, revisit `~/.xsession`
