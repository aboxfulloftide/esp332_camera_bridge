#include <Arduino.h>

void setup() {
  Serial0.begin(115200);
  Serial.begin(115200);
  delay(1500);
  Serial.println("serial_probe: Serial up");
  Serial0.println("serial_probe: Serial0 up");
}

void loop() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last >= 1000) {
    last = now;
    Serial.printf("serial_probe: tick %lu\n", now);
    Serial0.printf("serial_probe: tick %lu\n", now);
  }
}
