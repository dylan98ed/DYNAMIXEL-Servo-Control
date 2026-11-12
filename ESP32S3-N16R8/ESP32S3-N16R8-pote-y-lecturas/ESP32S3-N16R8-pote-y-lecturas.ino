
#include <Arduino.h>
#include <Dynamixel2Arduino.h>
#include <math.h>

// =====================================================
// FLAGS DE VISUALIZACION
// =====================================================

// Mostrar lectura del potenciometro y consigna
constexpr bool MOSTRAR_POTENCIOMETRO = false;

// Mostrar parametros recibidos desde el servo
constexpr bool MOSTRAR_SERVO = true;

// Controlar el movimiento fisico del servomotor
constexpr bool ENABLE_MOTION = true;

// =====================================================
// CONFIGURACION DEL HARDWARE
// =====================================================

// Potenciometro
constexpr int PIN_POT = 4;

// RS485: ESP32-S3 -> SP3485
constexpr int PIN_TX  = 17;
constexpr int PIN_RX  = 18;
constexpr int PIN_DIR = 16;

// DYNAMIXEL PRO H42-20-S300-R original
constexpr uint8_t DXL_ID = 6;
constexpr uint32_t DXL_BAUD = 1000000;
constexpr uint16_t MODEL_EXPECTED = 51200;

// Limites angulares
constexpr float ANG_MIN = -50.0f;
constexpr float ANG_MAX =  50.0f;

// Calibracion analogica
constexpr int ADC_MIN = 0;
constexpr int ADC_MAX = 4095;

// Conversiones del H42 original
constexpr float PULSOS_REV = 303750.0f;
constexpr float RPM_POR_UNIDAD = 0.00329218f;
constexpr float MA_POR_UNIDAD = 4.02832f;

// Limite de velocidad del servo
constexpr int32_t VELOCIDAD_RAW = 3000;

// Limitacion de consigna
constexpr float MAX_SLEW_DEG_S = 60.0f;
constexpr float PASO_MINIMO_DEG = 0.5f;

// Periodos
constexpr uint32_t CONTROL_MS = 50;
constexpr uint32_t REPORTE_MS = 1000;

// Maximo de ciclos de telemetria fallidos consecutivos
constexpr int MAX_FALLOS = 3;

// =====================================================
// OBJETOS Y VARIABLES
// =====================================================

HardwareSerial ServoSerial(1);
Dynamixel2Arduino dxl(ServoSerial, PIN_DIR);

using namespace ControlTableItem;

float adcFiltrado = 0.0f;
float objetivoPot = 0.0f;
float consigna = 0.0f;
float ultimaOrden = NAN;

int adcActual = 0;

bool torqueActivo = false;

uint32_t ultimoControl = 0;
uint32_t ultimoReporte = 0;

int fallosConsecutivos = 0;

// Estructura para distinguir una lectura valida de error
struct Campo {
  int32_t valor = 0;
  bool valido = false;
};

// Lecturas de telemetria
struct Telemetria {
  Campo posicion;
  Campo velocidad;
  Campo corriente;
  Campo voltaje;
  Campo temperatura;
  Campo moving;
  Campo torque;
  Campo hardwareError;

  Campo goalPosition;
  Campo goalVelocity;
  Campo goalTorque;
  Campo goalAcceleration;

  Campo external1;
  Campo external2;
  Campo external3;
  Campo external4;

  int fallos = 0;
};

// =====================================================
// FUNCIONES AUXILIARES
// =====================================================

float limitar(float valor, float minimo, float maximo) {
  return fmaxf(minimo, fminf(maximo, valor));
}

float pulsosAGrados(int32_t pulsos) {
  return pulsos * 360.0f / PULSOS_REV;
}

float rawARpm(int32_t raw) {
  return raw * RPM_POR_UNIDAD;
}

void detener(const char* motivo) {
  Serial.println();
  Serial.println("[ERROR] SISTEMA DETENIDO");
  Serial.println(motivo);

  Serial.printf("Error biblioteca: %u\n",
                dxl.getLastLibErrCode());

  if (torqueActivo) {
    if (dxl.torqueOff(DXL_ID)) {
      Serial.println("Torque deshabilitado");
    } else {
      Serial.println("No se pudo confirmar Torque OFF");
    }
  }

  while (true) {
    delay(1000);
  }
}

