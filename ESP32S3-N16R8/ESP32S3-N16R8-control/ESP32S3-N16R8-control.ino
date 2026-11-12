#include <Arduino.h>
#include <Dynamixel2Arduino.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ===============================================================
// TP2: Control supervisor + telemetria DYNAMIXEL H42-20-S300-R
// ESP32-S3 N16R8 + transceptor SP3485 (3.3V, half-duplex)
// NOTA: firmware ORIGINAL, modelo 51200 (NO version H42-R(A))
// ===============================================================

// Ajustar el ID y baudrate al servo identificado previamente.
constexpr uint8_t  DXL_ID   = 6;
constexpr uint32_t DXL_BAUD = 1000000;
constexpr uint16_t MODELO_ESPERADO = 51200;

// Puerto USB-C COM: monitor Arduino con USB CDC On Boot = Disabled
constexpr uint32_t BAUD_PC = 115200;
constexpr int PIN_TX = 17;  // DI del SP3485
constexpr int PIN_RX = 18;  // RO del SP3485
constexpr int PIN_DIR = 16; // RSE (DE + /RE)
constexpr int PIN_POT = 4;  // cursor del potenciometro (0..3.3 V)

// Limites ABSOLUTOS del banco, adicionales a los del servo.
// Modificarlos solo tras verificar el recorrido libre de la articulacion.
constexpr float BANCO_MIN_GRADOS = -180.0f;
constexpr float BANCO_MAX_GRADOS =  180.0f;
constexpr float BANCO_MAX_RPM = 20.0f;
constexpr int32_t BANCO_MAX_ACC_RAW = 25;

constexpr float PULSOS_REV = 303750.0f;
constexpr float RPM_POR_RAW = 0.00329218f;
constexpr float MA_POR_RAW = 4.02832f;
constexpr uint32_t POT_PERIODO_MS = 50;
constexpr float POT_RAMPA_GRADOS_S = 30.0f;

// Opciones de visualizacion por defecto (ademas del comando STREAM).
constexpr bool MOSTRAR_CONSIGNA = true;
constexpr bool MOSTRAR_TELEMETRIA = true;

HardwareSerial BusServo(1);
Dynamixel2Arduino dxl(BusServo, PIN_DIR);
using namespace ControlTableItem;

enum class Modo : uint8_t { PC, POT };
Modo modo = Modo::PC;

bool conectado = false;
bool armado = false;
bool falloBloqueante = false;
bool streaming = true;
uint32_t periodoReporte = 500;
uint32_t ultimoReporte = 0;
uint32_t ultimoPot = 0;
uint8_t fallosLectura = 0;

float limiteMin = BANCO_MIN_GRADOS;
float limiteMax = BANCO_MAX_GRADOS;
float hwMin = -180.0f;
float hwMax = 180.0f;
int32_t hwLimiteVelRaw = 0;
int32_t hwLimiteAccRaw = 0;
float velObjetivoRpm = 8.0f;
int32_t accObjetivoRaw = 5;
float ultimaConsigna = NAN;
float consignaPot = 0.0f;
float adcFiltrado = 0.0f;
int adcActual = 0;

char linea[96];
uint8_t longitudLinea = 0;

struct Lectura {
  int32_t raw;
  bool ok;
};
struct Estado {
  Lectura posicion, velocidad, corriente, tension, temperatura;
  Lectura moving, torque, errorHW;
  bool ok;
};

float limitarF(float x, float minV, float maxV) {
  return fmaxf(minV, fminf(maxV, x));
}
float rawGrados(int32_t raw) {
  return (float)raw * 360.0f / PULSOS_REV;
}
Lectura leer(uint8_t item) {
  int32_t n = dxl.readControlTableItem(item, DXL_ID);
  return {n, dxl.getLastLibErrCode() == 0};
}
bool escribir(uint8_t item, int32_t valor) {
  if (!dxl.writeControlTableItem(item, DXL_ID, valor)) {
    Serial.printf("[ERROR] Escritura item=%u, lib=%u, status=0x%02X\n",
       item, dxl.getLastLibErrCode(), dxl.getLastStatusPacketError());
    return false;
  }
  return true;
}

void fallo(const char* razon) {
  if (falloBloqueante) return;
  falloBloqueante = true;
  Serial.printf("[FALLO] %s. Se bloquean movimientos.\n", razon);
  if (conectado && armado) {
    if (dxl.torqueOff(DXL_ID)) {
      Serial.println("[FALLO] Se ordeno TORQUE OFF.");
    } else {
      Serial.println("[FALLO] No se pudo confirmar TORQUE OFF: cortar potencia de forma segura.");
    }
  }
  armado = false;
}

