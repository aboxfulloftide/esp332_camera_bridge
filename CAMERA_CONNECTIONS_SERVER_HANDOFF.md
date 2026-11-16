# Camera Connections Table: Server Handoff

Date: 2026-11-16

Goal: record every attempt to connect to a trail camera, successful or not, with
the time, the camera Wi-Fi signal strength and the camera Bluetooth (BLE) signal
strength. Timings, the outcome and link quality go in the same row.

All of this work is on the server at `192.168.1.42`, in
`/var/www/html/trail_cam/index.php` (PHP 8 + MySQL). Nothing changes in nginx,
in the `mysql` credentials file, or in any existing table.

Client changes (`tools/pi_trail_job.py`, `tools/pi_radio_test.py`) are tracked
separately. The server side should be deployed first so the endpoint exists
before any client sends to it.

## 0. Back up

There is no git checkout on the server, so copy the file first:

```bash
cd /var/www/html/trail_cam
cp -p index.php index.php.bak-20261116
```

Rollback: `cp -p index.php.bak-20261116 index.php`. The new table can stay;
nothing else reads it.

## 1. Schema: add the table to `install_schema()`

`install_schema()` (around line 672) runs on the first `db()` call of each
request and only uses `CREATE TABLE IF NOT EXISTS`, so appending one more
statement is safe. Add this at the end of the function, after
`board_status_snapshots`:

```php
    $pdo->exec("
        CREATE TABLE IF NOT EXISTS camera_connections (
            id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
            scanner_host VARCHAR(128) NOT NULL,
            camera VARCHAR(64) NOT NULL,
            job_id VARCHAR(128) NULL,

            started_at DATETIME NOT NULL,
            connected_at DATETIME NULL,
            ended_at DATETIME NULL,

            ok TINYINT(1) NOT NULL,
            failure_stage VARCHAR(64) NULL,
            failure_message VARCHAR(255) NULL,

            wake_method VARCHAR(32) NULL,
            wake_attempts SMALLINT UNSIGNED NULL,

            ble_rssi_dbm SMALLINT NULL,
            wifi_rssi_dbm SMALLINT NULL,
            wifi_rssi_min SMALLINT NULL,
            wifi_rssi_avg DECIMAL(5,1) NULL,
            wifi_rssi_max SMALLINT NULL,
            wifi_freq_mhz SMALLINT UNSIGNED NULL,
            wifi_tx_bitrate VARCHAR(64) NULL,
            wifi_rx_bitrate VARCHAR(64) NULL,

            ble_scan_ms INT UNSIGNED NULL,
            ble_connect_ms INT UNSIGNED NULL,
            hotspot_wait_ms INT UNSIGNED NULL,
            wifi_join_ms INT UNSIGNED NULL,
            total_connect_ms INT UNSIGNED NULL,

            link_losses SMALLINT UNSIGNED NULL,
            items_downloaded INT UNSIGNED NULL,
            bytes_downloaded BIGINT UNSIGNED NULL,
            avg_kbps DECIMAL(10,1) NULL,

            details JSON NULL,
            payload JSON NOT NULL,
            created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

            UNIQUE KEY uniq_camconn_attempt (scanner_host, camera, started_at),
            INDEX idx_camconn_time (started_at),
            INDEX idx_camconn_camera_time (camera, started_at),
            INDEX idx_camconn_ok (ok, started_at)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
    ");
```

Column notes:

| Column | Meaning |
|---|---|
| `scanner_host` | Which bridge made the attempt (Pi, desktop, ESP32 board hostname). |
| `camera` | `e6_plus`, `e6_original`, and so on. |
| `job_id` | Staging job folder name, e.g. `job_20261115-070003_e6_plus`. |
| `started_at` | When the attempt began (UTC). Part of the retry key. |
| `connected_at` | When camera HTTP first answered. NULL if it never did. |
| `ended_at` | When the session closed. Filled in by a later re-send. |
| `ok` | 1 if the camera was reachable over HTTP, otherwise 0. |
| `failure_stage` | `ble_not_found`, `ble_connect`, `hotspot_timeout`, `wifi_join`, `camera_http`, or NULL. |
| `wake_method` | `none` (hotspot already up), `bleak`, or `direct`. |
| `ble_rssi_dbm` | Bluetooth signal from the BLE scan. NULL when no scan ran (`none`/`direct`). |
| `wifi_rssi_dbm` | Camera Wi-Fi signal right after joining. |
| `wifi_rssi_min/avg/max` | Camera Wi-Fi signal over the whole session (from the sampler). |
| `*_ms` | Time spent in each bring-up step. |
| `link_losses` | Camera Wi-Fi rejoins during the session. |
| `items_downloaded`, `bytes_downloaded`, `avg_kbps` | What the session got done. |
| `details` | Free-form JSON for anything else (BLE adapter, notify text, scan %). |
| `payload` | The full request row as received, like the other `board_*` tables. |

