# Control y telemetría DYNAMIXEL H42 con ESP32-S3

Proyecto para identificar, controlar y supervisar un servomotor **DYNAMIXEL H42-20-S300-R** mediante una **ESP32-S3 N16R8** y un transceptor **SP3485**. Incluye los programas de la ESP32 y una interfaz web local desarrollada en Python con Flask para enviar comandos, visualizar telemetría y exportar mediciones.

El flujo principal de trabajo es **escanear el bus → configurar y cargar el programa de control → operar desde el Monitor Serie o desde el panel de Python**.

```text
PC (Monitor Serie o panel Flask)
              ↕ USB-C COM · 115200 baudios
        ESP32-S3 N16R8
              ↕ UART1 + SP3485 · RS-485 · DYNAMIXEL Protocol 2.0
       DYNAMIXEL H42-20-S300-R
```

La ESP32 realiza la comunicación con el servomotor y ejecuta el control supervisor. El panel de Python se comunica con la ESP32 por USB; no accede directamente al bus RS-485. El programa de control también se puede utilizar sin el panel, enviando comandos desde el Monitor Serie.

## 1. Programas de la ESP32

Los sketches están en [`ESP32S3-N16R8/`](ESP32S3-N16R8/). Cada subcarpeta contiene un programa independiente: se carga uno por vez en la placa. Los dos principales son **escaner** y **control**.

### Hardware y preparación

- ESP32-S3 N16R8 y cable USB de datos conectado al puerto USB-C **COM**.
- Transceptor SP3485 de 3,3 V para el enlace RS-485 half-duplex.
- Servomotor DYNAMIXEL con su alimentación y cableado correspondientes.
- Arduino IDE con soporte para ESP32 y la biblioteca **Dynamixel2Arduino** instalada.
- Para el modo de control por potenciómetro: cursor conectado a GPIO4, con señal de 0 a 3,3 V.

Los sketches principales utilizan estas conexiones:

| Señal ESP32-S3 | Conexión |
|---|---|
| GPIO17 / TX | DI del SP3485 |
| GPIO18 / RX | RO del SP3485 |
| GPIO16 / dirección | DE y /RE unidos del SP3485 (RSE) |
| GND | Referencia de masa común del montaje |
| A / B del SP3485 | Bus RS-485 del servomotor |
| GPIO4 | Cursor del potenciómetro, si se utiliza `MODE POT` |

Para el programa de control, el código indica usar **USB CDC On Boot = Disabled** y el conector **COM**. El Monitor Serie debe configurarse a **115200 baudios** y enviar los comandos con fin de línea **Nueva línea**.

### Escáner: identificar el servomotor

Archivo: [`ESP32S3-N16R8-escaner.ino`](ESP32S3-N16R8/ESP32S3-N16R8-escaner/ESP32S3-N16R8-escaner.ino).

Este programa permite descubrir los datos de comunicación antes de configurar el control:

- Utiliza DYNAMIXEL Protocol 2.0 y envía un `PING` broadcast (ID 254).
- Prueba **57600, 115200, 1000000, 2000000 y 9600 baudios**, en ese orden.
- Reserva espacio para hasta 16 respuestas por velocidad.
- Muestra **ID, número de modelo, versión de firmware y baudrate** de los dispositivos detectados.
- No modifica parámetros ni envía órdenes de movimiento.

Cargá el sketch, abrí el Monitor Serie a 115200 baudios y reiniciá la placa para ver el escaneo completo. El recorrido se ejecuta una vez al arrancar; para repetirlo, reiniciá la ESP32. Anotá el ID, el modelo y el baudrate detectados para preparar el siguiente programa.

### Control: movimiento y telemetría

Archivo: [`ESP32S3-N16R8-control.ino`](ESP32S3-N16R8/ESP32S3-N16R8-control/ESP32S3-N16R8-control.ino).

Es el programa principal para operar el **H42-20-S300-R original, modelo 51200**; no está preparado para la variante H42-R(A). Antes de cargarlo, ajustá `DXL_ID` y `DXL_BAUD` según el resultado del escáner. Los valores incluidos son:

| Parámetro | Valor en el código |
|---|---|
| ID del servo (`DXL_ID`) | 6 |
| Baudrate RS-485 (`DXL_BAUD`) | 1000000 |
| Modelo esperado (`MODELO_ESPERADO`) | 51200 |
| Comunicación con la PC (`BAUD_PC`) | 115200 |
| Límites del banco | −180° a +180°, restringidos además por la EEPROM del servo |
| Velocidad admitida | 0,5 a 20 rpm, sujeta al límite EEPROM |
| Aceleración admitida | 1 a 25 RAW, sujeta al límite EEPROM |
| Telemetría periódica inicial | Cada 500 ms |