bool leerPosicion(float& grados) {
  Lectura p = leer(PRESENT_POSITION);
  if (!p.ok) return false;
  grados = rawGrados(p.raw);
  return isfinite(grados);
}

Estado leerEstado() {
  Estado e;
  e.posicion    = leer(PRESENT_POSITION);
  e.velocidad   = leer(PRESENT_VELOCITY);
  e.corriente   = leer(PRESENT_CURRENT);
  e.tension     = leer(PRESENT_INPUT_VOLTAGE);
  e.temperatura = leer(PRESENT_TEMPERATURE);
  e.moving      = leer(MOVING);
  e.torque      = leer(TORQUE_ENABLE);
  e.errorHW     = leer(HARDWARE_ERROR_STATUS);
  e.ok = e.posicion.ok && e.velocidad.ok && e.corriente.ok &&
         e.tension.ok && e.temperatura.ok && e.moving.ok &&
         e.torque.ok && e.errorHW.ok;
  return e;
}

void mostrarEstado(const Estado& e) {
  if (!e.ok) {
    Serial.printf("t=%lu | TELEMETRIA INCOMPLETA (lib=%u)\n",
       (unsigned long)millis(), dxl.getLastLibErrCode());
    return;
  }
  Serial.printf("t=%lu", (unsigned long)millis());
  if (MOSTRAR_CONSIGNA) {
    Serial.printf(" | MODE=%s | MIN=%.1f | MAX=%.1f | OBJ=%.2f",
      modo == Modo::PC ? "PC" : "POT", limiteMin, limiteMax,
      isnan(ultimaConsigna) ? 0.0f : ultimaConsigna);
    if (modo == Modo::POT) Serial.printf(" | ADC=%d", adcActual);
  }
  if (MOSTRAR_TELEMETRIA) {
    Serial.printf(" | POS=%.2fdeg | VEL=%.2frpm | I=%.1fmA | VIN=%.1fV"
                  " | T=%ldC | MOV=%ld | TORQ=%ld | HW=0x%02lX",
      rawGrados(e.posicion.raw), e.velocidad.raw * RPM_POR_RAW,
      (float)((int16_t)e.corriente.raw) * MA_POR_RAW,
      e.tension.raw * 0.1f,
      (long)e.temperatura.raw,
      (long)e.moving.raw,
      (long)e.torque.raw,
      (unsigned long)(e.errorHW.raw & 0xFF));
  }
  Serial.println();
}

bool conectar() {
  conectado = false;
  armado = false;
  Serial.printf("[BUS] PING ID=%u baud=%lu...\n",
      DXL_ID, (unsigned long)DXL_BAUD);
  if (!dxl.ping(DXL_ID)) {
    Serial.printf("[BUS] Sin respuesta, lib=%u. Revisar ID, baud, A/B, RSE y GND.\n",
                  dxl.getLastLibErrCode());
    return false;
  }
  uint16_t modelo = dxl.getModelNumber(DXL_ID);
  if (dxl.getLastLibErrCode() != 0 || modelo != MODELO_ESPERADO) {
    Serial.printf("[BUS] Modelo %u (esperado %u). No se habilita control.\n",
                  modelo, MODELO_ESPERADO);
    return false;
  }
  Lectura op = leer(OPERATING_MODE);
  Lectura srl = leer(STATUS_RETURN_LEVEL);
  Lectura lmin = leer(MIN_POSITION_LIMIT);
  Lectura lmax = leer(MAX_POSITION_LIMIT);
  Lectura lv = leer(VELOCITY_LIMIT);
  Lectura la = leer(ACCELERATION_LIMIT);
  Lectura hw = leer(HARDWARE_ERROR_STATUS);
  Lectura torquePrevio = leer(TORQUE_ENABLE);
  if (!op.ok || !srl.ok || !lmin.ok || !lmax.ok ||
      !lv.ok || !la.ok || !hw.ok || !torquePrevio.ok) {
    Serial.println("[BUS] Fallo leyendo configuracion interna.");
    return false;
  }
  if (op.raw != 3) {
    Serial.printf("[BUS] Operating Mode=%ld; se requiere 3 (Position).\n", (long)op.raw);
    return false;
  }
  if (srl.raw != 2) {
    Serial.printf("[BUS] Status Return Level=%ld; se requiere 2.\n", (long)srl.raw);
    return false;
  }
  if (hw.raw != 0) {
    Serial.printf("[BUS] Hardware Error Status=0x%02lX: revisar servo.\n",
                  (unsigned long)(hw.raw & 0xFF));
    return false;
  }
  hwMin = rawGrados(lmin.raw);
  hwMax = rawGrados(lmax.raw);
  hwLimiteVelRaw = lv.raw;
  hwLimiteAccRaw = la.raw;
  limiteMin = fmaxf(BANCO_MIN_GRADOS, hwMin);
  limiteMax = fminf(BANCO_MAX_GRADOS, hwMax);
  if (limiteMin >= limiteMax || hwLimiteVelRaw <= 0 || hwLimiteAccRaw <= 0) {
    Serial.println("[BUS] Limites internos incompatibles con la practica.");
    return false;
  }
  conectado = true;
  falloBloqueante = false;
  fallosLectura = 0;
  Serial.printf("[OK] Modelo=%u | EEPROM limites=[%.2f, %.2f]deg"
                " | limites efectivos=[%.2f, %.2f]deg\n",
         modelo, hwMin, hwMax, limiteMin, limiteMax);
  if (torquePrevio.raw != 0) {
    Serial.println("[ATENCION] El servo ya tenia TORQUE ON antes del programa; verificar mecanismo.");
  }
  Serial.println("[LISTO] Control de movimiento desarmado. Usar ARM para comenzar.");
  return true;
}

