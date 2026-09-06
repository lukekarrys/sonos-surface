#!/usr/bin/env python3
"""Compile/run a read-only Mac probe through the shared C++ Sonos adapter."""
import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--ip', required=True)
parser.add_argument('--uid')
args = parser.parse_args()
build = ROOT / '.build/host'
build.mkdir(parents=True, exist_ok=True)
includes = [ROOT / 'libraries/SurfaceCore/src', ROOT / 'libraries/SurfaceSonos/src',
            ROOT / '.deps/arduino/libraries/SurfaceJson/src', ROOT / '.deps/arduino/libraries/SurfaceXml/src']
sources = [includes[0] / 'SurfaceCore.cpp', includes[1] / 'SurfaceSonos.cpp',
           includes[3] / 'tinyxml2.cpp', ROOT / 'tests/sonos_read.cpp']
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                *['-I' + str(p) for p in includes], *map(str, sources), '-lcurl', '-o', str(build / 'sonos_read')], check=True)
subprocess.run([str(build / 'sonos_read'), args.ip, *([args.uid] if args.uid else [])], check=True)
