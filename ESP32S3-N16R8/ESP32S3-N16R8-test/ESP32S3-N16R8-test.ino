
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("=== ESP32-S3 N16R8 ===");

  Serial.printf("Flash: %u MB\n",
    ESP.getFlashChipSize() / 1048576);

  Serial.printf("PSRAM: %u MB\n",
    ESP.getPsramSize() / 1048576);

  Serial.printf("CPU: %u MHz\n",
    ESP.getCpuFreqMHz());

  if (psramFound()) {
    Serial.println("PSRAM OK");
  } else {
    Serial.println("ERROR: PSRAM no detectada");
  }
}

void loop() {
  Serial.println("ESP32 funcionando");
  delay(1000);
}
