"""Panel local USB/Serial para el firmware Control_H42_PC_Arduino.ino.

La PC es dueña exclusiva del puerto COM. Abrir el Monitor Serie de Arduino
simultáneamente impedirá conectarse en Windows.
"""

from __future__ import annotations
import csv
from collections import deque
from datetime import datetime
from io import StringIO
import os
import re
import threading
import time

from flask import Flask, Response, jsonify, render_template, request
import serial
from serial.tools import list_ports

from telemetry import parse_telemetry, parse_info

SERIAL_BAUD = 115200  # PC <-> ESP32 (NO el baudrate RS-485 del servo)
HOST = os.environ.get('PANEL_HOST', '127.0.0.1')
WEB_PORT = int(os.environ.get('PANEL_PORT', '5000'))
MAX_LOGS = 600
MAX_SAMPLES = 1200
COMMAND_RE = re.compile(r"^[A-Za-z]+(?:\s+[-+\w.]+)?$")
BARE = {'HELP', 'PING', 'INFO', 'STATUS', 'ARM', 'OFF', 'STOP'}
ENUM = {'MODE': {'PC', 'POT'}, 'STREAM': {'ON', 'OFF'}}
NUMERIC = {'POS', 'MIN', 'MAX', 'VEL', 'ACC', 'RATE'}


class SerialBridge:
    def __init__(self):
        self.lock = threading.RLock()
        self.device = None
        self.reader = None
        self.stop_evt = threading.Event()
        self.port = None
        self.error = ''
        self.latest: dict = {}
        self.last_sample_at = None
        self.samples = deque(maxlen=MAX_SAMPLES)
        self.logs = deque(maxlen=MAX_LOGS)
        self.log_id = 0
        self.started_at = time.time()

    def _log(self, content: str, kind: str = 'rx'):
        with self.lock:
            self.log_id += 1
            self.logs.append({
                'id': self.log_id,
                'time': datetime.now().strftime('%H:%M:%S'),
                'text': content[:500],
                'kind': kind,
            })

    def connect(self, port: str):
        if not port or len(port) > 128:
            raise ValueError('Seleccioná un puerto válido.')
        with self.lock:
            if self.device is not None:
                raise RuntimeError('Ya hay un puerto conectado. Desconectá primero.')
            self.stop_evt = threading.Event()
            self.error = ''
            try:
                ser = serial.Serial(port, SERIAL_BAUD, timeout=0.18, write_timeout=1)
            except (serial.SerialException, OSError) as exc:
                raise RuntimeError(f'No se pudo abrir {port}: {exc}') from exc
            self.device = ser
            self.port = port
            self.latest = {}
            self.last_sample_at = None
            self.samples.clear()
            self._log(f'Puerto {port} abierto a {SERIAL_BAUD} baudios.', 'sys')
            self.reader = threading.Thread(target=self._reading_loop, args=(ser, self.stop_evt), daemon=True)
            self.reader.start()

    def disconnect(self):
        with self.lock:
            self.stop_evt.set()
            dev = self.device
            self.device = None
            self.port = None
            self.last_sample_at = None
        if dev is not None:
            try:
                dev.close()
            except (serial.SerialException, OSError):
                pass
        self._log('Puerto serie desconectado.', 'sys')

    def _reading_loop(self, dev, stop_evt):
        while not stop_evt.is_set():
            try:
                line = dev.readline()
                if not line:
                    continue
                decoded = line.decode('utf-8', errors='replace').strip('\r\n\x00 ')
                if not decoded:
                    continue
                self._log(decoded)
                parsed = parse_telemetry(decoded)
                info = parse_info(decoded)
                with self.lock:
                    if parsed:
                        self.latest.update(parsed)
                        ts = time.time()
                        self.last_sample_at = ts
                        self.samples.append({'ts': round(ts, 3), **parsed})
                    if info:
                        self.latest.update(info)
                    # Los mensajes de estado también importan si el stream está apagado.
                    if decoded.startswith('[MODE] PC'):
                        self.latest['MODE'] = 'PC'
                    elif decoded.startswith('[MODE] POT'):
                        self.latest['MODE'] = 'POT'
                    elif decoded.startswith('[ARM]'):
                        self.latest['TORQ'] = 1
                    elif decoded.startswith('[OFF]') or '[FALLO]' in decoded:
                        self.latest['TORQ'] = 0
            except (serial.SerialException, OSError) as exc:
                if not stop_evt.is_set():
                    with self.lock:
                        if self.device is dev:
                            self.error = str(exc)
                            self._log(f'Error de lectura: {exc}', 'err')
                break
        # No se llama a disconnect() aquí para evitar cerrar un puerto nuevo
        # desde una hebra antigua; el puerto actual se libera solo si es el suyo.
        with self.lock:
            if self.device is dev:
                self.device = None
                self.port = None
                try:
                    dev.close()
                except (serial.SerialException, OSError):
                    pass

    def send(self, command: str):
        command = command.strip().upper()
        if not command or '\n' in command or '\r' in command or len(command) > 80:
            raise ValueError('Comando vacío o inválido.')
        if not COMMAND_RE.fullmatch(command):
            raise ValueError('Formato inválido. Usá HELP para ver los comandos.')
        parts = command.split()
        name = parts[0]
        if name in BARE:
            if len(parts) != 1:
                raise ValueError(f'{name} no acepta argumentos.')
        elif name in ENUM:
            if len(parts) != 2 or parts[1] not in ENUM[name]:
                raise ValueError(f'{name} permite: {", ".join(sorted(ENUM[name]))}.')
        elif name in NUMERIC:
            if len(parts) != 2:
                raise ValueError(f'Falta un valor para {name}.')
            try:
                value = float(parts[1])
            except ValueError as exc:
                raise ValueError('El argumento debe ser numérico.') from exc
            if not (-180 <= value <= 180) and name in {'POS','MIN','MAX'}:
                raise ValueError('En este panel se admiten ángulos de −180° a +180° como máximo.')
            if name == 'VEL' and not 0.5 <= value <= 100:
                raise ValueError('Velocidad fuera del rango del panel.')
            if name == 'ACC' and (int(value) != value or not 1 <= value <= 1000):
                raise ValueError('ACC debe ser entero positivo.')
            if name == 'RATE' and (int(value) != value or not 100 <= value <= 5000):
                raise ValueError('RATE debe estar entre 100 y 5000 ms.')
        else:
            raise ValueError('Comando no reconocido por este panel.')

        with self.lock:
            if self.device is None or not self.device.is_open:
                raise RuntimeError('Conectá la ESP32 antes de enviar comandos.')
            try:
                self.device.write((command + '\n').encode('ascii'))
                self.device.flush()
            except (serial.SerialException, OSError) as exc:
                self.error = str(exc)
                raise RuntimeError(f'Error enviando comando: {exc}') from exc
        self._log('> ' + command, 'tx')
        return command

    def snapshot(self, after: int = 0):
        with self.lock:
            connected = self.device is not None and self.device.is_open
            return {
                'connected': connected,
                'port': self.port,
                'baud': SERIAL_BAUD,
                'error': self.error,
                'last_sample_at': self.last_sample_at,
                'latest': dict(self.latest),
                'logs': [dict(x) for x in self.logs if x['id'] > after][-200:],
                'samples': list(self.samples)[-350:],
            }

    def export_csv(self):
        with self.lock:
            rows = list(self.samples)
        buff = StringIO()
        columns = ['ts', 't', 'MODE', 'MIN', 'MAX', 'OBJ', 'ADC', 'POS', 'VEL', 'I', 'VIN', 'T', 'MOV', 'TORQ', 'HW']
        w = csv.DictWriter(buff, fieldnames=columns, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)
        return buff.getvalue()


