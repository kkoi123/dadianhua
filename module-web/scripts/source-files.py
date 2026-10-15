"""Shared, explicit source-export selection. Private values are never printed."""
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SKIP_DIRS = {".git", ".tools", "build", "dist", "releases", "backups", "__pycache__", ".workbuddy", ".idea", ".vscode", ".ssh", "private", "sessions", ".tmp_versions", "node_modules"}
SKIP_NAMES = {".DS_Store", ".pairing-token", "Module.symvers", "modules.order", ".env", "core", "SOURCE_SHA256SUMS"}
SKIP_SUFFIXES = (".pyc", ".pyo", ".o", ".ko", ".a", ".so", ".dylib", ".mod.c", ".cmd", ".zip", ".tar.gz", ".tar.xz", ".tar.bz2", ".ipa", ".log", ".pcap", ".pcapng", ".pem", ".key", ".p12", ".mobileprovision", ".wav", ".pcm", ".raw", ".core")
SOURCE_SUFFIXES = {".c", ".h", ".inc", ".py", ".sh", ".js", ".cjs", ".html", ".css", ".md", ".txt", ".toml", ".json", ".config", ".in"}
SOURCE_NAMES = {".gitignore", ".gitattributes", "Makefile", "CMakeLists.txt", "LICENSE", "COPYING", "COPYRIGHT", "module.config"}


def source_files(root=ROOT):
    for current, dirs, files in os.walk(root, followlinks=False):
        for directory in dirs:
            if directory not in SKIP_DIRS and (Path(current) / directory).is_symlink():
                yield Path(current) / directory
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            if name in SKIP_NAMES or name.startswith(("._", ".env.", "initial-password")) or name.endswith(SKIP_SUFFIXES):
                continue
            path = Path(current) / name
            yield path


def inspect(root=ROOT, private_values=()):
    findings = []
    patterns = {
        "personal-home-path": re.compile(rb"/(?:Users|home)/[A-Za-z0-9_.-]+/"),
        "personal-tunnel-host": re.compile(rb"[A-Za-z0-9-]+\.vicp\.fun"),
        "actual-device-identity": re.compile(rb"\bphd[0-9a-f]{12,}\b"),
        "private-key-material": re.compile(rb"-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=]{20,}"),
        "bark-key-url": re.compile(rb"https://api\.day\.app/[A-Za-z0-9_-]{12,}"),
        "apple-signing-team": re.compile(rb"DEVELOPMENT_TEAM\s*=\s*[A-Z0-9]{10}"),
    }
    files = list(source_files(root))
    for file in files:
        name = file.relative_to(root).as_posix()
        if file.is_symlink() or any(parent.is_symlink() for parent in file.parents):
            findings.append({"file": name, "category": "symlink"}); continue
        data = file.read_bytes()
        if file.suffix not in SOURCE_SUFFIXES and file.name not in SOURCE_NAMES:
            findings.append({"file": name, "category": "unrecognized-source-file"})
        if file.suffix == ".json" and file.name != "provenance.json" and not file.name.endswith(".example.json"):
            findings.append({"file": name, "category": "unexpected-json-settings-or-records"})
        if data.startswith((b"\x7fELF", b"PK\x03\x04", b"\x1f\x8b")):
            findings.append({"file": name, "category": "unexpected-binary-or-archive"})
        for label, pattern in patterns.items():
            if pattern.search(data):
                findings.append({"file": name, "category": label})
        if any(value and (value in data or (value.decode(errors="ignore") and value.decode(errors="ignore").encode("utf-16-le") in data)) for value in private_values):
            findings.append({"file": name, "category": "known-private-value"})
    return files, findings
