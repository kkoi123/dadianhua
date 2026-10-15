#!/usr/bin/env python3
"""Compare the C primitive with Python's independent hashlib implementation."""
import hashlib
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()

def run(*args):
    return subprocess.check_output([str(binary), *args], text=True).strip()

for text in ('', 'abc', 'a' * 1000):
    actual = run('sha256', text)
    expected = hashlib.sha256(text.encode()).hexdigest()
    if actual != expected:
        raise SystemExit('SHA-256 vector failed')
vectors = (
    ('password', 'salt', 1, 32),
    ('password', 'salt', 2, 32),
    ('password', 'salt', 4096, 64),
    ('password', 'salt', 600000, 32),
    ('long-password-' * 12, 'module-admin-随机盐', 600000, 32),
    ('', '', 2, 32),
)
for password, salt, rounds, length in vectors:
    actual = run('pbkdf2', password, salt, str(rounds), str(length))
    expected = hashlib.pbkdf2_hmac('sha256', password.encode(), salt.encode(), rounds, length).hex()
    if actual != expected:
        raise SystemExit('PBKDF2-SHA256 vector failed for rounds=' + str(rounds))
print('Crypto verification passed: 3 SHA-256 and 6 PBKDF2-SHA256 vectors, including 600000 rounds and long passwords.')
