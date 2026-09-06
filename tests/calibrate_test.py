import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from calibrate import fit_samples

raw = [(87,120), (307,136), (69,289), (304,279), (72,441), (312,427)]
lines = []
for i, (rx, ry) in enumerate(raw):
    x, y = (92,276)[i % 2], (140,270,406)[i // 2]
    lines.append(f'[calibration] target={i+1} expected={x},{y} first={rx},{ry} last={rx},{ry} release={rx},{ry}')
text = '\n'.join(lines)
fit, residual = fit_samples(text, 21)
assert abs(fit['x_scale'] - 0.792093109) < 1e-9
assert abs(fit['y_offset'] - 27.650440782) < 1e-9
assert residual < 10
for bad in ['\n'.join(lines[:5]), '\n'.join(lines[1:]), text + '\n' + lines[0], text.replace('first=87,120', 'first=0,0')]:
    try:
        fit_samples(bad, 21)
    except ValueError:
        pass
    else:
        raise AssertionError('Bad/incomplete run accepted')
print('Calibration tool checks passed: measured fit and malformed/incomplete runs')
