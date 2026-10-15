#!/usr/bin/env python3
"""Offline ELF compatibility and package-size audit; target code is never run."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import re

binary = Path(sys.argv[1]).resolve()
package = Path(sys.argv[2]).resolve()
report = Path(sys.argv[3]).resolve()
data = binary.read_bytes()
dynamic = len(sys.argv)>4 and sys.argv[4]=='--dynamic'
esim_library = len(sys.argv)>4 and sys.argv[4]=='--esim-library' and binary.name=='libqdc-euicc.so'
dynamic = dynamic or esim_library
if data[:6] != b'\x7fELF\x01\x01':
    raise SystemExit('Target must be ELF32 little-endian')
header = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
elf_type, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phcount, shsize, shcount, shnames = header
if elf_type not in ((2,3) if dynamic else (2,)) or machine != 40 or version != 1:
    raise SystemExit('Target must be an ARM executable')
if flags >> 24 != 5 or not flags & 0x200 or flags & 0x400:
    raise SystemExit('Target must use ARM EABI5 soft-float calling convention')
programs = [struct.unpack_from('<IIIIIIII', data, phoff + i * phsize) for i in range(phcount)]
if not dynamic and any(program[0] in (2, 3) for program in programs):
    raise SystemExit('Target must be static, with neither PT_DYNAMIC nor PT_INTERP')
dependencies=[]
exports=[]
if dynamic:
    interpreters=[data[p[1]:p[1]+p[4]].rstrip(b'\0') for p in programs if p[0]==3]
    if interpreters!=([] if esim_library else [b'/lib/ld-linux.so.3']):
        raise SystemExit('Helper must use the module glibc EABI interpreter')
    sections=[struct.unpack_from('<10I',data,shoff+i*shsize) for i in range(shcount)]
    for s in sections:
        if s[1]!=6: continue
        strings=sections[s[6]]; table=data[strings[4]:strings[4]+strings[5]]
        for pos in range(s[4],s[4]+s[5],s[9]):
            tag,value=struct.unpack_from('<II',data,pos)
            if tag==1: dependencies.append(table[value:].split(b'\0')[0].decode())
    # ARM glibc provides the stack protector guard in its dynamic loader.
    allowed={'libc.so.6','libdl.so.2','libpthread.so.0','ld-linux.so.3'}
    if binary.name=='qdc-esim': allowed.add('libqdc-euicc.so')
    if esim_library: allowed.add('libm.so.6')
    if set(dependencies)-allowed:
        raise SystemExit('Unexpected helper dependency: '+repr(dependencies))
    for s in sections:
        if s[1]!=11: continue
        strings=sections[s[6]]; table=data[strings[4]:strings[4]+strings[5]]
        for pos in range(s[4],s[4]+s[5],s[9]):
            name,value,size,info,other,index=struct.unpack_from('<IIIBBH',data,pos)
            symbol=table[name:].split(b'\0')[0]
            if index: exports.append(symbol.decode())
            if index and symbol in {b'memcpy',b'memset',b'memmove',b'__chk_fail'}:
                raise SystemExit('Helper must not interpose glibc runtime symbols: '+symbol.decode())
    for v in re.findall(rb'GLIBC_(\d+\.\d+(?:\.\d+)?)',data):
        if tuple(map(int,v.split(b'.')))>(2,17): raise SystemExit('Requires a newer glibc than the module: '+v.decode())
if binary.name=='qdc-pcm' and 'main' not in exports:
    raise SystemExit('Module liblog needs the main symbol exported by qdc-pcm')
stacks = [program for program in programs if program[0] == 0x6474e551]
if len(stacks) != 1 or stacks[0][6] & 1:
    raise SystemExit('GNU_STACK missing or executable')
files = []
for path in sorted(package.rglob('*')):
    # The standalone tunnel is a separately built/installed package with its
    # own manifest. Keep the original web package's 1 MiB gate scoped to web.
    if path.relative_to(package).parts[0] == 'standalone':
        continue
    if path.is_symlink():
        raise SystemExit('Package must not contain symlinks')
    if path.is_file():
        content = path.read_bytes()
        files.append({'path': str(path.relative_to(package)), 'bytes': len(content),
                      'sha256': hashlib.sha256(content).hexdigest()})
total = sum(item['bytes'] for item in files)
limit = 1024 * 1024
if total > limit:
    raise SystemExit('Package exceeds the configured size target: ' + str(total))
output = {
    'target': 'ARMv7 Cortex-A7 / Linux / '+('glibc' if dynamic else 'musl')+' / EABI5 soft-float',
    'elf_class': 32, 'little_endian': True, 'e_machine': machine,
    'e_flags': hex(flags), 'statically_linked': not dynamic,
    'dynamic_dependencies': dependencies, 'exported_symbols': exports, 'executable_stack': False,
    'target_binary_bytes': len(data), 'target_sha256': hashlib.sha256(data).hexdigest(),
    'compiler': 'official Zig 0.13.0, macOS aarch64',
    'crypto': ('Mbed TLS 2.28.10 SHA-1/Base64 for RFC6455 handshake only' if binary.name=='qdc-audio' else 'Mbed TLS 2.28.10 SHA-256/HMAC/PBKDF2 authentication'),
    'target_execution_performed': False,
    'package_bytes': total, 'package_files': files,
}
report.parent.mkdir(parents=True, exist_ok=True)
report.write_text(json.dumps(output, indent=2) + '\n')
print('ELF audit passed: ARM32 EABI5 soft-float, '+('glibc helper' if dynamic else 'static CGI')+', nonexecutable stack.')
print('Binary:', len(data), 'bytes; install package:', total, 'bytes.')