Signal values are stored as dBm (negative integers). SMALLINT is signed, so
values like -97 fit.

## 2. Handlers

Add these near `handle_board_events()` (around line 3678). They use the existing
helpers `db()`, `sql_datetime()`, `json_col()`, `json_response()` and `ApiError`.

`sql_datetime(null)` returns "now", which is wrong for the optional time
columns, so there's a small wrapper that keeps NULL as NULL.

```php
function camconn_dt(mixed $value): ?string
{
    return ($value === null || $value === '') ? null : sql_datetime((string)$value);
}

function camconn_int(mixed $value): ?int
{
    return ($value === null || $value === '' || !is_numeric($value)) ? null : (int)round((float)$value);
}

function camconn_num(mixed $value): ?float
{
    return ($value === null || $value === '' || !is_numeric($value)) ? null : (float)$value;
}

function camconn_str(mixed $value, int $max): ?string
{
    return ($value === null || $value === '') ? null : mb_substr((string)$value, 0, $max);
}

/**
 * POST /api/board/camera-connections
 * Body: {"scanner_host": "...", "connections": [ {...}, ... ]}
 * Retry-safe: (scanner_host, camera, started_at) identifies an attempt. Re-sending
 * the same attempt updates it, so a client can send once at connect time and again
 * at session end with ended_at/throughput filled in.
 */
function handle_camera_connections(array $body): void
{
    $host = $body['scanner_host'] ?? null;
    $rows = $body['connections'] ?? null;
    if (empty($host) || !is_array($rows)) {
        throw new ApiError('invalid_request', 'scanner_host and connections are required.');
    }

    $cols = [
        'scanner_host', 'camera', 'job_id', 'started_at', 'connected_at', 'ended_at',
        'ok', 'failure_stage', 'failure_message', 'wake_method', 'wake_attempts',
        'ble_rssi_dbm', 'wifi_rssi_dbm', 'wifi_rssi_min', 'wifi_rssi_avg', 'wifi_rssi_max',
        'wifi_freq_mhz', 'wifi_tx_bitrate', 'wifi_rx_bitrate',
        'ble_scan_ms', 'ble_connect_ms', 'hotspot_wait_ms', 'wifi_join_ms', 'total_connect_ms',
        'link_losses', 'items_downloaded', 'bytes_downloaded', 'avg_kbps',
        'details', 'payload',
    ];
    $keyCols = ['scanner_host', 'camera', 'started_at'];
    $updates = [];
    foreach ($cols as $c) {
        if (in_array($c, $keyCols, true)) continue;
        // Keep values already stored when a re-send leaves a field out (NULL).
        $updates[] = in_array($c, ['ok', 'payload'], true)
            ? "$c = VALUES($c)"
            : "$c = COALESCE(VALUES($c), $c)";
    }
    $sql = 'INSERT INTO camera_connections (' . implode(', ', $cols) . ') VALUES ('
        . implode(', ', array_fill(0, count($cols), '?')) . ') ON DUPLICATE KEY UPDATE '
        . 'id = LAST_INSERT_ID(id), ' . implode(', ', $updates);

    $pdo = db();
    $stmt = $pdo->prepare($sql);
    $ids = [];
    $skipped = 0;
    foreach ($rows as $r) {
        if (!is_array($r) || empty($r['camera']) || empty($r['started_at']) || !array_key_exists('ok', $r)) {
            $skipped++;
            continue;
        }
        $stmt->execute([
            mb_substr((string)$host, 0, 128),
            mb_substr((string)$r['camera'], 0, 64),
            camconn_str($r['job_id'] ?? null, 128),
            sql_datetime((string)$r['started_at']),
            camconn_dt($r['connected_at'] ?? null),
            camconn_dt($r['ended_at'] ?? null),
            (int)(bool)$r['ok'],
            camconn_str($r['failure_stage'] ?? null, 64),
            camconn_str($r['failure_message'] ?? null, 255),
            camconn_str($r['wake_method'] ?? null, 32),
            camconn_int($r['wake_attempts'] ?? null),
            camconn_int($r['ble_rssi_dbm'] ?? null),
            camconn_int($r['wifi_rssi_dbm'] ?? null),
            camconn_int($r['wifi_rssi_min'] ?? null),
            camconn_num($r['wifi_rssi_avg'] ?? null),
            camconn_int($r['wifi_rssi_max'] ?? null),
            camconn_int($r['wifi_freq_mhz'] ?? null),
            camconn_str($r['wifi_tx_bitrate'] ?? null, 64),
            camconn_str($r['wifi_rx_bitrate'] ?? null, 64),
            camconn_int($r['ble_scan_ms'] ?? null),
            camconn_int($r['ble_connect_ms'] ?? null),
            camconn_int($r['hotspot_wait_ms'] ?? null),
            camconn_int($r['wifi_join_ms'] ?? null),
            camconn_int($r['total_connect_ms'] ?? null),
            camconn_int($r['link_losses'] ?? null),
            camconn_int($r['items_downloaded'] ?? null),
            camconn_int($r['bytes_downloaded'] ?? null),
            camconn_num($r['avg_kbps'] ?? null),
            isset($r['details']) ? json_col($r['details']) : null,
            json_col($r),
        ]);
        $ids[] = (int)$pdo->lastInsertId();
    }
    json_response(['ok' => true, 'saved' => count($ids), 'skipped' => $skipped, 'ids' => $ids], 200, false);
}

/**
 * GET /api/board/camera-connections?camera=e6_plus&scanner_host=...&since=2026-11-01&ok=0&limit=200
 */
function list_camera_connections(array $q): array
{
    $where = [];
    $args = [];
    foreach (['camera', 'scanner_host'] as $f) {
        if (!empty($q[$f])) { $where[] = "$f = ?"; $args[] = (string)$q[$f]; }
    }
    if (!empty($q['since'])) { $where[] = 'started_at >= ?'; $args[] = sql_datetime((string)$q['since']); }
    if (isset($q['ok']) && $q['ok'] !== '') { $where[] = 'ok = ?'; $args[] = (int)(bool)$q['ok']; }
    $limit = max(1, min(1000, (int)($q['limit'] ?? 200)));
    $sql = 'SELECT id, scanner_host, camera, job_id, started_at, connected_at, ended_at, ok, failure_stage,
                   failure_message, wake_method, wake_attempts, ble_rssi_dbm, wifi_rssi_dbm, wifi_rssi_min,
                   wifi_rssi_avg, wifi_rssi_max, wifi_freq_mhz, wifi_tx_bitrate, wifi_rx_bitrate,
                   ble_scan_ms, ble_connect_ms, hotspot_wait_ms, wifi_join_ms, total_connect_ms,
                   link_losses, items_downloaded, bytes_downloaded, avg_kbps, details
            FROM camera_connections'
        . ($where ? ' WHERE ' . implode(' AND ', $where) : '')
        . " ORDER BY started_at DESC LIMIT $limit";
    $stmt = db()->prepare($sql);
    $stmt->execute($args);
    $rows = $stmt->fetchAll();
    foreach ($rows as &$row) {
        $row['details'] = $row['details'] !== null ? json_decode($row['details'], true) : null;
    }
    return $rows;
}
```

