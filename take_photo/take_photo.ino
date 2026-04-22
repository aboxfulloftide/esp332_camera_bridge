/*
 * HT-HC32 Camera Test
 * Captures a JPEG photo and sends it over USB serial.
 * Protocol: "JPEG_START:<length>\n" + raw bytes + "JPEG_END\n"
 */
#include "esp_camera.h"

// Pin definitions come from the board variant (HT-HC33/pins_arduino.h)

void setup() {
  Serial.begin(921600);
  delay(500);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  bool hasPsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 1000000;
  Serial.printf("PSRAM: %s  SPIRAM free: %u\n", hasPsram ? "yes" : "no", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (hasPsram) {
    config.frame_size   = FRAMESIZE_UXGA;  // 1600x1200
    config.jpeg_quality = 4;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;  // 800x600 fits in DRAM
    config.jpeg_quality = 4;               // still max JPEG quality
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("CAM_ERR:0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_saturation(s, 0);
  // Discard first 3 frames so AEC/AWB can stabilise
  for (int i = 0; i < 3; i++) {
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup) esp_camera_fb_return(warmup);
    delay(150);
  }

  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  Serial.printf("CAM_SENSOR:%s PID:0x%04X\n", info ? info->name : "unknown", s->id.PID);
  Serial.println("CAM_READY");
}

void loop() {
  // Take a photo each time 'c' is received, or automatically on first loop
  static bool first = true;
  char cmd = 0;
  if (Serial.available()) cmd = Serial.read();

  if (cmd == 'i') {
    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    Serial.printf("CAM_SENSOR:%s PID:0x%04X\n", info ? info->name : "unknown", s->id.PID);
    Serial.printf("PSRAM:%s free:%u\n", psramFound() ? "yes" : "no", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    delay(100);
    return;
  }

  if (first || cmd == 'c') {
    first = false;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("CAM_CAPTURE_FAIL");
      delay(1000);
      return;
    }
    Serial.printf("JPEG_START:%u\n", fb->len);
    Serial.write(fb->buf, fb->len);
    Serial.println("JPEG_END");
    esp_camera_fb_return(fb);
  }
  delay(100);
}