Al conectarse, comprueba el modelo, el modo de posición (`Operating Mode = 3`), el nivel de respuesta (`Status Return Level = 2`), los límites internos y la ausencia de errores de hardware. Si la configuración no es compatible, no habilita el control; estas comprobaciones no reconfiguran automáticamente la EEPROM.

Arranca en **modo PC y con el control desarmado**, sin enviar movimiento. Esto no garantiza que el torque previo del servo está apagado: si ya estaba activo, el programa informa esa condición. El comando `ARM` fija la consigna en la posición actual y habilita el torque.

- **Modo PC (`MODE PC`)**: recibe consignas de posición y ajustes por USB desde el Monitor Serie o el panel.
- **Modo potenciómetro (`MODE POT`)**: utiliza GPIO4 para recorrer los límites configurados, con filtrado y una rampa de consigna de 30°/s. También requiere `ARM`.
- **Telemetría**: informa posición, velocidad, corriente, tensión, temperatura, movimiento, torque y errores, junto con el modo, los límites y la consigna.
- **Supervisión**: bloquea movimientos ante fallos detectados y, si el control está armado y conectado, intenta deshabilitar el torque.

`HELP` muestra los comandos disponibles. `INFO` consulta configuración y límites; `STATUS` solicita una lectura inmediata; `STREAM ON/OFF` controla la salida periódica y `RATE N` fija su período entre 100 y 5000 ms. La secuencia de ensayo y el uso del panel se detallan más abajo.

### Puesta en marcha

1. Preparar el montaje y cargar el **escáner** para identificar el servo.
2. Ajustar `DXL_ID` y `DXL_BAUD` en **control** y verificar que el modelo corresponda al esperado.
3. Revisar los límites del banco para el recorrido mecánico disponible y cargar el sketch de **control**, reemplazando el escáner.
4. Abrir el Monitor Serie a 115200 baudios y consultar `INFO` y `STATUS` para comprobar la conexión.
5. Continuar desde el Monitor Serie o cerrarlo y abrir el panel de Python. Ambos usan el mismo puerto COM y no pueden mantenerlo abierto simultáneamente.

### Otros sketches incluidos

La carpeta de la ESP32 también conserva programas auxiliares de pruebas y desarrollo: `ESP32S3-N16R8-blink`, `ESP32S3-N16R8-test`, `ESP32S3-N16R8-potenciometro`, `ESP32S3-N16R8-pote-y-lecturas` y `ESP32S3-N16R8-servo-industrial`. Son alternativas independientes; no es necesario cargarlas para utilizar el recorrido escáner → control → panel. Revisá sus ajustes y comportamiento antes de ejecutarlas, ya que no todas comparten el arranque desarmado del programa principal.

## 2. Interfaz gráfica en Python (Flask)

Aplicación web local para visualizar la telemetría del sketch **ESP32S3-N16R8-control** y enviarle comandos por USB. Incluye gráficos, terminal de respuestas y exportación CSV.

### Requisitos

- Windows 10/11 y Python 3.10 o superior.
- ESP32-S3 conectada al conector USB-C **COM** y con el sketch `ESP32S3-N16R8-control.ino` cargado.
- El sketch debe imprimir telemetría en líneas, por ejemplo:

```text
t=10420 | MODE=PC | MIN=-30.0 | MAX=40.0 | OBJ=40.00 | POS=32.15deg | VEL=8.72rpm | I=210.3mA | VIN=24.1V | T=32C | MOV=1 | TORQ=1 | HW=0x00
```

- El firmware acepta `POS`, `MIN`, `MAX`, `VEL`, `ACC`, `ARM`, `OFF`, `STOP`, `INFO`, `PING`, `STATUS`, `MODE PC`, `MODE POT`, `STREAM ON/OFF`, `RATE N` y `HELP`.

### Instalación y ejecución (Windows)

1. Clonar este repositorio o extraer el ZIP.
2. Abrir **PowerShell** en esta carpeta.
3. Instalar las dependencias:

```powershell
py -m pip install -r requirements.txt
```

4. **Cerrar el Monitor Serie de Arduino IDE**; Flask necesita tener el control exclusivo del mismo puerto COM.
5. Ejecutar:

```powershell
py app.py
```

6. Abrir: http://127.0.0.1:5000
7. Seleccionar el puerto `COM6` o el que corresponda, y presionar **Conectar**.
8. Enviar `INFO` para consultar el estado o `STREAM ON` si el sketch tiene el flujo apagado.

Para detener Flask, presionar `Ctrl+C` en PowerShell.

