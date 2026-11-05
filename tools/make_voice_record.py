#!/usr/bin/env python3
"""Generate deterministic NKVP/NKVN sectors from model-builder bundles (stdlib only)."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'firmware/data/sanotts'
KEYS = ('SAAN_DEC_HEAD', 'SAAN_G_EXCITE', 'SAAN_PHRASE_RULE_SBV2', 'SAAN_TAIL_PAUSE', 'SAAN_INT8_ACT')


def number(value):
    if isinstance(value, bool):
        return math.nan
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str) and re.fullmatch(r'[+-]?\d+(\.\d+)?([eE][+-]?\d+)?', value.strip()):
        return float(value)
    return math.nan


def valid_name(value):
    return isinstance(value, str) and re.fullmatch(r'[ -~]{1,31}', value) is not None


def bundle_values(bundle):
    with zipfile.ZipFile(bundle) as archive:
        manifest = json.loads(archive.read('build-flags.json'))
        blob = archive.read('model-int8.bin')
    sha = hashlib.sha256(blob).hexdigest()
    if (not 0 < len(blob) <= 16 * 1024 * 1024 or manifest.get('blobBytes') != len(blob)
            or str(manifest.get('blobSha256', '')).lower() != sha):
        raise ValueError('bundle model size/hash mismatch')
    flags = manifest.get('model', {})
    if any(not re.fullmatch(r'\d+', str(flags.get(key, ''))) for key in KEYS):
        raise ValueError('invalid model flags')
    voice = manifest.get('voice') or {}
    shape = manifest.get('f0Shape')
    if shape is None and 'k' in voice:
        shape = voice
    values = [1., 0., 0., 0., 75.]
    if shape is not None:
        values = [number(shape.get(key, 75 if key == 'tauMs' else None))
                  for key in ('k', 'centerHz', 'loHz', 'hiHz', 'tauMs')]
        if values[0] == 1:
            values[1:4] = [x if x >= 0 else 0. for x in values[1:4]]
            if not values[4] > 0:
                values[4] = 75.
    raw = voice.get('vnoise')
    if raw is None and int(flags['SAAN_G_EXCITE']) == 1:
        raise ValueError('excitation model requires voice.vnoise')
    noise = number(raw) if raw is not None else .03
    if not 0 < noise <= 1:
        raise ValueError('invalid vnoise')
    noise = struct.unpack('<f', struct.pack('<f', noise))[0]
    k, center, lo, hi, tau = values
    if (not all(math.isfinite(x) for x in values + [noise]) or not 0 <= k <= 3
            or not 0 < noise <= 1 or (k != 1 and not (0 < lo < center < hi and tau > 0))):
        raise ValueError('invalid voice values')
    candidate = re.sub(r'\.[^.]*$', '', bundle.name)
    name = candidate if valid_name(candidate) else None
    return blob, sha, values + [noise], name


def generate(bundle):
    blob, sha, values, name = bundle_values(bundle)
    sector = bytearray(b'\xff' * 4096)
    sector[:96] = bytes(96)
    struct.pack_into('<4sHHII32s5df', sector, 0, b'NKVP', 1, 0, len(blob), 0, bytes.fromhex(sha), *values)
    struct.pack_into('<I', sector, 92, zlib.crc32(sector[:92]))
    if name:
        sector[96:136] = bytes(40)
        struct.pack_into('<4sB', sector, 96, b'NKVN', len(name))
        sector[101:101 + len(name)] = name.encode('ascii')
        struct.pack_into('<I', sector, 132, zlib.crc32(sector[96:132]))
    return bytes(sector)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', nargs='?', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check', action='store_true', help='compare without writing')
    args = parser.parse_args()
    if args.output and not args.bundle:
        parser.error('--output requires a bundle')
    bundles = [args.bundle] if args.bundle else [DATA / f'{voice}-v2-excite-bundle.zip' for voice in ('female2', 'man2')]
    for bundle in bundles:
        output = args.output or bundle.with_name(bundle.name.replace('-bundle.zip', '-record.bin'))
        if output == bundle:
            parser.error('use --output for a bundle without the -bundle.zip suffix')
        expected = generate(bundle)
        if args.check:
            if not output.exists() or output.read_bytes() != expected:
                raise SystemExit(f'{output}: differs or missing')
        else:
            output.write_bytes(expected)
        print(f'{output.name}: {hashlib.sha256(expected).hexdigest()} ({len(expected)} bytes)')


if __name__ == '__main__':
    main()
