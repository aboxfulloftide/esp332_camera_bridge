/*
 * Broad I2C scanner - tries multiple GPIO pairs to find the camera sensor.
 * OV2640 SCCB address is 0x21 (7-bit) or 0x30 (write-only).
 */
#include <Wire.h>

// GPIOs to skip (strapping pins, USB, serial)
bool skip(int p) {
  return (p == 0 || p == 3 || p == 19 || p == 20 || p == 43 || p == 44);
}

void scanPair(int sda, int scl) {
  Wire.end();
  if (!Wire.begin(sda, scl, 100000)) return;
  Wire.beginTransmission(0x21);
  if (Wire.endTransmission() == 0) {
    Serial.printf("  OV2640 found! SDA=%d SCL=%d (addr 0x21)\n", sda, scl);
  }
  Wire.beginTransmission(0x30);
  if (Wire.endTransmission() == 0) {
    Serial.printf("  Device at 0x30! SDA=%d SCL=%d\n", sda, scl);
  }
  Wire.end();
}

void setup() {
  Serial.begin(921600);
  delay(1000);
  Serial.println("=== Scanning all GPIO pairs for camera ===");

  // First try the documented pins
  Serial.println("Trying documented pins: SDA=45 SCL=42");
  Wire.begin(45, 42, 100000);
  int found = 0;
  for (int addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("  Nothing.");
  Wire.end();

  // Broad scan of likely GPIO pairs
  Serial.println("Broad scan...");
  int gpios[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,
                 21,38,39,40,41,42,45,46,47,48};
  int n = sizeof(gpios)/sizeof(gpios[0]);
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      if (i == j) continue;
      int sda = gpios[i], scl = gpios[j];
      if (skip(sda) || skip(scl)) continue;
      Wire.end();
      Wire.begin(sda, scl, 100000);
      Wire.beginTransmission(0x21);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  OV2640(0x21) at SDA=%d SCL=%d\n", sda, scl);
      }
      Wire.beginTransmission(0x30);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  Sensor(0x30) at SDA=%d SCL=%d\n", sda, scl);
      }
    }
  }
  Wire.end();
  Serial.println("=== Scan complete ===");
}

void loop() {}