app = Flask(__name__)
bridge = SerialBridge()


@app.get('/')
def index():
    return render_template('index.html')


@app.get('/api/ports')
def ports():
    return jsonify([{
        'device': p.device,
        'description': p.description or p.device,
    } for p in list_ports.comports()])


@app.post('/api/connect')
def connect():
    port = str((request.get_json(silent=True) or {}).get('port') or '').strip()
    try:
        bridge.connect(port)
        return jsonify(ok=True, message=f'Conectado a {port}')
    except (ValueError, RuntimeError) as exc:
        return jsonify(ok=False, error=str(exc)), 400


@app.post('/api/disconnect')
def disconnect():
    bridge.disconnect()
    return jsonify(ok=True)


@app.post('/api/command')
def command():
    cmd = str((request.get_json(silent=True) or {}).get('command') or '')
    try:
        sent = bridge.send(cmd)
        return jsonify(ok=True, command=sent)
    except (ValueError, RuntimeError) as exc:
        return jsonify(ok=False, error=str(exc)), 400


@app.get('/api/state')
def state():
    after = request.args.get('after', default=0, type=int)
    return jsonify(bridge.snapshot(max(0, after)))


@app.get('/api/export.csv')
def export_csv():
    content = '\ufeff' + bridge.export_csv()  # BOM para abrir UTF-8 en Excel
    return Response(content, mimetype='text/csv; charset=utf-8', headers={
        'Content-Disposition': 'attachment; filename="telemetria_h42.csv"'
    })


if __name__ == '__main__':
    print(f'Panel H42: http://{HOST}:{WEB_PORT}')
    print('El Monitor Serie de Arduino debe estar CERRADO.')
    app.run(host=HOST, port=WEB_PORT, debug=False, use_reloader=False, threaded=True)
