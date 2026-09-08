"""Profile/USB/flash fixtures; no network or physical devices are used."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import config_profile as profiles
import configure
import serial_device

SECRET = 'secret with spaces "quotes" \\slashes # $()'


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / 'arbitrary label-2.json'
        self.env = self.root / '.env'
        self.env.write_text('WIFI_SSID=House Wi-Fi\nWIFI_PASSWORD="' + SECRET + '"\n')
        self.config = {'wifi_ssid': '${WIFI_SSID}', 'wifi_password': '${WIFI_PASSWORD}',
                       'read_only': True, 'rooms': {'office': {}}}
        self.write(self.config)
        self.env_patch = patch.dict(os.environ, {}, clear=True)
        self.env_patch.start()
        self.addCleanup(self.env_patch.stop)

    def write(self, config):
        self.path.write_text(json.dumps(config))

    def load(self, **kwargs):
        return profiles.load_profile(self.path, self.env, **kwargs)

    def test_default_and_arbitrary_paths(self):
        parser = argparse.ArgumentParser()
        profiles.add_profile_arguments(parser)
        self.assertEqual(parser.parse_args([]).config, ROOT / 'config/default.json')
        args = parser.parse_args(['--config', str(self.path), '--env-file', str(self.env)])
        self.assertEqual(args.config, self.path)
        self.assertEqual(args.env_file, self.env)
        other = self.root / 'unrelated-room.json'
        other.write_bytes(self.path.read_bytes())
        self.assertEqual(self.load(), profiles.load_profile(other, self.env))
        with patch.object(configure, 'configure_device') as send:
            configure.main(['--port', 'fake', '--config', str(self.path), '--env-file', str(self.env)])
            self.assertEqual(send.call_args.args[1]['rooms'], {'office': {}})
        with patch.object(configure, 'load_profile', return_value=self.load()) as load, \
                patch.object(configure, 'configure_device'):
            configure.main(['--port', 'fake'])
            load.assert_called_once_with(profiles.DEFAULT_CONFIG, None)

    def test_env_values_and_precedence(self):
        config, payload = self.load()
        self.assertEqual(config['wifi_ssid'], 'House Wi-Fi')
        self.assertEqual(config['wifi_password'], SECRET)
        self.assertEqual(json.loads(payload), config)
        with patch.dict(os.environ, WIFI_SSID='Process Wi-Fi', WIFI_PASSWORD=''):
            config, _ = self.load()
        self.assertEqual(config['wifi_ssid'], 'Process Wi-Fi')
        self.assertEqual(config['wifi_password'], '')
        with patch.object(profiles, 'DEFAULT_ENV', self.env):
            self.assertEqual(profiles.load_profile(self.path), self.load())
        with patch.object(profiles, 'DEFAULT_ENV', self.root / 'absent'), \
                patch.dict(os.environ, WIFI_SSID='Process', WIFI_PASSWORD=SECRET):
            self.assertEqual(profiles.load_profile(self.path)[0]['wifi_password'], SECRET)
        with self.assertRaises(profiles.ProfileError):
            profiles.load_profile(self.path, self.root / 'absent')

    def test_simple_env_grammar(self):
        self.env.write_text("# comment\n\n A = ' spaces = # remain ' \nB=literal\\n$HOME\nC=\n")
        self.assertEqual(profiles.load_env(self.env),
                         {'A': ' spaces = # remain ', 'B': 'literal\\n$HOME', 'C': ''})
        for text in ['A=x\nA=y', 'export A=x', 'A', 'A="unclosed', SECRET]:
            self.env.write_text(text)
            with self.assertRaises(profiles.ProfileError) as error:
                profiles.load_env(self.env)
            self.assertNotIn(SECRET, str(error.exception))

    def test_missing_multiple_and_no_evaluation(self):
        self.config['wifi_ssid'] = '${PREFIX} ${WIFI_SSID}/${PREFIX}'
        self.write(self.config)
        with self.assertRaises(profiles.ProfileError):
            self.load()
        config, _ = self.load(environ={'PREFIX': 'The'})
        self.assertEqual(config['wifi_ssid'], 'The House Wi-Fi/The')
        for value in ['${WIFI_SSID:-default}', '${}', '${WIFI_SSID', '${OTHER}']:
            self.config['wifi_ssid'] = value
            self.write(self.config)
            with self.assertRaises(profiles.ProfileError):
                self.load()
        self.config['wifi_ssid'] = '${WIFI_SSID}'
        self.write(self.config)
        with self.assertRaises(profiles.ProfileError):
            self.load(environ={'WIFI_SSID': '${WIFI_PASSWORD}'})
        literal = '$(touch should-not-exist) `echo no` $HOME'
        config, _ = self.load(environ={'WIFI_SSID': literal})
        self.assertEqual(config['wifi_ssid'], literal)
        self.assertFalse((self.root / 'should-not-exist').exists())

    def test_json_and_resolved_limits(self):
        for text in ['{"wifi_password":"' + SECRET + '"', '{"rooms":${WIFI_SSID}}',
                     '{"rooms":{},"rooms":{}}', '{"read_only":NaN}']:
            self.path.write_text(text)
            with self.assertRaises(profiles.ProfileError) as error:
                self.load()
            self.assertNotIn(SECRET, str(error.exception))
        for value in [[], {'rooms': []}, {'read_only': 'false'}, {'wifi_password': 2},
                      {'unexpected': True}, {'${WIFI_SSID}': 'value'}]:
            self.write(value)
            with self.assertRaises(profiles.ProfileError):
                self.load()
        self.write(self.config)
        with self.assertRaises(profiles.ProfileError):
            self.load(environ={'WIFI_PASSWORD': 'é' * 2100})
        config, payload = self.load(environ={'WIFI_PASSWORD': '\", "read_only": false, "x": "'})
        self.assertTrue(json.loads(payload)['read_only'])
        self.assertEqual(json.loads(payload), config)
        self.config['rooms'] = {'x': [[[[[[[{}]]]]]]]}
        self.write(self.config)
        with self.assertRaises(profiles.ProfileError):
            self.load()

    def test_no_secret_files_or_diagnostics(self):
        before = {p: p.read_bytes() for p in self.root.iterdir()}
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output), \
                patch.object(configure, 'open_port', side_effect=RuntimeError(SECRET)):
            with self.assertRaises(SystemExit):
                configure.main(['--port', 'fake', '--config', str(self.path), '--env-file', str(self.env)])
        self.assertNotIn(SECRET, output.getvalue())
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.iterdir()})
        self.env.write_text('WIFI_SSID=only\n')
        with patch.object(configure, 'open_port') as opened, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                configure.main(['--port', 'fake', '--config', str(self.path), '--env-file', str(self.env)])
            opened.assert_not_called()

    def test_upload_queries_status_and_checks_result(self):
        config, payload = self.load()
        config['rooms']['office'] = {'album': {}}
        payload = json.dumps(config)
        before = {'policyRevision': 7, 'read_only': False, 'rooms': {}}
        after = {'policyRevision': 8, 'read_only': True, 'rooms': {'office': {}}}
        ports = []

        class Port:
            def __init__(self, state):
                self.state, self.writes, self.lines = state, [], []
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def reset_input_buffer(self):
                self.lines.clear()
            def write(self, data):
                self.writes.append(data)
                self.lines = [b'device-config ' + json.dumps(self.state).encode() if data == b'config-status\n'
                              else b'CONFIG_SAVED rebooting']
            def readline(self):
                return self.lines.pop(0)

        for result in [after, {**after, 'policyRevision': 7}, {**after, 'rooms': {}},
                       {**after, 'read_only': False}]:
            ports = [Port(before), Port(result)]
            files = {p: p.read_bytes() for p in self.root.iterdir()}
            output = io.StringIO()
            with patch.object(configure, 'ready_port', side_effect=ports), contextlib.redirect_stdout(output):
                if result == after:
                    configure.configure_device('fake', config, payload)
                else:
                    with self.assertRaises(profiles.ProfileError):
                        configure.configure_device('fake', config, payload)
            self.assertEqual(ports[0].writes, [b'config-status\n', ('config ' + payload + '\n').encode()])
            self.assertEqual(ports[1].writes, [b'config-status\n'])
            self.assertNotIn(SECRET, output.getvalue())
            self.assertEqual(files, {p: p.read_bytes() for p in self.root.iterdir()})
        port = Port(before)
        with patch.object(port, 'readline', return_value=('CONFIG_INVALID ' + SECRET).encode()):
            with self.assertRaises(profiles.ProfileError) as error:
                configure.request(port, payload, 'CONFIG_SAVED', ('CONFIG_INVALID',))
            self.assertNotIn(SECRET, str(error.exception))

    def test_reboot_port_reopens_without_resending(self):
        first, second = unittest.mock.MagicMock(), unittest.mock.MagicMock()
        with patch.object(configure, 'open_port', side_effect=[first, second]), \
                patch.object(configure, 'wait_ready', side_effect=[OSError(SECRET), None]), \
                patch.object(configure.time, 'sleep'):
            self.assertIs(configure.ready_port('fake'), second)
        first.close.assert_called_once()
        first.write.assert_not_called()
        second.write.assert_not_called()

    def run_flash(self, profile=True, build_code=0, flash_code=0):
        build = self.root / '.build/stick-runtime'
        build.mkdir(parents=True, exist_ok=True)
        (build / 'sonos_surface.ino.bin').touch()
        hardware = self.root / 'sdk'
        platform = hardware / 'esp32/hardware/esp32/3.3.11/platform.txt'
        platform.parent.mkdir(parents=True, exist_ok=True)
        platform.write_text('tools.esptool_py.upload.pattern_args=--after hard-reset\n')
        (build / 'build.options.json').write_text(json.dumps({
            'hardwareFolders': str(hardware), 'customBuildProperties': '-DSURFACE_STICK'}))
        events = []
        def run(command, **kwargs):
            self.assertNotIn(SECRET, str(command))
            is_build = 'build' in command
            events.append('build' if is_build else 'flash')
            return types.SimpleNamespace(returncode=build_code if is_build else flash_code)
        argv = ['device.py', 'flash', 'stick', '--port', 'fake']
        if profile:
            argv += ['--config', str(self.path), '--env-file', str(self.env)]
        with patch.object(sys, 'argv', argv), patch.dict(sys.modules, serial=types.ModuleType('serial')), \
                patch('subprocess.run', side_effect=run), \
                patch.object(serial_device, 'open_port') as opened, \
                patch.object(serial_device, 'wait_ready', side_effect=lambda *a, **k: events.append('ready')), \
                patch.object(configure, 'configure_device', side_effect=lambda *a: events.append('configure')), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            try:
                exec(compile((ROOT / 'scripts/device.py').read_text(), 'device.py', 'exec'),
                     {'__file__': str(self.root / 'scripts/device.py'), '__name__': '__main__'})
            except SystemExit:
                pass
            if not events:
                opened.assert_not_called()
        return events

    def test_flash_profile_preflight_and_order(self):
        self.assertEqual(self.run_flash(), ['build', 'flash', 'ready', 'configure'])
        self.assertEqual(self.run_flash(profile=False), ['flash', 'ready'])
        self.assertEqual(self.run_flash(build_code=1), ['build'])
        self.assertEqual(self.run_flash(flash_code=1), ['build', 'flash', 'ready'])
        self.env.write_text('WIFI_SSID=only\n')
        self.assertEqual(self.run_flash(), [])
        self.path.write_text('{invalid')
        self.assertEqual(self.run_flash(), [])


if __name__ == '__main__':
    unittest.main()
