#!/usr/bin/env python3
"""Send a private configuration file over USB; no secret command-line arguments."""
import argparse
import json
from pathlib import Path
from serial_device import open_port, exchange

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--file', type=Path, default=Path('.local/config.json'))
args = parser.parse_args()
payload = json.dumps(json.loads(args.file.read_text()), separators=(',', ':'))
if len(payload.encode()) > 4088:
    parser.error('Configuration too large')
with open_port(args.port) as port:
    exchange(port, 'config ' + payload, 'CONFIG_SAVED',
             ['CONFIG_INVALID', 'CONFIG_BUSY', 'CONFIG_SAVE_FAILED'])
    print('Configuration saved; board is rebooting. Open the monitor.')
