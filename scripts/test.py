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

buttons = ROOT / '.deps/arduino/libraries/M5Unified/src'
if not (buttons / 'utility/Button_Class.hpp').exists():
    buttons = ROOT / '.deps/host/M5Buttons/src'
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'libraries/SurfaceDevice/src'),
                '-I' + str(includes[0]), '-I' + str(buttons),
                str(buttons / 'utility/Button_Class.cpp'),
                str(ROOT / 'tests/stick_button_test.cpp'), '-o', str(BUILD / 'stick_button_test')], check=True)
subprocess.run([str(BUILD / 'stick_button_test')], check=True)

subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'libraries/SurfaceDevice/src'),
                *['-I' + str(p) for p in includes],
                *map(str, sorted((ROOT / 'libraries/SurfaceCore/src').glob('*.cpp'))),
                str(ROOT / 'tests/waveshare_ui_test.cpp'), '-o', str(BUILD / 'waveshare_ui_test')], check=True)
subprocess.run([str(BUILD / 'waveshare_ui_test')], check=True)

subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I' + str(ROOT / 'libraries/SurfaceDevice/src'), '-I' + str(includes[0]),
                str(ROOT / 'tests/artwork_test.cpp'), '-o', str(BUILD / 'artwork_test')], check=True)
subprocess.run([str(BUILD / 'artwork_test')], check=True)
