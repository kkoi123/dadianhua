#!/usr/bin/env python3
"""Version-locked ARM adaptation: UDP login retry uses udpkeepalive.

Only accepts the previously verified private-interpreter PHTunnel build.
No credentials, configuration, ELF headers, or other instructions are changed.
"""
import hashlib
import json
from pathlib import Path
import struct

BASE_SHA256 = 'd12352f1d848a04f935b6d693b60482ae785e13843b7b794dfcaff1c234cd1a3'
ADDRESS = 0x27b0c
ORIGINAL = bytes.fromhex('102702e3')  # movw r2, #10000
REPLACEMENT = bytes.fromhex('502094e5')  # ldr r2, [r4, #0x50]


def patch_client(client):
    if hashlib.sha256(client).hexdigest() != BASE_SHA256:
        raise ValueError('Unsupported PHTunnel hash; refusing instruction patch')
    if client[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H', client, 18)[0] != 40:
        raise ValueError('Expected little-endian ARM ELF32')
    phoff = struct.unpack_from('<I', client, 28)[0]
    size, count = struct.unpack_from('<HH', client, 42)
    if size != 32 or phoff + size * count > len(client):
        raise ValueError('Invalid ELF program headers')
    offsets = []
    for i in range(count):
        kind, offset, address, _, filesz, _, flags, _ = struct.unpack_from('<8I', client, phoff + size * i)
        if kind == 1 and flags & 1 and address <= ADDRESS and ADDRESS + 4 <= address + filesz:
            offsets.append(offset + ADDRESS - address)
    if len(offsets) != 1 or offsets[0] + 4 > len(client):
        raise ValueError('Patch address must map to exactly one executable file segment')
    offset = offsets[0]
    if client[offset:offset + 4] != ORIGINAL:
        raise ValueError('Original instruction mismatch')
    patched = client[:offset] + REPLACEMENT + client[offset + 4:]
    restored = patched[:offset] + ORIGINAL + patched[offset + 4:]
    if restored != client or len(patched) != len(client):
        raise ValueError('Unexpected changes outside patch instruction')
    return patched, {
        'name': 'udp-login-retry-configured-interval',
        'baseSha256': BASE_SHA256,
        'sha256': hashlib.sha256(patched).hexdigest(),
        'virtualAddress': hex(ADDRESS), 'fileOffset': hex(offset),
        'original': ORIGINAL.hex(), 'replacement': REPLACEMENT.hex(),
        'behavior': 'First UDP login immediate; login timeout/retry reads udpkeepalive (+0x50); successful heartbeat unchanged',
    }


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--record', type=Path)
    args = parser.parse_args()
    patched, record = patch_client(args.source.read_bytes())
    args.output.write_bytes(patched)
    args.output.chmod(0o700)
    if args.record:
        args.record.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
