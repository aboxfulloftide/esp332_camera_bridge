/*
 * HT-HC32/HT-HC33 battery probe.
 *
 * Datasheet/schematic:
 * - GPIO1  = ADC_IN, battery divider sense
 * - GPIO20 = ADC_Ctrl, battery divider enable/control
 * - GPIO15/GPIO16 are sampled as candidate charger status pins
 */
#include <Arduino.h>

static const int BAT_ADC_PIN = 1;
static const int BAT_ADC_CTRL_PIN = 20;
static const int CHRG_PIN = 15;
static const int DONE_PIN = 16;

static uint32_t readMilliVoltsAveraged(int pin, int samples = 64) {
  uint64_t total = 0;
  for (int i = 0; i < samples; ++i) {
    total += analogReadMilliVolts(pin);
    delay(5);
  }
  return total / samples;
}

static uint32_t readRawAveraged(int pin, int samples = 64) {
  uint64_t total = 0;
  for (int i = 0; i < samples; ++i) {
    total += analogRead(pin);
    delay(5);
  }
  return total / samples;
}

static void printReading(const char *label, int ctrlLevel) {
  digitalWrite(BAT_ADC_CTRL_PIN, ctrlLevel);
  delay(50);

  uint32_t raw = readRawAveraged(BAT_ADC_PIN);
  uint32_t mv = readMilliVoltsAveraged(BAT_ADC_PIN);
  float batteryV = (mv / 1000.0f) * 2.0f; // schematic shows 100K/100K divider

  Serial.printf("%s ctrl=%d adc_raw=%lu adc_mv=%lu battery_est_v=%.3f\n",
                label, ctrlLevel, (unsigned long)raw, (unsigned long)mv, batteryV);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("battery_probe: HT-HC32/HT-HC33");
  Serial.printf("GPIO1 ADC_IN, GPIO20 ADC_Ctrl, GPIO15/16 candidate charger status\n");

  pinMode(BAT_ADC_CTRL_PIN, OUTPUT);
  pinMode(CHRG_PIN, INPUT_PULLUP);
  pinMode(DONE_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
}

void loop() {
  printReading("BAT", LOW);
  printReading("BAT", HIGH);

  Serial.printf("status gpio15=%d gpio16=%d usb_mv=%lu chip_temp_c=%.1f uptime_ms=%lu\n",
                digitalRead(CHRG_PIN),
                digitalRead(DONE_PIN),
                (unsigned long)0,
                temperatureRead(),
                millis());
  Serial.println("---");
  delay(2000);
}