bool mandarPosicion(float solicitada, bool explicar = true) {
  if (!conectado || falloBloqueante || !armado) {
    if (explicar) Serial.println("[ERROR] Servo no armado. Usar ARM.");
    return false;
  }
  float aplicada = limitarF(solicitada, limiteMin, limiteMax);
  if (!dxl.setGoalPosition(DXL_ID, aplicada, UNIT_DEGREE)) {
    Serial.printf("[ERROR] Goal Position, lib=%u, status=0x%02X\n",
      dxl.getLastLibErrCode(), dxl.getLastStatusPacketError());
    fallo("No se pudo escribir Goal Position");
    return false;
  }
  ultimaConsigna = aplicada;
  if (explicar) {
    Serial.printf("[POS] Pedido=%.2fdeg | Aplicado=%.2fdeg | limites=[%.1f,%.1f]deg%s\n",
      solicitada, aplicada, limiteMin, limiteMax,
      fabsf(solicitada - aplicada) > 0.01f ? " | SATURADO" : "");
  }
  return true;
}

bool configurarVelocidad(float rpm) {
  if (!conectado || falloBloqueante) return false;
  if (!isfinite(rpm) || rpm < 0.5f || rpm > BANCO_MAX_RPM) {
    Serial.printf("[ERROR] VEL admite 0.5..%.1f rpm.\n", BANCO_MAX_RPM);
    return false;
  }
  int32_t raw = (int32_t)lroundf(rpm / RPM_POR_RAW);
  if (raw <= 0 || raw > hwLimiteVelRaw) {
    Serial.println("[ERROR] Velocidad excede limite EEPROM del servo.");
    return false;
  }
  if (!escribir(GOAL_VELOCITY, raw)) return false;
  Lectura verif = leer(GOAL_VELOCITY);
  if (!verif.ok || verif.raw != raw) return false;
  velObjetivoRpm = raw * RPM_POR_RAW;
  Serial.printf("[VEL] %.2f rpm (raw=%ld)\n", velObjetivoRpm, (long)raw);
  return true;
}

bool configurarAceleracion(int32_t raw) {
  if (!conectado || falloBloqueante) return false;
  if (raw < 1 || raw > BANCO_MAX_ACC_RAW || raw > hwLimiteAccRaw) {
    Serial.printf("[ERROR] ACC RAW: 1..%ld (o menor segun EEPROM).\n",
      (long)BANCO_MAX_ACC_RAW);
    return false;
  }
  if (!escribir(GOAL_ACCELERATION, raw)) return false;
  Lectura verif = leer(GOAL_ACCELERATION);
  if (!verif.ok || verif.raw != raw) return false;
  accObjetivoRaw = raw;
  Serial.printf("[ACC] raw=%ld | 1 raw = 58000 rpm^2 del motor interno\n", (long)raw);
  return true;
}

