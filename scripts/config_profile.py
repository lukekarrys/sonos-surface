"""Load environment profiles and expand JSON string values, entirely on the host."""
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / 'config/default.json'
DEFAULT_ENV = ROOT / '.env'
VARIABLE = re.compile(r'\$\{([A-Za-z_][A-Za-z0-9_]*)\}')
KEY = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


class ProfileError(ValueError):
    """Messages must never contain profile text, environment values, or device output."""


def add_profile_arguments(parser, default=DEFAULT_CONFIG):
    parser.add_argument('--config', type=Path, default=default,
                        help='Environment profile path (independent of board and filename)')
    parser.add_argument('--env-file', type=Path,
                        help='Local KEY=VALUE secrets file (default: repo-root .env)')


def load_env(path=None):
    selected = DEFAULT_ENV if path is None else Path(path)
    try:
        text = selected.read_text(encoding='utf-8')
    except FileNotFoundError:
        if path is None:
            return {}
        raise ProfileError('Environment file not found') from None
    except (OSError, UnicodeError):
        raise ProfileError('Cannot read environment file') from None
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if not separator or not KEY.fullmatch(key) or key in values:
            raise ProfileError('Invalid or duplicate environment assignment')
        if value.startswith(('"', "'")):
            if len(value) < 2 or value[-1] != value[0]:
                raise ProfileError('Unclosed environment value quote')
            value = value[1:-1]
        values[key] = value
    return values


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProfileError('Duplicate configuration key')
        result[key] = value
    return result


def reject_constant(_):
    raise ProfileError('Non-finite configuration number')


def parse_json(text):
    try:
        return json.loads(text, object_pairs_hook=unique_object, parse_constant=reject_constant)
    except (ValueError, RecursionError):
        raise ProfileError('Invalid or duplicate configuration JSON (values omitted)') from None


def expand(value, environment, depth=0):
    if isinstance(value, (dict, list)):
        if depth >= 8:
            raise ProfileError('Configuration nesting exceeds eight containers')
        if isinstance(value, dict):
            if any('${' in key for key in value):
                raise ProfileError('Placeholders are allowed only in JSON string values')
            return {key: expand(item, environment, depth + 1) for key, item in value.items()}
        return [expand(item, environment, depth + 1) for item in value]
    if not isinstance(value, str):
        return value

    def substitute(match):
        name = match.group(1)
        if name not in environment:
            raise ProfileError('Missing referenced environment variable (values omitted)')
        return environment[name]

    resolved = VARIABLE.sub(substitute, value)
    if '${' in resolved:
        raise ProfileError('Unresolved or unsupported environment placeholder')
    return resolved


def validate_shape(config):
    # Only host framing/top-level checks. Room/mode semantics belong to firmware.
    if not isinstance(config, dict):
        raise ProfileError('Configuration must be a JSON object')
    strings = {'wifi_ssid', 'wifi_password', 'apple_region'}
    for key, value in config.items():
        if key in strings:
            valid = isinstance(value, str)
        elif key == 'read_only':
            valid = isinstance(value, bool)
        elif key == 'rooms':
            valid = isinstance(value, dict)
        else:
            raise ProfileError('Unknown configuration field')
        if not valid:
            raise ProfileError('Invalid configuration field type')


def load_profile(path=DEFAULT_CONFIG, env_file=None, environ=None):
    try:
        text = Path(path).read_text(encoding='utf-8')
    except (OSError, UnicodeError):
        raise ProfileError('Cannot read configuration profile') from None
    environment = load_env(env_file)
    environment.update(os.environ if environ is None else environ)
    # Parse structure first so quotes/backslashes in credentials cannot inject JSON.
    config = expand(parse_json(text), environment)
    validate_shape(config)
    try:
        payload = json.dumps(config, separators=(',', ':'), ensure_ascii=False, allow_nan=False)
        size = len(payload.encode('utf-8'))
    except (ValueError, UnicodeError):
        raise ProfileError('Invalid resolved configuration JSON (values omitted)') from None
    if size > 4088:
        raise ProfileError('Resolved configuration exceeds 4088 UTF-8 bytes')
    return config, payload
