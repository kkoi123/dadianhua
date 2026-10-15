#!/usr/bin/env python3
"""Fetch official ARM client and isolated runtime; never access device credentials."""
import hashlib, io, json, pathlib, struct, tarfile, urllib.request
import runpy

patch_client = runpy.run_path(str(pathlib.Path(__file__).with_name('patch-oray-retry.py')))['patch_client']

root = pathlib.Path(__file__).resolve().parent
cache = root / 'build/standalone'
dest = root / 'dist/standalone'
cache.mkdir(parents=True, exist_ok=True)
(dest / 'lib').mkdir(parents=True, exist_ok=True)

def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        if not response.url.startswith('https://'):
            raise RuntimeError('Insecure redirect')
        return response.read(8 * 1024 * 1024 + 1)

url = 'https://dl.oray.com/hsk/rom/OpenWrt_PHTunnel_V1.0.tar.gz'
archive = cache / 'oray-official.tar.gz'
data = archive.read_bytes() if archive.exists() else fetch(url)
assert hashlib.sha256(data).hexdigest() == 'eabef3677ddd6896044dba5344b2a41dfcadc163ed2478d558ecfef008cec240'
archive.write_bytes(data)
member = 'OpenWrt_PHTunnel_V1.0.0/arm_cortex-a7-musl/1.0.0/bin/phtunnel'
with tarfile.open(fileobj=io.BytesIO(data)) as tar:
    client = tar.extractfile(member).read()
assert hashlib.sha256(client).hexdigest() == '0b72d3c46919d29f71eb8b54a2442868ce993f809d60340d8c1751f8833e5737'
# The SDK re-executes a sibling named phtunnel. Let that worker be executed
# directly using an owned interpreter path in its existing mapped slot;
# This pathname adaptation does not alter instructions or ELF segment headers.
# The separate version-locked retry-timer patch is applied and recorded below.
# System runtime files are not replaced.
assert client[:7] == b'\x7fELF\x01\x01\x01'
assert struct.unpack_from('<H', client, 18)[0] == 40  # EM_ARM
phoff = struct.unpack_from('<I', client, 28)[0]
phsize, phcount = struct.unpack_from('<HH', client, 42)
assert phsize == 32 and phoff + phsize * phcount <= len(client)
interpreters = [phoff + i * phsize for i in range(phcount)
                if struct.unpack_from('<I', client, phoff + i * phsize)[0] == 3]
assert len(interpreters) == 1
header = interpreters[0]
old_offset = struct.unpack_from('<I', client, header + 4)[0]
old_size = struct.unpack_from('<I', client, header + 16)[0]
assert client[old_offset:old_offset + old_size] == b'/lib/ld-musl-arm.so.1\x00'
interpreter = b'/usrdata/qdc-web/ld\x00'
assert len(interpreter) <= old_size
patched = bytearray(client)
patched[old_offset:old_offset + old_size] = interpreter.ljust(old_size, b'\x00')
# Prove every byte of the upstream binary outside this pathname slot is
# unchanged. Retain its verified original hash in the provenance record.
restored = bytearray(patched[:len(client)])
restored[old_offset:old_offset + old_size] = client[old_offset:old_offset + old_size]
assert bytes(restored) == client
adapted_hash = hashlib.sha256(patched).hexdigest()
patched, retry_patch = patch_client(bytes(patched))
(dest / 'phtunnel').write_bytes(patched)
records = [{'url': url, 'archiveSha256': hashlib.sha256(data).hexdigest(),
            'member': member, 'upstreamSha256': hashlib.sha256(client).hexdigest(),
            'sha256': hashlib.sha256(patched).hexdigest(),
            'adaptation': 'Private PT_INTERP pathname and one version-locked UDP login timer instruction; ELF headers unchanged',
            'interpreterAdaptedSha256': adapted_hash,
            'instructionPatches': [retry_patch],
            'interpreter': interpreter[:-1].decode()}]

base = 'https://downloads.openwrt.org/releases/22.03.7/targets/mediatek/mt7629/packages/'
manifest = fetch(base + 'Packages').decode()
for package in ['libc_1.2.3-4_arm_cortex-a7.ipk', 'libgcc1_11.2.0-4_arm_cortex-a7.ipk']:
    record = next((dict(line.split(': ', 1) for line in stanza.splitlines() if ': ' in line)
                  for stanza in manifest.split('\n\n') if 'Filename: ' + package + '\n' in stanza + '\n'), None)
    p = cache / package
    data = p.read_bytes() if p.exists() else fetch(base + package)
    digest = hashlib.sha256(data).hexdigest()
    if record is not None:
        assert digest == record['SHA256sum'], package
    p.write_bytes(data)
    with tarfile.open(fileobj=io.BytesIO(data)) as outer:
        payload = outer.extractfile('./data.tar.gz').read()
    with tarfile.open(fileobj=io.BytesIO(payload)) as tar:
        for item in tar:
            if item.isfile() and item.name.startswith('./lib/'):
                name = pathlib.Path(item.name).name
                (dest / 'lib' / name).write_bytes(tar.extractfile(item).read())
    records.append({'url': base + package, 'sha256': digest,
                    'verifiedAgainst': base + 'Packages' if record else 'official HTTPS download, computed digest'})

for name in ['phtunnel', 'lib/libc.so', 'lib/libgcc_s.so.1']:
    p = dest / name
    assert p.is_file(), name
    p.chmod(0o700 if name == 'phtunnel' else 0o600)
(dest / 'provenance.json').write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n')
print('Official ARM client and private musl/libgcc runtime ready:',
      sum(p.stat().st_size for p in dest.rglob('*') if p.is_file()), 'bytes')
