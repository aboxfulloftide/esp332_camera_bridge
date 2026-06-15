/*
 * Temporary SD card serial transfer sketch for HT-HC33.
 *
 * Mounts the onboard microSD card on the same SPI pins as sd_status_logger and
 * streams every regular file over USB serial. It does not format, delete, or
 * write to the card.
 */
#include <Arduino.h>
#include <FS.h>
#include <halow_SD.h>
#include <SPI.h>

static const int SD_MOSI_PIN = 11;
static const int SD_CLK_PIN = 15;
static const int SD_MISO_PIN = 16;
static const int SD_CS_PIN = 10;
static const uint32_t SERIAL_BAUD = 921600;
static const uint8_t COPY_BUFFER_SIZE = 128;

static SPIClass sdSpi(HSPI);

static String escapePath(const String &path) {
  String out;
  out.reserve(path.length() + 8);
  for (size_t i = 0; i < path.length(); ++i) {
    const char c = path[i];
    if (c == '\\' || c == '\n' || c == '\r') {
      out += '\\';
      if (c == '\n') {
        out += 'n';
      } else if (c == '\r') {
        out += 'r';
      } else {
        out += '\\';
      }
    } else {
      out += c;
    }
  }
  return out;
}

static bool mountSdCard() {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SD_MISO_PIN, INPUT_PULLUP);
  sdSpi.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  const uint32_t frequencies[] = {4000000UL, 1000000UL, 400000UL};
  for (uint32_t frequency : frequencies) {
    Serial.printf("SD_MOUNT_TRY:%lu\n", static_cast<unsigned long>(frequency));
    if (SD.begin(SD_CS_PIN, sdSpi, frequency, "/sd", 5, false) && SD.cardType() != CARD_NONE) {
      Serial.printf("SD_MOUNT_OK:%lu:%llu:%llu\n",
                    static_cast<unsigned long>(frequency),
                    SD.cardSize(),
                    SD.usedBytes());
      return true;
    }
    SD.end();
    delay(100);
  }
  Serial.println("SD_MOUNT_FAIL");
  return false;
}

static void sendFile(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    Serial.printf("FILE_SKIP:%s\n", escapePath(path).c_str());
    if (file) {
      file.close();
    }
    return;
  }

  const uint64_t size = file.size();
  Serial.printf("FILE_BEGIN:%s:%llu\n", escapePath(path).c_str(), size);
  Serial.flush();

  uint8_t buffer[COPY_BUFFER_SIZE];
  uint64_t sent = 0;
  while (sent < size) {
    const size_t n = file.read(buffer, sizeof(buffer));
    if (n == 0) {
      break;
    }
    Serial.write(buffer, n);
    sent += n;
  }
  Serial.flush();
  file.close();
  Serial.printf("\nFILE_END:%s:%llu\n", escapePath(path).c_str(), sent);
}

static void walkDir(const String &path, uint32_t depth) {
  if (depth > 16) {
    Serial.printf("DIR_DEPTH_LIMIT:%s\n", escapePath(path).c_str());
    return;
  }

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("DIR_SKIP:%s\n", escapePath(path).c_str());
    if (dir) {
      dir.close();
    }
    return;
  }

  File entry = dir.openNextFile();
  while (entry) {
    String entryPath = entry.path();
    if (entry.isDirectory()) {
      Serial.printf("DIR:%s\n", escapePath(entryPath).c_str());
      entry.close();
      walkDir(entryPath, depth + 1);
    } else {
      entry.close();
      sendFile(entryPath);
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1500);
  Serial.println("SD_TRANSFER_READY");
  if (mountSdCard()) {
    walkDir("/", 0);
  }
  Serial.println("SD_TRANSFER_DONE");
}

void loop() {
  delay(1000);
}