Si Windows no reconoce `py`, reemplazarlo por `python`.

### Uso en el laboratorio

El programa empieza sin mover el eje. En el panel se puede ejecutar:

1. `PING` para comprobar comunicación.
2. `INFO` y `STATUS` para leer configuración y telemetría.
3. `MODE PC` para usar el control desde la PC.
4. `VEL 5` y `ACC 5` para fijar parámetros lentos antes del movimiento.
5. `ARM` **solo después de verificar que el eje está libre, el equipo está sujeto y existe parada física de potencia**.
6. `POS 10` → `POS -10` para un ensayo inicial pequeño.
7. `MIN -30`, `MAX 40` y `POS 60` para estudiar la saturación de consignas.
8. `STOP` para mantener la posición actual o `OFF` para liberar el eje (no usar con cargas suspendidas).

La terminal inferior muestra las respuestas de la ESP32. La gráfica superior compara **posición real** (POS) y **consigna** (OBJ); la inferior muestra **velocidad** (VEL) y **corriente** (I). El botón **Exportar CSV** descarga las muestras acumuladas desde que se conectó el puerto.

### Diferencia entre puertos y baudios

- PC ↔ ESP32 por USB-C COM: **115200 baudios**, configurados en esta aplicación.
- ESP32 ↔ H42 por UART1/SP3485/RS-485: **1000000 baudios en el sketch de control incluido**, ajustables mediante `DXL_BAUD`. Flask no la modifica.

La aplicación lee el puerto COM como un programa serie más. No puede leer “lo que aparece” en otra ventana del Monitor Serie si Arduino tiene bloqueado el COM.

### Consideraciones técnicas

- Una conexión USB serie **no demuestra** que el servo esté conectado. El panel lo considera con telemetría reciente si detecta una línea `t=...` válida en los últimos cuatro segundos.
- Los valores mostrados no son simulados. Si el motor no envía telemetría, aparecen `—` y **Sin telemetría reciente**.
- Si `MOSTRAR_TELEMETRIA` o `MOSTRAR_CONSIGNA` están deshabilitados en el sketch, esos campos no estarán disponibles en la interfaz.
- `MIN`/`MAX` son límites supervisores del sketch. El código incluido define `BANCO_MIN_GRADOS=-180` y `BANCO_MAX_GRADOS=180`; el rango efectivo se intersecta con los límites EEPROM del servo. **Flask no puede ampliar los límites definidos en el sketch**. También deben respetarse los límites EEPROM y mecánicos reales del motor.
- El panel limita el formato de los comandos, pero la **validación de seguridad final** corresponde al firmware del servo y al montaje mecánico.
- `STOP` no es un paro de emergencia; `OFF` libera la retención del eje.
- El servidor Flask se ejecuta por defecto **solo en esta PC** (`127.0.0.1`) y sin modo de depuración. No requiere conexión a Internet para usarse una vez instaladas las dependencias.
- Los gráficos utilizan HTML Canvas y JavaScript propio (sin servicios externos).

### Resolución de problemas

| Síntoma | Posible causa y solución |
|---|---|
| `Could not open port COM6` / `PermissionError` | Cerrar Monitor Serie, Plotter Serie u otro programa que utilice COM6. |
| Aparece conectado pero no hay gráficos | Mandar `STREAM ON`, revisar PING, mantener `MOSTRAR_TELEMETRIA=true` en el sketch. |
| `POS 180` queda limitado | Revisar constantes `BANCO_*`, comandos `MIN/MAX` y límites internos que devuelve `INFO`. |
| No aparece ningún puerto | Revisar cable USB de datos, drivers del conversor USB-UART y conexión en Windows. |
| Datos corruptos o caracteres ilegibles | Comprobar 115200 baudios de USB, no confundir con baudrate del servo. |
| No llegan comandos | Verificar que la ESP tenga cargado el sketch `ESP32S3-N16R8-control.ino` y que el puerto seleccionado sea el correcto. |

### Archivos de la interfaz

- `app.py`: servidor Flask + lectura/escritura USB vía PySerial.
- `telemetry.py`: parser del texto del firmware.
- `templates/index.html`: interfaz.
- `static/app.css`, `static/app.js`: apariencia, gráficos y comandos.
- `tests/test_parser.py`: pruebas del formato de telemetría.

Este proyecto no incluye el firmware del servo ni lo modifica automáticamente.

### Inicio mediante archivo .bat

En Windows, también podés hacer doble clic en `INICIAR_PANEL.bat`, que verifica las dependencias y abre el navegador. La primera vez requiere Internet para instalar Flask y PySerial; después el panel funciona sin Internet.
