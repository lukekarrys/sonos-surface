#!/usr/bin/env python3
"""Fit existing raw diagnostic samples or store/read per-device Waveshare calibration."""
import argparse
import json
import re
from pathlib import Path
from serial_device import open_port, exchange

SAMPLE = re.compile(r'\[calibration\] target=(\d+) expected=(\d+),(\d+) first=(\d+),(\d+) last=(\d+),(\d+) release=(\d+),(\d+)')


def fit_axis(raw, expected):
    mean_raw, mean_expected = sum(raw) / len(raw), sum(expected) / len(expected)
    variance = sum((x - mean_raw) ** 2 for x in raw)
    if variance < 100:
        raise ValueError('Samples do not span enough of the screen')
    scale = sum((x - mean_raw) * (y - mean_expected) for x, y in zip(raw, expected)) / variance
    return scale, mean_expected - scale * mean_raw


def fit_samples(text, controller):
    samples = {}
    for match in SAMPLE.finditer(text):
        target, x, y, rx, ry, lx, ly, ex, ey = map(int, match.groups())
        if not 1 <= target <= 6:
            raise ValueError('Unknown target number')
        if target == 1:
            samples = {}  # Use the last complete ordered run, never mix reboots.
        if target != len(samples) + 1:
            raise ValueError('Expected an ordered six-target run')
        if not (0 <= rx < 368 and 0 <= ry < 448) or max(abs(rx-lx), abs(ry-ly), abs(rx-ex), abs(ry-ey)) > 16:
            raise ValueError('Invalid or moving contact; repeat diagnostic')
        if (x, y) != ((92, 276)[(target-1) % 2], (140, 270, 406)[(target-1) // 2]):
            raise ValueError('Unexpected diagnostic target')
        samples[target] = (x, y, rx, ry)
    if len(samples) != 6:
        raise ValueError('Need a complete six-target diagnostic run')
    rows = list(samples.values())
    xs, xo = fit_axis([r[2] for r in rows], [r[0] for r in rows])
    ys, yo = fit_axis([r[3] for r in rows], [r[1] for r in rows])
    if not (0.5 <= xs <= 1.5 and 0.5 <= ys <= 1.5 and abs(xo) <= 112 and abs(yo) <= 112):
        raise ValueError('Fit outside supported correction bounds')
    residual = max(max(abs(xs*rx+xo-x), abs(ys*ry+yo-y)) for x,y,rx,ry in rows)
    if residual > 16:
        raise ValueError(f'Fit residual {residual:.1f}px exceeds 16px; inspect/repeat samples')
    return dict(version=1, controller=controller, x_scale=xs, x_offset=xo, y_scale=ys, y_offset=yo), residual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--samples', type=Path, help='Captured six-target serial log; fit and print without writing')
    parser.add_argument('--controller', type=lambda s: int(s, 0), choices=[21, 56], default=21)
    parser.add_argument('--output', type=Path, help='Save fit JSON for review')
    parser.add_argument('--file', type=Path, help='Reviewed calibration JSON to write; requires --port')
    parser.add_argument('--port', help='Read current calibration, or write --file')
    args = parser.parse_args()
    if args.samples:
        if args.file or args.port:
            parser.error('Fit samples separately; review JSON before --file --port')
        try:
            calibration, residual = fit_samples(args.samples.read_text(), args.controller)
        except ValueError as error:
            parser.error(str(error))
        payload = json.dumps(calibration, indent=2) + '\n'
        print(payload, end='')
        print(f'Max training residual: {residual:.2f}px (fresh taps still required)')
        if args.output:
            args.output.write_text(payload)
    elif args.port:
        if args.output:
            parser.error('--output requires --samples')
        payload = json.dumps(json.loads(args.file.read_text()), separators=(',', ':')) if args.file else None
        if payload and len(payload.encode()) > 1024:
            parser.error('Calibration too large')
        with open_port(args.port) as port:
            print(exchange(port, 'touch-calibration' + (' ' + payload if payload else ''),
                           'CALIBRATION_SAVED' if payload else 'CALIBRATION ',
                           ['CALIBRATION_INVALID', 'CALIBRATION_SAVE_FAILED']))
    else:
        parser.error('Use --samples, or --port with optional --file')


if __name__ == '__main__':
    main()
