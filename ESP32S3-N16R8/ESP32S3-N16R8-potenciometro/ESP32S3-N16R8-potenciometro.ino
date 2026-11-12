
#include <Arduino.h>
#include <Dynamixel2Arduino.h>
#include <math.h>

// ======================================================
// CONFIGURACION ESP32-S3
// ======================================================

// Potenciometro
constexpr int PIN_POT = 4;

// Comunicacion RS485
constexpr int PIN_TX  = 17;
constexpr int PIN_RX  = 18;
constexpr int PIN_DIR = 16;

// Datos del servo (ajustar segun deteccion)
constexpr uint8_t DXL_ID = 6;
constexpr uint32_t DXL_BAUD = 1000000;

// Modelo H42-20-S300-R original
constexpr uint16_t MODEL_EXPECTED = 51200;

// ACTIVAR SOLAMENTE DESPUES DE VERIFICAR EL HARDWARE
constexpr bool ENABLE_MOTION = true;

// Limites angulares
constexpr float ANG_MIN = -50.0f;
constexpr float ANG_MAX =  50.0f;

// Calibracion ADC (ajustable)
constexpr int ADC_MIN = 0;
constexpr int ADC_MAX = 4095;

// Velocidad del servo: ~9,9 rpm de salida
constexpr int32_t GOAL_VELOCITY_RAW = 3000;

// Maximo cambio de consigna: 60 grados/segundo
constexpr float MAX_SLEW_DEG_S = 60.0f;

constexpr unsigned long LOOP_MS = 50;
constexpr unsigned long REPORT_MS = 300;
constexpr unsigned long READ_POS_MS = 500;

HardwareSerial ServoSerial(1);
Dynamixel2Arduino dxl(ServoSerial, PIN_DIR);

using namespace ControlTableItem;

// ======================================================
// VARIABLES
// ======================================================

float adcFiltrado = 0.0f;
float objetivoPot = 0.0f;
float consigna = 0.0f;
float posicionReal = 0.0f;

bool torqueActivo = false;

unsigned long ultimoLoop = 0;
unsigned long ultimoReporte = 0;
unsigned long ultimaLectura = 0;

float ultimaOrden = NAN;

// ======================================================
// FUNCIONES AUXILIARES
// ======================================================

float limitar(float x, float minimo, float maximo) {
  return fmaxf(minimo, fminf(maximo, x));
}

void detener(const char *mensaje) {
  Serial.println();
  Serial.println("================================");
  Serial.println("[ERROR] SISTEMA DETENIDO");
  Serial.println(mensaje);
  Serial.printf("Error biblioteca: %u\n",
                dxl.getLastLibErrCode());

  if (torqueActivo) {
    if (dxl.torqueOff(DXL_ID)) {
      Serial.println("Torque deshabilitado");
    } else {
      Serial.println("No se pudo confirmar Torque OFF");
    }
  }

  Serial.println("Revisar equipo antes de reiniciar");
  Serial.println("================================");

  while (true) {
    delay(1000);
  }
}

// Promedio de 16 lecturas del ADC
int leerADC() {
  uint32_t suma = 0;

  for (int i = 0; i < 16; i++) {
    suma += analogRead(PIN_POT);
    delayMicroseconds(200);
  }

  return suma / 16;
}

