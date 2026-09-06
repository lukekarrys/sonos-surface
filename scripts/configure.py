#!/usr/bin/env python3
"""Send a private configuration file over USB; no secret command-line arguments."""
import argparse
import json
import time
from pathlib import Path
try:
    import serial
except ImportError:
    raise SystemExit('Use .deps/venv/bin/python; see README USB configuration setup')

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--file', type=Path, default=Path('.local/config.json'))
args = parser.parse_args()
payload = json.dumps(json.loads(args.file.read_text()), separators=(',', ':'))
if len(payload.encode()) > 4088:
    parser.error('Configuration too large')
port = serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=5)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
with port:
    # Native USB serial opening can reset either board. Wait for firmware input
    # readiness rather than losing config bytes during boot or using a sleep.
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        line = port.readline().decode(errors='replace')
        if 'READY: NFC/touch;' in line or ('heartbeat ' in line and 'busy=0' in line):
            break
    else:
        raise SystemExit('Firmware not ready; configuration was not sent. Check boot logs/port.')
    port.write(('config ' + payload + '\n').encode())
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        line = port.readline().decode(errors='replace').strip()
        if 'CONFIG_SAVED' in line:
            print('Configuration saved; board is rebooting. Open the monitor.')
            break
        if 'CONFIG_INVALID' in line or 'CONFIG_BUSY' in line or 'CONFIG_SAVE_FAILED' in line:
            raise SystemExit(line)
    else:
        raise SystemExit('No CONFIG_SAVED acknowledgment. Check port/monitor, then retry.')