void armar() {
  if (!conectado || falloBloqueante) {
    Serial.println("[ERROR] Sin conexion valida o fallo bloqueante. Revisar y reiniciar.");
    return;
  }
  if (armado) {
    Serial.println("[ARM] Ya estaba habilitado.");
    return;
  }
  float posicion;
  if (!leerPosicion(posicion)) { fallo("No se pudo leer posicion para ARM"); return; }
  if (!configurarVelocidad(velObjetivoRpm)) { fallo("Fallo configurando VEL"); return; }
  if (!configurarAceleracion(accObjetivoRaw)) { fallo("Fallo configurando ACC"); return; }
  // Evita salto a una consigna anterior al habilitar torque.
  if (!dxl.setGoalPosition(DXL_ID, posicion, UNIT_DEGREE)) {
    fallo("No se pudo fijar posicion actual antes del ARM"); return;
  }
  if (!dxl.torqueOn(DXL_ID)) { fallo("TORQUE ON sin respuesta"); return; }
  armado = true;
  ultimaConsigna = posicion;
  consignaPot = posicion;
  Serial.printf("[ARM] Torque habilitado. Manteniendo %.2fdeg.\n", posicion);
}

void desarmar() {
  if (!conectado) { Serial.println("[ERROR] Sin conexion."); return; }
  if (!dxl.torqueOff(DXL_ID)) { fallo("TORQUE OFF sin confirmacion"); return; }
  armado = false;
  Serial.println("[OFF] Torque deshabilitado (eje sin retencion). ");
}

void parar() {
  if (!armado) { Serial.println("[STOP] Servo no armado."); return; }
  float posicion;
  if (!leerPosicion(posicion)) { fallo("STOP: sin posicion real"); return; }
  // Se usa posicion real para evitar desplazamiento adicional por saturacion.
  if (!dxl.setGoalPosition(DXL_ID, posicion, UNIT_DEGREE)) {
    fallo("STOP: no se pudo fijar posicion"); return;
  }
  ultimaConsigna = posicion;
  consignaPot = posicion;
  modo = Modo::PC;
  Serial.printf("[STOP] Consigna mantenida en %.2fdeg (NO es parada de emergencia).\n",
                posicion);
}

void imprimirAyuda() {
  Serial.println("\n=== COMANDOS (una linea y ENTER) ===");
  Serial.println("PING             : identificar/recuperar conexion (sin movimiento)");
  Serial.println("ARM              : fijar posicion actual y habilitar torque");
  Serial.println("OFF              : torque off (el eje queda libre)");
  Serial.println("MODE PC          : control por comandos USB");
  Serial.println("MODE POT         : seguimiento por potenciometro GPIO4");
  Serial.println("POS 40           : mover a 40deg (respeta MIN/MAX)");
  Serial.println("MIN -30          : nuevo limite software inferior");
  Serial.println("MAX 45           : nuevo limite software superior");
  Serial.println("VEL 5            : limite de velocidad, rpm de salida");
  Serial.println("ACC 5            : aceleracion RAW (1..25)");
  Serial.println("STOP             : mantener posicion actual (no emergencia)");
  Serial.println("STATUS           : leer y mostrar telemetria inmediatamente");
  Serial.println("INFO             : mostrar limites y registros de configuracion");
  Serial.println("STREAM ON/OFF    : activar/desactivar filas periodicas");
  Serial.println("RATE 200         : periodo de telemetria en ms (100..5000)");
  Serial.println("HELP             : mostrar comandos\n");
}

void informacion() {
  Serial.printf("[INFO] ID=%u | baud=%lu | modo=%s | torque=%s | streaming=%s/%lums\n",
    DXL_ID, (unsigned long)DXL_BAUD, modo == Modo::PC ? "PC" : "POT",
    armado ? "ON" : "OFF", streaming ? "ON" : "OFF",
    (unsigned long)periodoReporte);
  if (conectado) {
    Serial.printf("[INFO] Limites SOFT=[%.1f,%.1f]deg | EEPROM=[%.1f,%.1f]deg\n",
      limiteMin, limiteMax, hwMin, hwMax);
  } else {
    Serial.printf("[INFO] Limites SOFT=[%.1f,%.1f]deg | servo no verificado\n",
      limiteMin, limiteMax);
  }
  Serial.printf("[INFO] VEL=%.2frpm | ACC=%ld raw | HW VEL limite=%.2frpm | ACC limite=%ld raw\n",
    velObjetivoRpm, (long)accObjetivoRaw,
    hwLimiteVelRaw * RPM_POR_RAW, (long)hwLimiteAccRaw);
  if (conectado) {
    Lectura gp = leer(GOAL_POSITION);
    Lectura gv = leer(GOAL_VELOCITY);
    Lectura ga = leer(GOAL_ACCELERATION);
    if (gp.ok && gv.ok && ga.ok) {
      Serial.printf("[INFO] Registros servo: GoalPos=%.2fdeg (596), GoalVel=%.2frpm (600), GoalAcc=%ldraw (606)\n",
        rawGrados(gp.raw), gv.raw * RPM_POR_RAW, (long)ga.raw);
    } else {
      Serial.println("[INFO] No se pudieron leer todos los Goal registrados.");
    }
  }
}

