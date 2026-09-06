#!/usr/bin/env python3
"""Build, flash and monitor; no secrets are compiled into firmware."""
import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('action', choices=['build', 'flash', 'monitor'])
parser.add_argument('board', choices=['stick', 'waveshare'])
parser.add_argument('--port')
parser.add_argument('--touch-diagnostic', action='store_true',
                    help='Waveshare coordinate display; always read-only, touch actions disabled')
parser.add_argument('--allow-sonos-mutations', action='store_true',
                    help='Explicit playback-test build; current authorization is Office only')
args = parser.parse_args()
if args.touch_diagnostic and (args.board != 'waveshare' or args.allow_sonos_mutations):
    parser.error('--touch-diagnostic requires waveshare without --allow-sonos-mutations')
if args.action != 'build' and not args.port:
    parser.error('--port is required for flash/monitor')
fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,UploadSpeed=460800,'
fqbn += 'FlashSize=8M,PartitionScheme=default_8MB' if args.board == 'stick' else 'FlashSize=16M,PartitionScheme=app3M_fat9M_16MB'
base = ['arduino-cli', '--config-file', str(ROOT / '.deps/arduino-cli.yaml')]
build = ROOT / '.build' / (args.board + ('-touch' if args.touch_diagnostic else
                         '-playback' if args.allow_sonos_mutations else '-readonly'))
sketch = ROOT / 'firmware/sonos_surface'
if args.action == 'build':
    command = base + ['compile', '--fqbn', fqbn, '--libraries', str(ROOT / 'libraries'),
                      '--build-path', str(build), '--warnings', 'default',
                      '--build-property', 'compiler.cpp.extra_flags=-std=gnu++17 -DSURFACE_' + args.board.upper() +
                      ' -DSURFACE_ALLOW_SONOS_MUTATIONS=' + str(int(args.allow_sonos_mutations)) +
                      ' -DSURFACE_TOUCH_DIAGNOSTIC=' + str(int(args.touch_diagnostic)),
                      str(sketch)]
elif args.action == 'flash':
    if not (build / 'sonos_surface.ino.bin').exists():
        parser.error('Build this board first')
    command = base + ['upload', '--fqbn', fqbn, '--port', args.port, '--input-dir', str(build), str(sketch)]
else:
    command = base + ['monitor', '--port', args.port, '--config', 'baudrate=115200']
subprocess.run(command, cwd=ROOT, check=True)
