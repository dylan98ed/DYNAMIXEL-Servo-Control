
#include <Arduino.h>
#include <Dynamixel2Arduino.h>
#include <math.h>

// ===================================================
// CONFIGURACION ESP32-S3 + SP3485
// ===================================================

#define PIN_TX   17
#define PIN_RX   18
#define PIN_DIR  16

#define DXL_ID   6
#define DXL_BAUD 1000000

// H42-20-S300-R original
#define MODEL_EXPECTED 51200

// Control de seguridad:
// false: solo diagnostico, sin movimiento
// true: ejecuta la secuencia automatica
const bool ENABLE_MOTION = true;

// Tiempos y velocidad
const unsigned long ESPERA_MS = 1000;
const int32_t VELOCIDAD_RAW = 6000; // ~19,75 rpm

HardwareSerial ServoSerial(1);
Dynamixel2Arduino dxl(ServoSerial, PIN_DIR);

using namespace ControlTableItem;

// ===================================================
// DIAGNOSTICO DE ERRORES
// ===================================================

void mostrarError(const char* operacion) {
  Serial.print("[ERROR] ");
  Serial.println(operacion);

  Serial.print("Codigo biblioteca: ");
  Serial.println(dxl.getLastLibErrCode());

  Serial.print("Error del servo: ");
  Serial.println(dxl.getLastStatusPacketError());
}

void detenerSistema(const char* motivo) {
  Serial.println();
  Serial.println("================================");
  Serial.println("SISTEMA DETENIDO");
  Serial.println(motivo);
  Serial.println("================================");

  if (dxl.torqueOff(DXL_ID)) {
    Serial.println("Torque deshabilitado");
  } else {
    Serial.println("No se pudo confirmar Torque OFF");
  }

  while (true) {
    delay(1000);
  }
}

// ===================================================
// CONSULTAR POSICION
// ===================================================

bool leerPosicion(float &posicion) {
  posicion = dxl.getPresentPosition(
    DXL_ID, UNIT_DEGREE
  );

  return dxl.getLastLibErrCode() == 0;
}

// ===================================================
// MOVER SERVOMOTOR
// ===================================================

void moverServo(float grados) {

  Serial.println();
  Serial.println("--------------------------------");
  Serial.printf("Objetivo: %.2f grados\n", grados);

  if (!dxl.setGoalPosition(
        DXL_ID, grados, UNIT_DEGREE)) {

    mostrarError("No se pudo enviar Goal Position");
    detenerSistema("Error de movimiento");
  }

  Serial.println("[OK] Orden enviada");

  // Esperar 1 segundo despues de enviar la orden
  delay(ESPERA_MS);

  float posicionReal;

  if (!leerPosicion(posicionReal)) {
    mostrarError("Lectura de posicion");
    detenerSistema("Error de comunicacion");
  }

  Serial.printf("Posicion real: %.2f grados\n",
                posicionReal);

  float error = grados - posicionReal;

  Serial.printf("Error de posicion: %.2f grados\n",
                error);

  if (fabs(error) > 3.0f) {
    Serial.println(
      "[AVISO] El servo aun no llego al objetivo"
    );
  }

  Serial.println("--------------------------------");
}

// ===================================================
// SETUP
// ===================================================

void setup() {

  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" BRAZO ROBOTICO - ESP32-S3");
  Serial.println(" DYNAMIXEL PRO H42-20-S300-R");
  Serial.println("================================");

  // Configurar pines UART1
  ServoSerial.begin(
    DXL_BAUD,
    SERIAL_8N1,
    PIN_RX,
    PIN_TX
  );

  // Biblioteca DYNAMIXEL
  dxl.begin(DXL_BAUD);
  dxl.setPortProtocolVersion(2.0);

  Serial.println("[1] RS485 inicializado");
  Serial.printf("Baudrate: %d\n", DXL_BAUD);
  Serial.printf("Servo ID: %d\n", DXL_ID);

  delay(1000);

  // -----------------------------------------------
  // DETECTAR SERVOMOTOR
  // -----------------------------------------------

  Serial.println("[2] Buscando servomotor...");

  if (!dxl.ping(DXL_ID)) {
    mostrarError("PING sin respuesta");
    detenerSistema("Servo no detectado");
  }

  Serial.println("[OK] Servo detectado");

  uint16_t modelo = dxl.getModelNumber(DXL_ID);

  Serial.printf("Numero de modelo: %u\n", modelo);

  if (modelo != MODEL_EXPECTED) {
    detenerSistema(
      "Modelo distinto del H42-20-S300-R original"
    );
  }

  // -----------------------------------------------
  // COMPROBAR MODO POSICION
  // -----------------------------------------------

  Serial.println("[3] Verificando modo posicion...");

  int32_t modo = dxl.readControlTableItem(
    OPERATING_MODE, DXL_ID
  );

  if (dxl.getLastLibErrCode() != 0) {
    detenerSistema("No se pudo leer Operating Mode");
  }

  if (modo != 3) {
    Serial.printf("Modo actual: %ld\n", (long)modo);
    detenerSistema("El servo no esta en modo posicion");
  }

  Serial.println("[OK] Modo posicion");

  // -----------------------------------------------
  // MODO DIAGNOSTICO
  // -----------------------------------------------

  if (!ENABLE_MOTION) {
    Serial.println();
    Serial.println("MODO DIAGNOSTICO");
    Serial.println("Movimiento deshabilitado");
    Serial.println("Comunicacion correcta");
    Serial.println("Cambiar ENABLE_MOTION a true");
    return;
  }

  // -----------------------------------------------
  // CONFIGURAR VELOCIDAD
  // -----------------------------------------------

  Serial.println("[4] Configurando velocidad...");

  if (!dxl.writeControlTableItem(
        GOAL_VELOCITY,
        DXL_ID,
        VELOCIDAD_RAW)) {

    mostrarError("Goal Velocity");
    detenerSistema("Error configurando velocidad");
  }

  Serial.println("[OK] Velocidad configurada");

  // -----------------------------------------------
  // HABILITAR TORQUE
  // -----------------------------------------------

  Serial.println("[5] Habilitando torque...");

  if (!dxl.torqueOn(DXL_ID)) {
    mostrarError("Torque ON");
    detenerSistema("No se pudo habilitar torque");
  }

  Serial.println("[OK] Torque habilitado");

  // -----------------------------------------------
  // MOVIMIENTO INICIAL
  // -----------------------------------------------

  Serial.println("[6] Posicion inicial");

  moverServo(0.0f);

  Serial.println();
  Serial.println("INICIANDO LOOP AUTOMATICO");
}

// ===================================================
// LOOP
// ===================================================

void loop() {

  if (!ENABLE_MOTION) {
    delay(1000);
    return;
  }

  moverServo(50.0f);

  moverServo(-50.0f);
}
