#!/usr/bin/env python3
"""Independent real-file verification and malformed-input integration tests."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def invoke(probe, mix, destination):
    destination.mkdir(parents=True, exist_ok=True)
    return subprocess.run(
        [str(probe), str(mix), str(destination / 'manifest.json'),
         str(destination / 'extracted')],
        capture_output=True, text=True, timeout=30,
    )


def classic(entries, payload):
    return (struct.pack('<HI', len(entries), len(payload)) +
            b''.join(struct.pack('<III', *entry) for entry in entries) + payload)


def verify_real(probe, mix, index, root):
    reference = json.loads(index.read_text())
    result = invoke(probe, mix, root / 'real')
    require(result.returncode == 0, result.stderr)
    manifest = json.loads((root / 'real/manifest.json').read_text())
    entries = manifest['entries']
    require(manifest['entry_count'] == len(entries) == len(reference['entries']),
            'entry count differs from reference')
    require(manifest['data_start'] == reference['data_start'], 'data_start differs')
    total = 0
    # Open the MIX independently and seek to every absolute offset from the manifest.
    with mix.open('rb') as stream:
        for number, (actual, expected) in enumerate(zip(entries, reference['entries'])):
            for key in ('id', 'size', 'offset'):
                require(actual[key] == expected[key], f'entry {number}: {key} differs')
            stream.seek(actual['offset'])
            data = stream.read(actual['size'])
            require(len(data) == actual['size'], f'entry {number}: short read')
            require(hashlib.sha256(data).hexdigest() == actual['sha256'],
                    f'entry {number}: SHA-256 differs')
            total += len(data)
    first = next(entry for entry in entries if entry['size'])
    extracted = root / 'real/extracted' / f'{first["id"]:08x}.bin'
    contents = extracted.read_bytes()
    with mix.open('rb') as stream:
        stream.seek(first['offset'])
        require(contents == stream.read(first['size']), 'extracted bytes differ from MIX')
    sha256 = hashlib.sha256(contents).hexdigest()
    require(sha256 == first['sha256'] and sha256 in result.stdout,
            'extracted SHA-256 differs or was not printed')
    print(f'PASS real MIX: {len(entries)} entries; data_start={manifest["data_start"]}; '
          f'all id/size/offset fields match reference')
    print(f'PASS SHA-256: {len(entries)}/{len(entries)} independently verified; '
          f'hashed bytes={total}')
    print(f'PASS extraction: {extracted.name}; bytes={len(contents)}; sha256={sha256}')


def verify_cases(probe, root):
    payload = b'abc'
    valid = classic([(0xFEDCBA98, 0, 3)], payload)
    valid_cases = {
        'classic-known-sha256': valid,
        'ts-ra2-unencrypted': struct.pack('<I', 0) + valid,
        'zero-length-entry': classic([(1, 0, 0), (2, 0, 3)], payload),
        'classic-empty-archive': classic([], b''),
        'ts-ra2-empty-archive': struct.pack('<IHI', 0, 0, 0),
        'zero-length-at-end': classic([(1, 3, 0), (2, 0, 3)], payload),
    }
    for name, data in valid_cases.items():
        source = root / f'{name}.mix'
        source.write_bytes(data)
        output = root / name
        result = invoke(probe, source, output)
        require(result.returncode == 0, f'{name}: {result.stderr}')
        manifest = json.loads((output / 'manifest.json').read_text())
        for entry in manifest['entries']:
            part = data[entry['offset']:entry['offset'] + entry['size']]
            require(hashlib.sha256(part).hexdigest() == entry['sha256'],
                    f'{name}: SHA-256 mismatch')
        if name == 'classic-known-sha256':
            require(manifest['entries'][0]['sha256'] ==
                    'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
                    'SHA-256 known vector failed')
        require(source.read_bytes() == data, f'{name}: input changed')
    print(f'PASS valid fixtures: {len(valid_cases)} (classic, TS/RA2, empty archive, '
          'empty resource, known SHA-256 vector)')

    cases = {
        'empty-file': (b'', 'empty MIX file'),
        'one-byte-header': (b'\x01', 'truncated MIX header/index'),
        'short-classic-header': (b'\x01\x00\x00', 'truncated MIX header/index'),
        'short-ts-ra2-header': (b'\0' * 8, 'truncated MIX header/index'),
        'truncated-index': (struct.pack('<HI', 2, 0), 'truncated MIX index'),
        'huge-count': (struct.pack('<HI', 65535, 0), 'truncated MIX index'),
        'truncated-payload': (valid[:-1], 'bad MIX data length'),
        'trailing-byte': (valid + b'x', 'bad MIX data length'),
        'bad-declared-length': (valid[:2] + struct.pack('<I', 0xFFFFFFFF) + valid[6:],
                                'bad MIX data length'),
        'offset-out-of-bounds': (classic([(1, 4, 1)], payload), 'out of bounds'),
        'size-out-of-bounds': (classic([(1, 0, 4)], payload), 'out of bounds'),
        'u32-overflow-range': (classic([(1, 0xFFFFFFFF, 0xFFFFFFFF)], payload), 'out of bounds'),
        'late-invalid-entry': (classic([(1, 0, 3), (2, 2, 2)], payload), 'out of bounds'),
        'encrypted': (struct.pack('<IHI', 0x20000, 1, 3), 'encrypted MIX is unsupported'),
        'unsupported-flags': (struct.pack('<IHI', 0x40000, 1, 3), 'unsupported MIX flags'),
        'missing-checksum': (struct.pack('<I', 0x10000) + valid, 'bad MIX data length'),
    }
    for name, (data, message) in cases.items():
        source = root / f'{name}.mix'
        source.write_bytes(data)
        output = root / name
        result = invoke(probe, source, output)
        require(result.returncode == 1, f'{name}: expected exit 1, got {result.returncode}')
        require(message in result.stderr, f'{name}: {result.stderr}')
        require(not (output / 'manifest.json').exists(), f'{name}: wrote manifest')
        require(not (output / 'extracted').exists(), f'{name}: extracted invalid data')
        require(source.read_bytes() == data, f'{name}: input changed')
        print(f'PASS {name}: exit=1; {result.stderr.strip()}')

    result = invoke(probe, root / 'nonexistent.mix', root / 'missing-file')
    require(result.returncode == 1 and 'not a regular file' in result.stderr,
            'missing input not rejected')
    print('PASS missing input: exit=1; clear error, no crash')

    source = root / 'classic-known-sha256.mix'
    result = subprocess.run([str(probe), str(source), str(source), str(root / 'collision')],
                            capture_output=True, text=True, timeout=30)
    require(result.returncode == 1 and 'overwrite MIX input' in result.stderr,
            'input/output collision not rejected')
    require(source.read_bytes() == valid, 'collision overwrote input')
    print('PASS input/output collision: exit=1; input preserved')
    print(f'PASS malformed/error cases: {len(cases) + 2}; no crash or invalid output')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--mix', type=Path)
    parser.add_argument('--index', type=Path)
    parser.add_argument('--self-authored-only', action='store_true',
                        help='Use only generated archives; no game data required')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='.probe-test-', dir=Path(__file__).resolve().parent) as tmp:
        root = Path(tmp)
        if not args.self_authored_only:
            if not args.mix or not args.index:
                parser.error('--mix and --index are required unless --self-authored-only is set')
            verify_real(args.probe.resolve(), args.mix, args.index, root)
        verify_cases(args.probe.resolve(), root)
    print('ALL CHECKS PASSED')


if __name__ == '__main__':
    main()
