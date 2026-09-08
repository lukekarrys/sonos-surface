#!/usr/bin/env python3
"""Resolve a committed profile, upload it, and verify non-secret device status."""
import argparse
import json
import time
from config_profile import ProfileError, add_profile_arguments, load_profile
from serial_device import open_port, wait_ready


def ready_port(name):
    deadline = time.monotonic() + 30
    while True:
        port = None
        try:
            port = open_port(name)
            wait_ready(port, seconds=max(1, deadline - time.monotonic()))
            return port
        except OSError:
            if port is not None:
                port.close()
            if time.monotonic() >= deadline:
                raise ProfileError('Device port unavailable') from None
            time.sleep(0.25)
        except BaseException:
            if port is not None:
                port.close()
            raise


def request(port, command, success, failures=()):
    port.reset_input_buffer()
    port.write((command + '\n').encode('utf-8'))
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        line = port.readline().decode(errors='replace').strip()
        if any(token in line for token in failures):
            raise ProfileError('Device rejected configuration or failed to save it')
        if success in line:
            return line.split(success, 1)[1]
    raise ProfileError('Device acknowledgment not received; inspect device before retrying')


def status(port):
    result = json.loads(request(port, 'config-status', 'device-config '))
    if (not isinstance(result, dict) or type(result.get('policyRevision')) is not int or
            type(result.get('read_only')) is not bool or not isinstance(result.get('rooms'), dict)):
        raise ProfileError('Invalid device configuration status')
    return result


def configure_device(port_name, config, payload):
    try:
        with ready_port(port_name) as port:
            before = status(port)
            request(port, 'config ' + payload, 'CONFIG_SAVED',
                    ('CONFIG_INVALID', 'CONFIG_BUSY', 'CONFIG_SAVE_FAILED'))
        # The saved acknowledgment precedes reboot. Read readiness afresh, then
        # explicitly query status; never accept an unsolicited pre-upload status.
        with ready_port(port_name) as port:
            after = status(port)
        expected_rooms = {
            room: {kind: fields for kind, fields in policies.items() if fields}
            for room, policies in config.get('rooms', {}).items()
        }
        if (after['policyRevision'] != before['policyRevision'] + 1 or
                after['read_only'] != config.get('read_only', True) or
                after['rooms'] != expected_rooms):
            raise ProfileError('Device configuration status does not match submitted profile')
    except ProfileError:
        raise
    except (Exception, SystemExit):
        # Serial exceptions may contain transmitted bytes; never expose them.
        raise ProfileError('Configuration transfer or status verification failed (values omitted)') from None
    print('Configuration saved; revision, read-only mode, and room policies verified.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    add_profile_arguments(parser)
    args = parser.parse_args(argv)
    try:
        config, payload = load_profile(args.config, args.env_file)
        configure_device(args.port, config, payload)
    except ProfileError as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