float convertirAGrados(float adc) {
  float normalizado =
      (adc - ADC_MIN) / float(ADC_MAX - ADC_MIN);

  normalizado = limitar(normalizado, 0.0f, 1.0f);

  return ANG_MIN +
         normalizado * (ANG_MAX - ANG_MIN);
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" ESP32-S3 + POTENCIOMETRO 100k");
  Serial.println(" DYNAMIXEL PRO H42-20-S300-R");
  Serial.println("====================================");

  // Configuracion ADC
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_POT, ADC_11db);

  adcFiltrado = leerADC();
  objetivoPot = convertirAGrados(adcFiltrado);

  Serial.println("[OK] ADC inicializado");
  Serial.printf("ADC inicial: %.0f\n", adcFiltrado);
  Serial.printf("Angulo inicial: %.2f grados\n",
                objetivoPot);

  if (!ENABLE_MOTION) {
    Serial.println();
    Serial.println("MODO DIAGNOSTICO DEL POTENCIOMETRO");
    Serial.println("Motor deshabilitado");
    return;
  }

  // Inicializacion UART1
  ServoSerial.begin(
    DXL_BAUD,
    SERIAL_8N1,
    PIN_RX,
    PIN_TX
  );

  dxl.begin(DXL_BAUD);
  dxl.setPortProtocolVersion(2.0);

  Serial.println("Buscando servo...");

  if (!dxl.ping(DXL_ID)) {
    detener("Servo sin respuesta al PING");
  }

  uint16_t modelo = dxl.getModelNumber(DXL_ID);

  Serial.printf("Servo detectado. Modelo: %u\n", modelo);

  if (modelo != MODEL_EXPECTED) {
    detener("Modelo de servo inesperado");
  }

  // Deshabilitar torque antes de configurar
  if (!dxl.torqueOff(DXL_ID)) {
    detener("No se pudo deshabilitar torque");
  }

  uint32_t modo = dxl.readControlTableItem(
    OPERATING_MODE, DXL_ID
  );

  if (dxl.getLastLibErrCode() != 0 || modo != 3) {
    detener("El servo no esta en modo posicion");
  }

  // Verificar limites configurados en el servo
  int32_t minimo = dxl.readControlTableItem(
    MIN_POSITION_LIMIT, DXL_ID
  );

  if (dxl.getLastLibErrCode() != 0) {
    detener("Error leyendo limite minimo");
  }

  int32_t maximo = dxl.readControlTableItem(
    MAX_POSITION_LIMIT, DXL_ID
  );

  if (dxl.getLastLibErrCode() != 0) {
    detener("Error leyendo limite maximo");
  }

  constexpr int32_t PULSOS_50 = 42188;

  if (minimo > -PULSOS_50 ||
      maximo < PULSOS_50) {
    detener("Limites del servo no permiten +/-50 grados");
  }

  // Velocidad limitada
  if (!dxl.writeControlTableItem(
        GOAL_VELOCITY,
        DXL_ID,
        GOAL_VELOCITY_RAW)) {
    detener("Error configurando velocidad");
  }

  // Leer posicion real antes de dar torque
  posicionReal = dxl.getPresentPosition(
    DXL_ID, UNIT_DEGREE
  );

  if (dxl.getLastLibErrCode() != 0 ||
      !isfinite(posicionReal)) {
    detener("No se pudo leer la posicion inicial");
  }

  Serial.printf("Posicion inicial: %.2f grados\n",
                posicionReal);

  // Mantener posicion actual al habilitar torque
  consigna = posicionReal;

  if (!dxl.setGoalPosition(
        DXL_ID, consigna, UNIT_DEGREE)) {
    detener("Error estableciendo objetivo inicial");
  }

  if (!dxl.torqueOn(DXL_ID)) {
    detener("No se pudo habilitar torque");
  }

  torqueActivo = true;
  ultimaOrden = consigna;

  Serial.println("[OK] Servo habilitado");
  Serial.println("Control por potenciometro iniciado");

  ultimoLoop = millis();
  ultimoReporte = millis();
  ultimaLectura = millis();
}

// ======================================================
// LOOP PRINCIPAL
// ======================================================

void loop() {

  unsigned long ahora = millis();

  if (ahora - ultimoLoop < LOOP_MS) {
    return;
  }

  float dt = (ahora - ultimoLoop) / 1000.0f;
  ultimoLoop = ahora;

  // Leer potenciometro
  int adc = leerADC();

  // Filtro exponencial
  adcFiltrado = 0.8f * adcFiltrado + 0.2f * adc;

  // Convertir lectura ADC a angulo
  objetivoPot = convertirAGrados(adcFiltrado);

  // Modo diagnostico: solo lectura analogica
  if (!ENABLE_MOTION) {
    if (ahora - ultimoReporte >= REPORT_MS) {
      ultimoReporte = ahora;

      Serial.printf(
        "ADC: %4d | Filtrado: %6.0f | "
        "Objetivo: %+6.2f grados\n",
        adc, adcFiltrado, objetivoPot
      );
    }

    return;
  }

  // Limitar velocidad de cambio de consigna
  float incrementoMax = MAX_SLEW_DEG_S * dt;

  float diferencia = objetivoPot - consigna;

  consigna += limitar(
    diferencia,
    -incrementoMax,
    incrementoMax
  );

  // Enviar solamente si el cambio es significativo
  if (isnan(ultimaOrden) ||
      fabsf(consigna - ultimaOrden) >= 0.5f) {

    if (!dxl.setGoalPosition(
          DXL_ID, consigna, UNIT_DEGREE)) {
      detener("Error enviando Goal Position");
    }

    ultimaOrden = consigna;
  }

  // Consultar posicion real periodicamente
  if (ahora - ultimaLectura >= READ_POS_MS) {
    ultimaLectura = ahora;

    posicionReal = dxl.getPresentPosition(
      DXL_ID, UNIT_DEGREE
    );

    if (dxl.getLastLibErrCode() != 0 ||
        !isfinite(posicionReal)) {
      detener("Error leyendo Present Position");
    }
  }

  // Terminal de diagnostico
  if (ahora - ultimoReporte >= REPORT_MS) {
    ultimoReporte = ahora;

    Serial.printf(
      "ADC:%4d | POT:%+6.1f | "
      "CMD:%+6.1f | REAL:%+6.1f\n",
      adc,
      objetivoPot,
      consigna,
      posicionReal
    );
  }
}