## 3. Routes

In the router, next to the other `/api/board/...` routes (around line 4939), add:

```php
    if ($method === 'POST' && $path === '/api/board/camera-connections') {
        handle_camera_connections(request_json());
    }
    if ($method === 'GET' && $path === '/api/board/camera-connections') {
        json_response(['connections' => list_camera_connections($_GET)]);
    }
```

The public URLs are `http://192.168.1.42/trail_cam/api/board/camera-connections`.
The board routes don't use auth today, and these follow the same rule.

## 4. Request contract (what clients will send)

Only `camera`, `started_at` and `ok` are required for each row. Everything else
can be NULL or left out.

```json
{
  "scanner_host": "trail-pi",
  "connections": [
    {
      "camera": "e6_plus",
      "job_id": "job_20261115-070003_e6_plus",
      "started_at": "2026-11-15T07:00:03Z",
      "connected_at": "2026-11-15T07:00:38Z",
      "ended_at": "2026-11-15T07:02:12Z",
      "ok": true,
      "failure_stage": null,
      "wake_method": "bleak",
      "wake_attempts": 1,
      "ble_rssi_dbm": -78,
      "wifi_rssi_dbm": -49,
      "wifi_rssi_min": -58,
      "wifi_rssi_avg": -51.4,
      "wifi_rssi_max": -47,
      "wifi_freq_mhz": 2437,
      "wifi_tx_bitrate": "11.0 MBit/s",
      "wifi_rx_bitrate": "1.0 MBit/s",
      "ble_scan_ms": 4200,
      "ble_connect_ms": 1300,
      "hotspot_wait_ms": 18500,
      "wifi_join_ms": 6100,
      "total_connect_ms": 35000,
      "link_losses": 0,
      "items_downloaded": 46,
      "bytes_downloaded": 812345678,
      "avg_kbps": 1204.7,
      "details": {"ble_adapter": "hci1", "ble_ok_notify": true}
    }
  ]
}
```

