import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from telemetry import parse_telemetry, parse_info


def test_complete_row():
    line = ('t=10420 | MODE=PC | MIN=-30.0 | MAX=40.0 | OBJ=40.00 | '
            'POS=32.15deg | VEL=8.72rpm | I=210.3mA | VIN=24.1V | '
            'T=32C | MOV=1 | TORQ=1 | HW=0x00')
    d = parse_telemetry(line)
    assert d == {'t':10420,'MODE':'PC','MIN':-30.0,'MAX':40.0,'OBJ':40.0,
                 'POS':32.15,'VEL':8.72,'I':210.3,'VIN':24.1,'T':32,
                 'MOV':1,'TORQ':1,'HW':0}


def test_partial_row():
    d = parse_telemetry('t=7811 | POS=-179.98deg | VEL=-12.1rpm | HW=0x04')
    assert d['POS'] == -179.98 and d['VEL'] == -12.1 and d['HW'] == 4


def test_other_logs_not_telemetry():
    assert parse_telemetry('[POS] Pedido=60.00deg | Aplicado=40.00deg') is None
    assert parse_telemetry('t=124 | TELEMETRIA INCOMPLETA (lib=3)') is None


def test_info():
    d = parse_info('[INFO] Limites SOFT=[-50.0,50.0]deg | EEPROM=[-180,180]deg')
    assert d == {'MIN':-50.0,'MAX':50.0}
    d2 = parse_info('[INFO] ID=1 | baud=57600 | modo=POT | torque=ON | streaming=ON/500ms')
    assert d2 == {'MODE':'POT','TORQ':1}