// =====================================================
// LECTURA DEL POTENCIOMETRO
// =====================================================

int leerADC() {
  uint32_t suma = 0;

  for (int i = 0; i < 16; i++) {
    suma += analogRead(PIN_POT);
    delayMicroseconds(200);
  }

  return suma / 16;
}

float adcAGrados(float adc) {
  float porcentaje =
      (adc - ADC_MIN) / float(ADC_MAX - ADC_MIN);

  porcentaje = limitar(porcentaje, 0.0f, 1.0f);

  return ANG_MIN +
         porcentaje * (ANG_MAX - ANG_MIN);
}

// =====================================================
// LECTURA SEGURA DE UN REGISTRO DYNAMIXEL
// =====================================================

Campo leerCampo(uint8_t registro) {
  Campo dato;

  dato.valor = dxl.readControlTableItem(
    registro,
    DXL_ID,
    100
  );

  dato.valido = (dxl.getLastLibErrCode() == 0);

  return dato;
}

// =====================================================
// TELEMETRIA DEL SERVOMOTOR
// =====================================================

Telemetria leerTelemetria() {
  Telemetria t;

  t.posicion = leerCampo(PRESENT_POSITION);
  t.velocidad = leerCampo(PRESENT_VELOCITY);
  t.corriente = leerCampo(PRESENT_CURRENT);
  t.voltaje = leerCampo(PRESENT_INPUT_VOLTAGE);
  t.temperatura = leerCampo(PRESENT_TEMPERATURE);

  t.moving = leerCampo(MOVING);
  t.torque = leerCampo(TORQUE_ENABLE);
  t.hardwareError = leerCampo(HARDWARE_ERROR_STATUS);

  t.goalPosition = leerCampo(GOAL_POSITION);
  t.goalVelocity = leerCampo(GOAL_VELOCITY);
  t.goalTorque = leerCampo(GOAL_TORQUE);
  t.goalAcceleration = leerCampo(GOAL_ACCELERATION);

  t.external1 = leerCampo(EXTERNAL_PORT_DATA_1);
  t.external2 = leerCampo(EXTERNAL_PORT_DATA_2);
  t.external3 = leerCampo(EXTERNAL_PORT_DATA_3);
  t.external4 = leerCampo(EXTERNAL_PORT_DATA_4);

  const Campo* campos[] = {
    &t.posicion,
    &t.velocidad,
    &t.corriente,
    &t.voltaje,
    &t.temperatura,
    &t.moving,
    &t.torque,
    &t.hardwareError,
    &t.goalPosition,
    &t.goalVelocity,
    &t.goalTorque,
    &t.goalAcceleration,
    &t.external1,
    &t.external2,
    &t.external3,
    &t.external4
  };

  for (const Campo* campo : campos) {
    if (!campo->valido) {
      t.fallos++;
    }
  }

  return t;
}

// =====================================================
// IMPRESION DE VALORES
// =====================================================

void imprimirEntero(const char* etiqueta, Campo c) {
  Serial.print(etiqueta);

  if (c.valido) {
    Serial.print(c.valor);
  } else {
    Serial.print("ERR");
  }
}

void imprimirDecimal(const char* etiqueta,
                     Campo c,
                     float escala,
                     int decimales = 2) {
  Serial.print(etiqueta);

  if (c.valido) {
    Serial.print(c.valor * escala, decimales);
  } else {
    Serial.print("ERR");
  }
}