Response:

```json
{"ok": true, "saved": 1, "skipped": 0, "ids": [1]}
```

Sending the same `(scanner_host, camera, started_at)` again updates that row and
doesn't create a duplicate. Fields sent as NULL keep whatever was stored before.

Timestamps without a timezone are interpreted in the server's PHP timezone, as
`sql_datetime()` already does for other tables. Clients should send UTC with `Z`.

## 5. Verify after deploying

```bash
# 1. PHP syntax
php -l /var/www/html/trail_cam/index.php

# 2. Insert a test row (creates the table on the first request)
curl -s -X POST http://192.168.1.42/trail_cam/api/board/camera-connections \
  -H 'Content-Type: application/json' \
  -d '{"scanner_host":"handoff-test","connections":[{"camera":"e6_plus","started_at":"2026-11-16T12:00:00Z","ok":false,"failure_stage":"hotspot_timeout","ble_rssi_dbm":-91}]}'

# 3. Re-send the same row with Wi-Fi data: expect the same id, still one row
curl -s -X POST http://192.168.1.42/trail_cam/api/board/camera-connections \
  -H 'Content-Type: application/json' \
  -d '{"scanner_host":"handoff-test","connections":[{"camera":"e6_plus","started_at":"2026-11-16T12:00:00Z","ok":true,"wifi_rssi_dbm":-55}]}'

# 4. Read it back: expect ok=1, wifi_rssi_dbm=-55, and ble_rssi_dbm still -91
curl -s 'http://192.168.1.42/trail_cam/api/board/camera-connections?scanner_host=handoff-test'

# 5. Check that existing routes still work
curl -s http://192.168.1.42/trail_cam/api/board/telemetry/latest | head -c 300

# 6. Delete the test row
mysql <db> -e "DELETE FROM camera_connections WHERE scanner_host='handoff-test';"
```

## 6. Useful queries once data is coming in

```sql
-- Success rate and average signal per camera, last 30 days
SELECT camera, COUNT(*) attempts, SUM(ok) successes,
       ROUND(AVG(ble_rssi_dbm),1) avg_ble, ROUND(AVG(wifi_rssi_dbm),1) avg_wifi
FROM camera_connections
WHERE started_at >= NOW() - INTERVAL 30 DAY
GROUP BY camera;

-- Where failures happen
SELECT camera, failure_stage, COUNT(*) n, ROUND(AVG(ble_rssi_dbm),1) avg_ble
FROM camera_connections WHERE ok = 0
GROUP BY camera, failure_stage ORDER BY n DESC;

-- Throughput by Wi-Fi signal bucket
SELECT FLOOR(wifi_rssi_avg/5)*5 AS rssi_bucket, COUNT(*) n, ROUND(AVG(avg_kbps),1) kbps
FROM camera_connections WHERE ok = 1 AND avg_kbps IS NOT NULL
GROUP BY rssi_bucket ORDER BY rssi_bucket DESC;
```

## 7. Optional later: web UI

Add a "Camera connections" panel to `app.js` that calls the GET route and shows
the recent attempts (time, ok/failure stage, BLE dBm, Wi-Fi dBm), plus a small
signal-over-time chart. Not needed for the table to be useful.

## Minimal version

For only the original ask, the minimal subset is these columns: `scanner_host`,
`camera`, `started_at`, `ok`, `ble_rssi_dbm`, `wifi_rssi_dbm`, `payload`. The
handler and routes stay the same: extra columns that clients don't send stay
NULL, so starting with the full table costs nothing.
