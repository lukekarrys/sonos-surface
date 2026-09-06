#!/usr/bin/env python3
"""Build, flash and monitor; no secrets are compiled into firmware."""
import argparse
import subprocess
import json
import time
import select
import sys
from serial_device import open_port, wait_ready
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('action', choices=['build', 'flash', 'monitor', 'reset', 'reboot'])
parser.add_argument('board', choices=['stick', 'waveshare'])
parser.add_argument('--port')
parser.add_argument('--seconds', type=float, default=0, help='Bound monitor duration (0 = until Ctrl-C)')
parser.add_argument('--download-mode', action='store_true', help='reset only: serial already shows waiting for download')
parser.add_argument('--touch-diagnostic', action='store_true',
                    help='Waveshare coordinate display; touch actions disabled (set runtime read_only=true)')
args = parser.parse_args()
if args.touch_diagnostic and args.board != 'waveshare':
    parser.error('--touch-diagnostic requires waveshare')
if args.action != 'build' and not args.port:
    parser.error('--port is required for USB commands')
if args.download_mode and args.action != 'reset':
    parser.error('--download-mode is only for reset')
fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,UploadSpeed=460800,'
fqbn += 'FlashSize=8M,PartitionScheme=default_8MB' if args.board == 'stick' else 'FlashSize=16M,PartitionScheme=app3M_fat9M_16MB'
base = ['arduino-cli', '--config-file', str(ROOT / '.deps/arduino-cli.yaml')]
build = ROOT / '.build' / (args.board + ('-touch' if args.touch_diagnostic else '-runtime'))
sketch = ROOT / 'firmware/sonos_surface'
if args.action == 'build':
    command = base + ['compile', '--fqbn', fqbn, '--libraries', str(ROOT / 'libraries'),
                      '--build-path', str(build), '--warnings', 'default',
                      '--build-property', 'compiler.cpp.extra_flags=-std=gnu++17 -DSURFACE_' + args.board.upper() +
                      ' -DSURFACE_TOUCH_DIAGNOSTIC=' + str(int(args.touch_diagnostic)),
                      str(sketch)]
elif args.action == 'flash':
    if not (build / 'sonos_surface.ino.bin').exists():
        parser.error('Build this board first')
    # Keep Arduino's pinned upload recipe, overriding reset and native USB baud.
    options = json.loads((build / 'build.options.json').read_text())
    properties = options.get('customBuildProperties', '').replace(',', ' ').split()
    if '-DSURFACE_' + args.board.upper() not in properties:
        parser.error('Built image board differs; rebuild before flashing')
    hardware = Path(options['hardwareFolders'].split(',')[0])
    platform = hardware / 'esp32/hardware/esp32/3.3.11/platform.txt'
    prefix = 'tools.esptool_py.upload.pattern_args='
    recipe = next(line for line in platform.read_text().splitlines() if line.startswith(prefix))
    if '--after hard-reset' not in recipe:
        parser.error('Pinned upload recipe changed; inspect reset behavior before flashing')
    recipe = recipe.replace('--after hard-reset', '--after watchdog-reset')
    # CLI 1.1.1 has already promoted tool properties to upload.* at override time.
    recipe = recipe.replace('tools.esptool_py.upload.pattern_args=', 'upload.pattern_args=', 1)
    # Check pyserial availability before writing flash; required for boot verification.
    try:
        import serial
    except ImportError:
        parser.error('Use .deps/venv/bin/python for USB commands; see README')
    command = base + ['upload', '--fqbn', fqbn, '--port', args.port, '--input-dir', str(build),
                      '--upload-property', recipe, '--upload-property', 'upload.speed=115200', str(sketch)]
elif args.action == 'reset':
    # esptool download-mode transition + watchdog reset; never erase or reflash.
    options_file = build / 'build.options.json'
    hardware = (Path(json.loads(options_file.read_text())['hardwareFolders'].split(',')[0])
                if options_file.exists() else Path.home() / 'Library/Arduino15/packages')
    esptool = hardware / 'esp32/tools/esptool_py/5.3.1/esptool'
    command = [str(esptool), '--chip', 'esp32s3', '--port', args.port,
               '--before', 'no-reset' if args.download_mode else 'default-reset',
               '--after', 'watchdog-reset', '--connect-attempts', '2', 'chip-id']
else:
    with open_port(args.port) as port:
        if args.action == 'reboot':
            wait_ready(port)
            port.write(b'reboot\n')
            # Wait for reboot acknowledgment before accepting readiness from the new boot.
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                line = port.readline().decode(errors='replace')
                if 'REBOOT_BUSY' in line:
                    raise SystemExit('Device busy; reboot was not performed')
                if 'REBOOTING' in line:
                    break
            else:
                raise SystemExit('No reboot acknowledgment')
            wait_ready(port, echo=True)
        else:
            deadline = time.monotonic() + args.seconds if args.seconds else float('inf')
            try:
                while time.monotonic() < deadline:
                    line = port.readline().decode(errors='replace')
                    if line:
                        print(line, end='', flush=True)
                    if sys.stdin.isatty() and select.select([sys.stdin], [], [], 0)[0]:
                        command_line = sys.stdin.readline()
                        if command_line:
                            port.write(command_line.encode())
            except KeyboardInterrupt:
                pass
    raise SystemExit(0)
result = subprocess.run(command, cwd=ROOT)
if result.returncode and args.action == 'build':
    raise SystemExit(result.returncode)
if result.returncode:
    print("Tool reported failure; checking application separately. Do not assume a reflash is needed.", flush=True)
if args.action in ('flash', 'reset'):
    # Watchdog reset may briefly re-enumerate native USB. Reopen, never flash again.
    deadline = time.monotonic() + 10
    while True:
        try:
            port = open_port(args.port)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise SystemExit('USB port did not return; list ports, then inspect/power-cycle the board.')
            time.sleep(0.25)
    with port:
        wait_ready(port, echo=True)
    print('Application ready (peripheral readiness is reported separately).')
if result.returncode:
    raise SystemExit('Tool failed, but application readiness was observed; inspect the tool output above. This does not certify a new image.')
