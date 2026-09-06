"""Native USB helpers: avoid monitor-induced control-line resets; verify the app."""
import time


def open_port(name):
    try:
        import serial
    except ImportError:
        raise SystemExit('Use .deps/venv/bin/python; install pyserial per README')
    # pyserial 3.5's POSIX open() explicitly writes both control lines, even
    # when set False before opening. On these native USB devices that reset the
    # chip in measurements. Monitoring must not issue either modem-line ioctl.
    class NativeUsbSerial(serial.Serial):
        def _update_dtr_state(self):
            pass

        def _update_rts_state(self):
            pass

    port = NativeUsbSerial(port=None, baudrate=115200, timeout=0.5, write_timeout=5)
    port.port = name
    port.open()
    return port


def wait_ready(port, seconds=20, echo=False):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode(errors='replace').strip()
        if echo and line:
            print(line, flush=True)
        if 'READY: NFC/touch;' in line or ('heartbeat ' in line and 'busy=0' in line):
            return
    raise SystemExit('Application readiness not observed. Inspect serial/reset; a verified flash is not a verified boot.')


def exchange(port, command, success, failures, seconds=10):
    wait_ready(port)
    port.write((command + '\n').encode())
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline().decode(errors='replace').strip()
        if any(token in line for token in failures):
            raise SystemExit(line)
        if success in line:
            return line
    raise SystemExit('No acknowledgment received; inspect device before retrying.')
