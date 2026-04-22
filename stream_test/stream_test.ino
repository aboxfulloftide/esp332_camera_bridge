/*
 * HT-HC33 MJPEG Stream Test
 * Uses HaLow (802.11ah) WiFi. Stream at http://<ip>:81/stream
 * or open http://<ip>/ in a browser.
 */
#include "esp_camera.h"
#include <HaLow.h>   // must be included to satisfy linker (lwip_mmnetif)
#include <WiFi.h>
#include <WiFiClient.h>

#if __has_include("local_config.h")
#include "local_config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "replace-with-your-ssid"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "replace-with-your-password"
#endif

void startCameraServer();

void setup() {
  Serial.begin(115200);
  delay(500);

  // Camera init
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
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.frame_size   = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size  = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed — halting");
    while (true) delay(1000);
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_saturation(s, 0);

  // Standard 2.4GHz WiFi
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.printf("Camera ready!\n  Page:   http://%s/\n  Stream: http://%s:81/stream\n",
                WiFi.localIP().toString().c_str(),
                WiFi.localIP().toString().c_str());
}

void loop() {
  delay(5000);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost");
  }
}
