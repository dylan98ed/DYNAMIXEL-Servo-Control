
#include <Arduino.h>
#include <Dynamixel2Arduino.h>

// =============================================
// ESP32-S3 + SP3485
// =============================================

#define PIN_TX   17
#define PIN_RX   18
#define PIN_DIR  16

HardwareSerial ServoSerial(1);
Dynamixel2Arduino dxl(ServoSerial, PIN_DIR);

// Velocidades de bus a comprobar
const uint32_t velocidades[] = {
  57600,
  115200,
  1000000,
  2000000,
  9600
};

const int NUM_VELOCIDADES =
  sizeof(velocidades) / sizeof(velocidades[0]);

bool encontrado = false;

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" ESCANER DE SERVOMOTORES DYNAMIXEL");
  Serial.println(" ESP32-S3 + RS485 SP3485");
  Serial.println("====================================");

  ServoSerial.begin(
    57600,
    SERIAL_8N1,
    PIN_RX,
    PIN_TX
  );

  dxl.setPortProtocolVersion(2.0);

  // Escanear todas las velocidades
  for (int i = 0; i < NUM_VELOCIDADES; i++) {

    uint32_t baud = velocidades[i];

    Serial.println();
    Serial.println("------------------------------------");
    Serial.printf("Probando velocidad: %lu bps\n",
                  (unsigned long)baud);

    dxl.begin(baud);

    delay(200);

    // Espacio para respuestas de hasta 16 motores
    DYNAMIXEL::InfoFromPing_t info[16];

    // PING broadcast (ID 254).
    // No cambia ningun parametro.
    uint8_t cantidad = dxl.ping(
      254,
      info,
      16
    );

    if (cantidad > 0) {

      encontrado = true;

      Serial.println();
      Serial.println("***** SERVOMOTOR DETECTADO *****");

      for (int j = 0; j < cantidad; j++) {

        Serial.printf("ID: %u\n", info[j].id);

        Serial.printf("Modelo: %u\n",
                      info[j].model_number);

        Serial.printf("Firmware: %u\n",
                      info[j].firmware_version);

        Serial.printf("Baudrate: %lu\n",
                      (unsigned long)baud);

        Serial.println("--------------------------------");
      }

    } else {
      Serial.println("Sin respuesta");
      Serial.printf("Error biblioteca: %u\n",
                    dxl.getLastLibErrCode());
    }
  }

  Serial.println();
  Serial.println("====================================");

  if (encontrado) {
    Serial.println("ESCANEO FINALIZADO: SERVO DETECTADO");
  } else {
    Serial.println("NO SE DETECTARON SERVOMOTORES");
    Serial.println("Revisar alimentacion y cableado RS485");
  }

  Serial.println("====================================");
}

void loop() {
  // No se realizan movimientos.
}
