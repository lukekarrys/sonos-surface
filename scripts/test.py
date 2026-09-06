#!/usr/bin/env python3
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.build/host'
BUILD.mkdir(parents=True, exist_ok=True)
includes = [ROOT / 'libraries/SurfaceCore/src', ROOT / 'libraries/SurfaceSonos/src',
            ROOT / '.deps/arduino/libraries/SurfaceJson/src', ROOT / '.deps/arduino/libraries/SurfaceXml/src']
sources = [*sorted((ROOT / 'libraries/SurfaceCore/src').glob('*.cpp')),
           *sorted((ROOT / 'libraries/SurfaceSonos/src').glob('*.cpp')),
           includes[-1] / 'tinyxml2.cpp', ROOT / 'tests/core_test.cpp']
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                *['-I' + str(p) for p in includes], *map(str, sources), '-o', str(BUILD / 'core_test')], check=True)
subprocess.run([str(BUILD / 'core_test')], check=True)
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'libraries/SurfaceDevice/src'),
                '-I' + str(includes[-2]),
                str(ROOT / 'tests/touch_test.cpp'), '-o', str(BUILD / 'touch_test')], check=True)
subprocess.run([str(BUILD / 'touch_test')], check=True)
subprocess.run(['python3', '-B', str(ROOT / 'tests/calibrate_test.py')], check=True)
