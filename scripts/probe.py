#!/usr/bin/env python3
"""Compile/run a read-only Mac probe through the shared C++ Sonos adapter."""
import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--ip', required=True)
parser.add_argument('--uid')
parser.add_argument('--queue-start', type=int, default=0)
parser.add_argument('--queue-count', type=int, default=2)
parser.add_argument('--samples', type=int, default=1)
parser.add_argument('--interval-ms', type=int, default=10000)
args = parser.parse_args()
build = ROOT / '.build/host'
build.mkdir(parents=True, exist_ok=True)
includes = [ROOT / 'libraries/SurfaceCore/src', ROOT / 'libraries/SurfaceSonos/src',
            ROOT / '.deps/arduino/libraries/SurfaceJson/src', ROOT / '.deps/arduino/libraries/SurfaceXml/src']
sources = [*sorted(includes[0].glob('*.cpp')), *sorted(includes[1].glob('*.cpp')),
           includes[3] / 'tinyxml2.cpp', ROOT / 'tests/sonos_read.cpp']
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                *['-I' + str(p) for p in includes], *map(str, sources), '-lcurl', '-o', str(build / 'sonos_read')], check=True)
subprocess.run([str(build / 'sonos_read'), args.ip, args.uid or '', str(args.queue_start), str(args.queue_count), str(args.samples), str(args.interval_ms)], check=True)