bool parseFloat(const char* s, float& v) {
  if (!s) return false;
  char* end = nullptr;
  v = strtof(s, &end);
  return end != s && *end == '\0' && isfinite(v);
}
bool parseInt(const char* s, long& v) {
  if (!s) return false;
  char* end = nullptr;
  v = strtol(s, &end, 10);
  return end != s && *end == '\0';
}

void procesarLinea(char* texto) {
  char* save = nullptr;
  char* cmd = strtok_r(texto, " \t", &save);
  char* arg = strtok_r(nullptr, " \t", &save);
  char* extra = strtok_r(nullptr, " \t", &save);
  if (!cmd) return;
  for (char* p=cmd; *p; ++p) *p = toupper((unsigned char)*p);
  if (arg) for (char* p=arg; *p; ++p) *p = toupper((unsigned char)*p);
  if (extra) { Serial.println("[ERROR] Demasiados argumentos."); return; }

  if (!strcmp(cmd, "HELP")) { imprimirAyuda(); return; }
  if (!strcmp(cmd, "PING")) {
    if (armado || falloBloqueante) {
      Serial.println("[ERROR] No se permite PING de reconexion durante movimiento o fallo bloqueante.");
    } else {
      conectar();
    }
    return;
  }
  if (!strcmp(cmd, "INFO")) { informacion(); return; }
  if (!strcmp(cmd, "STATUS")) {
    if (!conectado) { Serial.println("[ERROR] Sin comunicacion. Enviar PING."); return; }
    Estado e = leerEstado(); mostrarEstado(e); return;
  }
  if (!strcmp(cmd, "STREAM")) {
    if (!arg || (strcmp(arg,"ON") && strcmp(arg,"OFF"))) {
      Serial.println("Uso: STREAM ON o STREAM OFF"); return;
    }
    streaming = !strcmp(arg,"ON");
    Serial.printf("[STREAM] %s\n", streaming ? "ON" : "OFF"); return;
  }
  if (!strcmp(cmd, "RATE")) {
    long ms;
    if (!parseInt(arg, ms) || ms < 100 || ms > 5000) {
      Serial.println("Uso: RATE 100..5000 (ms)"); return;
    }
    periodoReporte = (uint32_t)ms;
    Serial.printf("[RATE] %ldms\n", ms); return;
  }
  if (!strcmp(cmd, "MODE")) {
    if (!arg) { Serial.println("Uso: MODE PC o MODE POT"); return; }
    if (!strcmp(arg,"PC")) { modo=Modo::PC; Serial.println("[MODE] PC"); return; }
    if (!strcmp(arg,"POT")) {
      modo=Modo::POT;
      float actual;
      if (conectado && leerPosicion(actual)) consignaPot = actual;
      else consignaPot = isnan(ultimaConsigna) ? 0.0f : ultimaConsigna;
      adcFiltrado = analogRead(PIN_POT);
      Serial.println("[MODE] POT: rampa limitada (30deg/s)"); return;
    }
    Serial.println("Uso: MODE PC o MODE POT"); return;
  }
  if (!strcmp(cmd, "ARM")) { armar(); return; }
  if (!strcmp(cmd, "OFF")) { desarmar(); return; }
  if (!strcmp(cmd, "STOP")) { parar(); return; }

  float x;
  if (!strcmp(cmd, "POS")) {
    if (!parseFloat(arg, x)) { Serial.println("Uso: POS <grados>"); return; }
    if (modo != Modo::PC) { Serial.println("[ERROR] Primero: MODE PC"); return; }
    mandarPosicion(x); return;
  }
  if (!strcmp(cmd, "MIN") || !strcmp(cmd, "MAX")) {
    if (!parseFloat(arg, x) || x < BANCO_MIN_GRADOS || x > BANCO_MAX_GRADOS) {
      Serial.printf("Uso: %s <grados entre %.1f y %.1f>\n",
        cmd, BANCO_MIN_GRADOS, BANCO_MAX_GRADOS); return;
    }
    if (!strcmp(cmd, "MIN")) {
      if (x >= limiteMax || (conectado && x < hwMin)) {
        Serial.println("[ERROR] MIN debe ser menor que MAX y estar dentro de limite EEPROM."); return;
      }
      limiteMin = x;
    } else {
      if (x <= limiteMin || (conectado && x > hwMax)) {
        Serial.println("[ERROR] MAX debe ser mayor que MIN y estar dentro de limite EEPROM."); return;
      }
      limiteMax = x;
    }
    Serial.printf("[LIMITES] Software: [%.1f, %.1f]deg (no se modifica EEPROM)\n",
       limiteMin, limiteMax);
    return;
  }
  if (!strcmp(cmd, "VEL")) {
    if (!parseFloat(arg, x)) { Serial.println("Uso: VEL <rpm>"); return; }
    if (!conectado) { Serial.println("[ERROR] Sin servo conectado."); return; }
    if (!configurarVelocidad(x)) Serial.println("[ERROR] No se confirmo VEL.");
    return;
  }
  if (!strcmp(cmd, "ACC")) {
    long n;
    if (!parseInt(arg, n)) { Serial.println("Uso: ACC <entero RAW 1..25>"); return; }
    if (!conectado) { Serial.println("[ERROR] Sin servo conectado."); return; }
    if (!configurarAceleracion((int32_t)n)) Serial.println("[ERROR] No se confirmo ACC.");
    return;
  }
  Serial.println("[ERROR] Comando desconocido. Enviar HELP.");
}

