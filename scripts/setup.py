#!/usr/bin/env python3
"""Pinned dependencies; host-only mode does not install board toolchains."""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / '.deps'
LIBS = DEPS / 'arduino/libraries'
CONFIG = DEPS / 'arduino-cli.yaml'
CORE = 'esp32:esp32@3.3.11'
INDEX = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
M5UNIFIED_VERSION = '0.2.21'
LIBRARIES = ['M5GFX@0.2.28', 'M5Unified@' + M5UNIFIED_VERSION,
             'M5Utility@0.2.0', 'M5HAL@0.1.2', 'M5UnitUnified@0.5.5',
             'M5Unit-NFC@0.1.0', 'GFX Library for Arduino@1.6.7']


def run(*args):
    subprocess.run(args, check=True, cwd=ROOT)


def download(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        run('curl', '--fail', '--location', '--silent', '--show-error', url, '-o', str(path))


def setup(host_only=False):
    LIBS.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text('directories:\n  user: ' + json.dumps(str(DEPS / 'arduino')) + '\n')
    for name, version in [('SurfaceJson', '3.12.0'), ('SurfaceXml', '11.0.0')]:
        lib = LIBS / name
        lib.mkdir(exist_ok=True)
        (lib / 'library.properties').write_text(
            f'name={name}\nversion={version}\nauthor=Upstream contributors\n'
            'maintainer=sonos-surface\nsentence=Portable upstream dependency\n'
            'paragraph=See upstream license\ncategory=Other\narchitectures=*\n')
    download('https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp',
             LIBS / 'SurfaceJson/src/surface_json.hpp')
    download('https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT',
             LIBS / 'SurfaceJson/LICENSE')
    for filename in ['tinyxml2.h', 'tinyxml2.cpp', 'LICENSE.txt']:
        download('https://raw.githubusercontent.com/leethomason/tinyxml2/11.0.0/' + filename,
                 LIBS / 'SurfaceXml' / ('src/' + filename if filename != 'LICENSE.txt' else filename))
    if host_only:
        # Only the SDK-independent button state machine is needed by host tests.
        for filename in ['src/utility/Button_Class.hpp', 'src/utility/Button_Class.cpp', 'LICENSE']:
            download('https://raw.githubusercontent.com/m5stack/M5Unified/' + M5UNIFIED_VERSION + '/' + filename,
                     DEPS / 'host/M5Buttons' / filename)
    if not host_only:
        run('arduino-cli', '--config-file', str(CONFIG), 'core', 'update-index', '--additional-urls', INDEX)
        run('arduino-cli', '--config-file', str(CONFIG), 'core', 'install', CORE, '--additional-urls', INDEX)
        run('arduino-cli', '--config-file', str(CONFIG), 'lib', 'update-index')
        run('arduino-cli', '--config-file', str(CONFIG), 'lib', 'install', '--no-deps', *LIBRARIES)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host-only', action='store_true')
    setup(parser.parse_args().host_only)
