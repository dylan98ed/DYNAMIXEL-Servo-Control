"""Decodificación de la salida textual del firmware TP2 H42.

No reemplaza DYNAMIXEL: interpreta la línea que la ESP32 imprime por USB.
Ejemplo:
 t=10420 | MODE=PC | MIN=-30.0 | MAX=40.0 | OBJ=40.00 |
 POS=32.15deg | VEL=8.72rpm | I=210.3mA | VIN=24.1V |
 T=32C | MOV=1 | TORQ=1 | HW=0x00
"""

from __future__ import annotations
import re

FLOAT = re.compile(r"^[ \t]*([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)")
TELEMETRY_KEYS = {"t", "MODE", "MIN", "MAX", "OBJ", "ADC", "POS", "VEL", "I", "VIN", "T", "MOV", "TORQ", "HW"}


def parse_telemetry(line: str) -> dict | None:
    """Devuelve los campos de una línea de telemetría válida, o None.

    Si ciertos campos se ocultan mediante flags del sketch, se omiten sin
    fabricar valores. Las lecturas no numéricas también se omiten.
    """
    if not re.search(r"(?:^|\|)\s*t=\d+", line):
        return None

    out: dict = {}
    for token in line.split('|'):
        if '=' not in token:
            continue
        key, val = [s.strip() for s in token.split('=', 1)]
        if key not in TELEMETRY_KEYS:
            continue
        if key == 'MODE':
            out['MODE'] = val.upper()
        elif key == 'HW':
            try:
                out['HW'] = int(val, 16) if val.lower().startswith('0x') else int(val)
            except ValueError:
                continue
        else:
            match = FLOAT.match(val)
            if match:
                number = float(match.group(1))
                if key in {'t', 'ADC', 'MOV', 'TORQ', 'T'}:
                    out[key] = int(number)
                else:
                    out[key] = number

    return out if 't' in out and len(out) >= 2 else None


def parse_info(line: str) -> dict:
    """Extrae datos básicos que imprime INFO, sin tocar registros."""
    out: dict = {}
    if '[INFO]' not in line:
        return out
    m = re.search(r'limites SOFT=\[\s*([-+\d.]+)\s*,\s*([-+\d.]+)\s*\]', line, re.I)
    if m:
        out['MIN'] = float(m.group(1))
        out['MAX'] = float(m.group(2))
    m = re.search(r'modo=(PC|POT)', line, re.I)
    if m:
        out['MODE'] = m.group(1).upper()
    m = re.search(r'torque=(ON|OFF)', line, re.I)
    if m:
        out['TORQ'] = int(m.group(1).upper() == 'ON')
    return out
