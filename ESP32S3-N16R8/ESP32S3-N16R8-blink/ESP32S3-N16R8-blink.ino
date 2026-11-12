
#include <Arduino.h>

// LED RGB integrado de la ESP32-S3
#define LED_RGB 48

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Iniciando prueba Blink...");
}

void loop() {
  // Encender LED rojo
  rgbLedWrite(LED_RGB, 60, 0, 0);
  Serial.println("LED ENCENDIDO");
  delay(1000);

  // Apagar LED
  rgbLedWrite(LED_RGB, 0, 0, 0);
  Serial.println("LED APAGADO");
  delay(1000);
}
