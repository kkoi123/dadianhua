#!/usr/bin/env python3
"""Copy selected unmodified Mbed TLS files from the checksum-pinned archive."""
import hashlib
import json
from pathlib import Path
import sys
import tarfile

ROOT = Path(__file__).resolve().parent
EXPECTED = '19e5b81fdac0fe22009b9e2bdcd52d7dcafbf62bc67fc59cf0a76b5b5540d149'
archive = Path(sys.argv[1])
if hashlib.sha256(archive.read_bytes()).hexdigest() != EXPECTED:
    raise SystemExit('Mbed TLS archive checksum mismatch')
headers = ('md', 'md_internal', 'sha256', 'pkcs5', 'platform_util', 'platform',
           'platform_time', 'threading', 'error', 'check_config', 'asn1',
           'constant_time', 'base64', 'bignum', 'md2', 'md4', 'md5', 'ripemd160', 'sha1', 'sha512')
files = ['LICENSE', 'library/common.h', 'library/constant_time_internal.h', 'library/constant_time_invasive.h', 'library/constant_time.c']
files += ['library/' + name + '.c' for name in ('md', 'sha256', 'sha1', 'base64', 'pkcs5', 'platform_util')]
files += ['include/mbedtls/' + name + '.h' for name in headers]
dest = ROOT / 'vendor' / 'mbedtls'
manifest = {}
with tarfile.open(archive, 'r:bz2') as source:
    for relative in files:
        member = source.getmember('mbedtls-2.28.10/' + relative)
        if not member.isfile():
            raise SystemExit('Unexpected non-file: ' + relative)
        data = source.extractfile(member).read()
        output = dest / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        manifest[relative] = hashlib.sha256(data).hexdigest()
record = {
    'version': '2.28.10',
    'source': 'https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-2.28.10/mbedtls-2.28.10.tar.bz2',
    'archive_sha256': EXPECTED,
    'selected_license': 'Apache-2.0',
    'files_unmodified': True,
    'files': manifest,
}
(dest / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
print('Vendored', len(files), 'unmodified Mbed TLS files.')