void leerComandos() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (longitudLinea) {
        linea[longitudLinea] = '\0';
        procesarLinea(linea);
        longitudLinea = 0;
      }
    } else if (c >= 32 && c <= 126) {
      if (longitudLinea < sizeof(linea) - 1) linea[longitudLinea++] = c;
      else { longitudLinea = 0; Serial.println("[ERROR] Linea demasiado larga."); }
    }
  }
}

void actualizarPotenciometro(uint32_t ahora) {
  if (modo != Modo::POT || !armado || falloBloqueante) return;
  if (ahora - ultimoPot < POT_PERIODO_MS) return;
  float dt = ultimoPot ? (ahora - ultimoPot) / 1000.0f : 0.05f;
  ultimoPot = ahora;
  adcActual = analogRead(PIN_POT);
  adcFiltrado = 0.80f * adcFiltrado + 0.20f * adcActual;
  float x = limiteMin + (adcFiltrado/4095.0f) * (limiteMax-limiteMin);
  consignaPot += limitarF(x-consignaPot,
                 -POT_RAMPA_GRADOS_S * dt, POT_RAMPA_GRADOS_S * dt);
  if (isnan(ultimaConsigna) || fabsf(consignaPot-ultimaConsigna) >= 0.6f) {
    mandarPosicion(consignaPot, false);
  }
}

void actualizarTelemetria(uint32_t ahora) {
  if (!conectado || ahora - ultimoReporte < periodoReporte) return;
  ultimoReporte = ahora;
  Estado e = leerEstado();
  if (streaming) mostrarEstado(e);
  if (!e.ok) {
    if (++fallosLectura >= 3) fallo("Tres lecturas de telemetria incompletas");
    return;
  }
  fallosLectura = 0;
  if (e.errorHW.raw != 0) fallo("El servo informa Hardware Error Status distinto de cero");
  if (armado && e.torque.raw == 0) fallo("El torque se deshabilito inesperadamente");
}

void setup() {
  Serial.begin(BAUD_PC);
  delay(1500);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_POT, ADC_11db);
  adcActual = analogRead(PIN_POT);
  adcFiltrado = adcActual;

  BusServo.begin(DXL_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  dxl.begin(DXL_BAUD);
  dxl.setPortProtocolVersion(2.0);

  Serial.println("\n=== TP2 | ESP32-S3 + DYNAMIXEL H42 ORIGINAL ===");
  Serial.println("PC: 115200 baud, monitor serie, fin de linea: Nueva linea");
  Serial.println("Modo inicial PC; no se envia movimiento al arrancar.");
  Serial.println("IMPORTANTE: pruebas solo con eje descargado y parada de potencia disponible.");
  conectar();
  imprimirAyuda();
  ultimoReporte = millis();
  ultimoPot = millis();
}

void loop() {
  leerComandos();
  uint32_t ahora = millis();
  actualizarPotenciometro(ahora);
  actualizarTelemetria(ahora);
}