void imprimirTelemetria(const Telemetria& t) {

  imprimirDecimal(
    "POS=", t.posicion, 360.0f / PULSOS_REV, 2
  );

  Serial.print("deg | ");

  imprimirDecimal(
    "VEL=", t.velocidad, RPM_POR_UNIDAD, 2
  );

  Serial.print("rpm | ");

  // Corriente: registro de 16 bits con signo
  Campo corriente = t.corriente;
  corriente.valor = static_cast<int16_t>(
    corriente.valor & 0xFFFF
  );

  imprimirDecimal(
    "I=", corriente, MA_POR_UNIDAD, 1
  );

  Serial.print("mA | ");

  imprimirDecimal(
    "V=", t.voltaje, 0.1f, 1
  );

  Serial.print("V | ");

  imprimirEntero("T=", t.temperatura);
  Serial.print("C | ");

  imprimirEntero("MOV=", t.moving);
  Serial.print(" | ");

  imprimirEntero("TORQ=", t.torque);
  Serial.print(" | ");

  Serial.print("HW=");

  if (t.hardwareError.valido) {
    Serial.print("0x");
    if ((t.hardwareError.valor & 0xFF) < 16) {
      Serial.print("0");
    }
    Serial.print(t.hardwareError.valor & 0xFF, HEX);
  } else {
    Serial.print("ERR");
  }

  Serial.print(" | ");

  imprimirDecimal(
    "GOAL=", t.goalPosition,
    360.0f / PULSOS_REV, 2
  );

  Serial.print("deg | ");

  imprimirDecimal(
    "GVEL=", t.goalVelocity,
    RPM_POR_UNIDAD, 2
  );

  Serial.print("rpm | ");

  imprimirEntero("GTQ=", t.goalTorque);
  Serial.print(" | ");

  imprimirEntero("GACC=", t.goalAcceleration);
  Serial.print(" | EXT=");

  if (t.external1.valido) {
    Serial.print(t.external1.valor);
  } else {
    Serial.print("ERR");
  }

  Serial.print(",");

  if (t.external2.valido) {
    Serial.print(t.external2.valor);
  } else {
    Serial.print("ERR");
  }

  Serial.print(",");

  if (t.external3.valido) {
    Serial.print(t.external3.valor);
  } else {
    Serial.print("ERR");
  }

  Serial.print(",");

  if (t.external4.valido) {
    Serial.print(t.external4.valor);
  } else {
    Serial.print("ERR");
  }

  Serial.print(" | FALLOS=");
  Serial.print(t.fallos);
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" CONTROL POR POTENCIOMETRO + TELEMETRIA");
  Serial.println(" ESP32-S3 / DYNAMIXEL H42-20-S300-R");
  Serial.println("======================================");

  Serial.printf("Mostrar POT: %s\n",
    MOSTRAR_POTENCIOMETRO ? "SI" : "NO");

  Serial.printf("Mostrar SERVO: %s\n",
    MOSTRAR_SERVO ? "SI" : "NO");

  Serial.printf("Movimiento: %s\n",
    ENABLE_MOTION ? "HABILITADO" : "DESHABILITADO");

  // Configuracion ADC
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_POT, ADC_11db);

  adcActual = leerADC();
  adcFiltrado = adcActual;
  objetivoPot = adcAGrados(adcFiltrado);

  // Configuracion UART1 / RS485
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
    detener("Servo no responde al PING");
  }

  uint16_t modelo = dxl.getModelNumber(DXL_ID);

  Serial.printf("Modelo detectado: %u\n", modelo);

  if (modelo != MODEL_EXPECTED) {
    detener("Modelo incorrecto o version no compatible");
  }

  if (ENABLE_MOTION) {

    // No cambiar EEPROM innecesariamente
    int32_t modo = dxl.readControlTableItem(
      OPERATING_MODE, DXL_ID
    );

    if (dxl.getLastLibErrCode() != 0 || modo != 3) {
      detener("Servo no esta en modo posicion");
    }

    // Verificar limites antes de mover
    int32_t limiteMin = dxl.readControlTableItem(
      MIN_POSITION_LIMIT, DXL_ID
    );

    if (dxl.getLastLibErrCode() != 0) {
      detener("No se pudo leer limite minimo");
    }

    int32_t limiteMax = dxl.readControlTableItem(
      MAX_POSITION_LIMIT, DXL_ID
    );

    if (dxl.getLastLibErrCode() != 0) {
      detener("No se pudo leer limite maximo");
    }

    const int32_t PULSOS_50 = 42188;

    if (limiteMin > -PULSOS_50 ||
        limiteMax < PULSOS_50) {
      detener("Limites no permiten +/-50 grados");
    }

    // Deshabilitar torque para configurar
    if (!dxl.torqueOff(DXL_ID)) {
      detener("No se pudo deshabilitar torque");
    }

    if (!dxl.writeControlTableItem(
          GOAL_VELOCITY,
          DXL_ID,
          VELOCIDAD_RAW)) {
      detener("No se pudo configurar velocidad");
    }

    // Leer posicion actual antes de habilitar torque
    float posicionInicial = dxl.getPresentPosition(
      DXL_ID, UNIT_DEGREE
    );

    if (dxl.getLastLibErrCode() != 0 ||
        !isfinite(posicionInicial)) {
      detener("No se pudo leer posicion inicial");
    }

    consigna = posicionInicial;

    if (!dxl.setGoalPosition(
          DXL_ID, consigna, UNIT_DEGREE)) {
      detener("Error fijando posicion inicial");
    }

    if (!dxl.torqueOn(DXL_ID)) {
      detener("Error habilitando torque");
    }

    torqueActivo = true;
    ultimaOrden = consigna;

    Serial.println("[OK] Torque habilitado");
  }

  ultimoControl = millis();
  ultimoReporte = millis();

  Serial.println("[OK] Sistema inicializado");
  Serial.println("======================================");
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  uint32_t ahora = millis();

  // -------------------------------------------------
  // CONTROL POR POTENCIOMETRO
  // -------------------------------------------------

  if (ahora - ultimoControl >= CONTROL_MS) {

    float dt = (ahora - ultimoControl) / 1000.0f;
    ultimoControl = ahora;

    adcActual = leerADC();

    // Filtro exponencial
    adcFiltrado =
      0.8f * adcFiltrado + 0.2f * adcActual;

    objetivoPot = adcAGrados(adcFiltrado);

    if (ENABLE_MOTION) {

      // Limitar rapidez de cambio de la consigna
      float incrementoMax = MAX_SLEW_DEG_S * dt;
      float diferencia = objetivoPot - consigna;

      consigna += limitar(
        diferencia,
        -incrementoMax,
        incrementoMax
      );

      // No enviar comandos ante variaciones minimas
      if (isnan(ultimaOrden) ||
          fabsf(consigna - ultimaOrden) >=
          PASO_MINIMO_DEG) {

        if (!dxl.setGoalPosition(
              DXL_ID,
              consigna,
              UNIT_DEGREE)) {

          detener("Error enviando Goal Position");
        }

        ultimaOrden = consigna;
      }
    }
  }

  // -------------------------------------------------
  // IMPRESION POR TERMINAL
  // -------------------------------------------------

  if (ahora - ultimoReporte >= REPORTE_MS) {

    ultimoReporte = ahora;

    if (!MOSTRAR_POTENCIOMETRO && !MOSTRAR_SERVO) {
      return;
    }

    // Una fila por ciclo de reporte
    bool hayColumnaAnterior = false;

    if (MOSTRAR_POTENCIOMETRO) {

      Serial.printf(
        "ADC=%4d | POT=%+6.2fdeg | CMD=%+6.2fdeg",
        adcActual,
        objetivoPot,
        ENABLE_MOTION ? consigna : objetivoPot
      );

      hayColumnaAnterior = true;
    }

    if (MOSTRAR_SERVO) {

      Telemetria t = leerTelemetria();

      if (hayColumnaAnterior) {
        Serial.print(" | ");
      }

      imprimirTelemetria(t);

      // Control de errores de lectura
      if (t.fallos > 0) {
        fallosConsecutivos++;
      } else {
        fallosConsecutivos = 0;
      }

      // Comprobar errores graves del hardware
      if (t.hardwareError.valido &&
          t.hardwareError.valor != 0) {

        Serial.println();
        detener("El servo reporta Hardware Error Status");
      }

      if (fallosConsecutivos >= MAX_FALLOS) {
        Serial.println();
        detener("Fallos consecutivos de telemetria");
      }
    }

    Serial.println();
  }
}
