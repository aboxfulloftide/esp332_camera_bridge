#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#define TAG ""
#else
#include "esp_log.h"
static const char *TAG = "camera_httpd";
#endif

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART        = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

// ── Moving-average filter for frame rate display ──────────────────────────────
typedef struct {
  size_t size, index, count;
  int sum;
  int *values;
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
  memset(filter, 0, sizeof(ra_filter_t));
  filter->values = (int *)malloc(sample_size * sizeof(int));
  if (!filter->values) return NULL;
  memset(filter->values, 0, sample_size * sizeof(int));
  filter->size = sample_size;
  return filter;
}

static int ra_filter_run(ra_filter_t *filter, int value) {
  if (!filter->values) return value;
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index = (filter->index + 1) % filter->size;
  if (filter->count < filter->size) filter->count++;
  return filter->sum / filter->count;
}

// ── MJPEG stream handler ──────────────────────────────────────────────────────
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb      = NULL;
  struct timeval _timestamp;
  esp_err_t res        = ESP_OK;
  size_t _jpg_buf_len  = 0;
  uint8_t *_jpg_buf    = NULL;
  char *part_buf[128];
  static int64_t last_frame = 0;
  if (!last_frame) last_frame = esp_timer_get_time();

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      ESP_LOGE(TAG, "Camera capture failed");
      res = ESP_FAIL;
    } else {
      _timestamp.tv_sec  = fb->timestamp.tv_sec;
      _timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool ok = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!ok) { ESP_LOGE(TAG, "JPEG conversion failed"); res = ESP_FAIL; }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf     = fb->buf;
      }
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART,
                             _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);

    if (fb) { esp_camera_fb_return(fb); fb = NULL; _jpg_buf = NULL; }
    else if (_jpg_buf) { free(_jpg_buf); _jpg_buf = NULL; }

    if (res != ESP_OK) break;

    int64_t fr_end = esp_timer_get_time();
    int64_t frame_time = (fr_end - last_frame) / 1000;
    last_frame = fr_end;
    ra_filter_run(&ra_filter, frame_time);
  }
  last_frame = 0;
  return res;
}

// ── Simple index page ─────────────────────────────────────────────────────────
static const char index_html[] = R"rawliteral(
<html><head><title>ESP32 Camera Stream</title></head>
<body style="font-family:sans-serif;text-align:center;background:#111;color:#eee">
  <h2>ESP32 Camera Stream</h2>
  <img id="stream" src="" style="max-width:100%;border:2px solid #444"/>
  <script>
    var base = document.location.hostname;
    document.getElementById('stream').src = 'http://' + base + ':81/stream';
  </script>
</body></html>)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, index_html, sizeof(index_html));
}

// ── Start both servers ────────────────────────────────────────────────────────
void startCameraServer() {
  ra_filter_init(&ra_filter, 20);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };
  httpd_uri_t stream_uri = {
    .uri     = "/stream",
    .method  = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  // Port 80: web page
  if (httpd_start(&camera_httpd, &config) == ESP_OK)
    httpd_register_uri_handler(camera_httpd, &index_uri);

  // Port 81: MJPEG stream
  config.server_port += 1;
  config.ctrl_port   += 1;
  if (httpd_start(&stream_httpd, &config) == ESP_OK)
    httpd_register_uri_handler(stream_httpd, &stream_uri);
}
