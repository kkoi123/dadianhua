#!/usr/bin/env python3
"""Allowlisted clean release from dist; never connect to a module."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
HERE = Path(__file__).resolve().parent
WEB = ("index.html", "app.css", "app.js", "events.js", "sms-codec.js", "communications.js", "call-audio.js", "audio.js", "pcm-worklet.js")
STATIC = [name + suffix for name in WEB for suffix in ("", ".gz")]
PROGRAMS = ["www/cgi-bin/api", "www/cgi-bin/qdc-comm", "www/bin/qdc-audio", "www/bin/qdc-pcm", "www/bin/qdc-esim", "lib/libqdc-euicc.so"]
STANDALONE = ["boot-control.sh", "init.sh", "service.sh", "qdc-config-store", "qdc-entry", "qdc-network-watch", "phtunnel", "provenance.json", "lib/libc.so", "lib/libgcc_s.so.1", "voice/qdc507_aprv3.ko", "voice/qdc507_voice.ko"]
LICENSES = ["MbedTLS-LICENSE.txt", "cJSON-MIT.txt", "libeuicc-LGPL-2.1.txt", "lpac-cjson-ext-MIT.txt"]


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def manifest(directory, filename, paths=None):
    paths = paths if paths is not None else sorted(p for p in directory.rglob("*") if p.is_file() and p.name != filename)
    (directory / filename).write_text("".join(digest(p) + "  " + p.relative_to(directory).as_posix() + "\n" for p in paths), encoding="utf-8")


def copy(src, dst):
    if src.is_symlink() or not src.is_file():
        raise RuntimeError("Missing or unsafe input: " + str(src))
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    dst.chmod(0o755 if src.suffix == ".sh" or src.name in ("install.py", "api", "qdc-comm", "qdc-audio", "qdc-pcm", "qdc-esim", "qdc-entry", "qdc-config-store", "qdc-network-watch", "phtunnel", "libc.so") else 0o644)


def tar_bytes(paths):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz", format=tarfile.PAX_FORMAT) as tar:
        for src, name in paths:
            data = src.read_bytes()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755 if src.suffix == ".sh" else 0o644
            info.mtime = 0
            tar.addfile(info, io.BytesIO(data))
    return stream.getvalue()


def build(output):
    version = re.search(r'#define VERSION "([0-9.]+)"', (ROOT / "api.c").read_text()).group(1)
    release_name = "qdc507-module-web-" + version
    release = output / release_name
    output.mkdir(parents=True, exist_ok=True)
    if release.exists():
        raise RuntimeError("Release folder already exists; choose a new --output directory: " + str(release))
    # Existing web manifest must validate before using the compiled payload.
    for line in (DIST / "www.sha256").read_text().splitlines():
        expected, path = line.split("  ", 1)
        if digest(DIST / "www" / path) != expected:
            raise RuntimeError("Web dist checksum failed: " + path)
    for dist_name, src in (("service.sh", "standalone-service.sh"), ("init.sh", "standalone-init.sh"), ("boot-control.sh", "standalone-boot-control.sh")):
        if digest(DIST / "standalone" / dist_name) != digest(ROOT / src):
            raise RuntimeError("Standalone script differs from source: " + dist_name)
    payload = release / "payload"
    inputs = ["www/" + n for n in STATIC] + PROGRAMS + ["standalone/" + n for n in STANDALONE] + ["licenses/" + n for n in LICENSES] + ["usb-compat/qdc_usb_compat.ko"]
    for name_in_dist in inputs:
        copy(DIST / name_in_dist, payload / name_in_dist)
    runtime_licenses = ("musl-COPYRIGHT.txt", "GCC-Runtime-Exception.txt", "GPL-3.0.txt")
    for name in runtime_licenses:
        copy(HERE / "third-party-licenses" / name, payload / "licenses" / name)
    for name_web in WEB:
        if digest(ROOT / "static" / name_web) != digest(payload / "www" / name_web):
            raise RuntimeError("Public asset differs from source: " + name_web)
    copy(ROOT / "module-web-remote.sh", payload / "control.sh")
    # Old dist manifests may include Finder metadata or old script hashes.
    # Generate fresh runtime manifests exclusively from the release allowlist.
    manifest(payload / "www", "www.sha256")
    manifest(payload / "standalone" / "voice", "sha256sums")
    manifest(payload / "standalone", "sha256sums", sorted(p for p in (payload / "standalone").rglob("*") if p.is_file()))
    manifest(payload, "PAYLOAD_SHA256")
    for name in ("install.py", "module-install.sh", "README-安装说明.md", "THIRD-PARTY.md"):
        copy(HERE / name, release / name)
    third = release / "third-party-source"
    third.mkdir()
    sources = []
    for sub in ("vendor/lpac/euicc", "vendor/lpac/cjson-ext", "vendor/cjson"):
        for p in sorted((ROOT / sub).rglob("*")):
            if p.is_file() and not p.is_symlink() and not p.name.startswith(".") and not p.name.startswith("._"):
                sources.append((p, p.relative_to(ROOT).as_posix()))
    for name in ("vendor/lpac/provenance.json", "vendor/lpac/REUSE.toml", "vendor/lpac/LICENSES/MIT.txt", "vendor/lpac/LICENSES/LGPL-2.1-only.txt"):
        sources.append((ROOT / name, name))
    copy(HERE / "rebuild-libeuicc.sh", third / "rebuild-libeuicc.sh")
    (third / "libeuicc-source.tar.gz").write_bytes(tar_bytes(sources))
    metadata = {"project": "QDC507 module web", "version": version, "releaseType": "first-install", "target": {"architecture": "ARMv7 EABI5 soft-float", "kernel": "3.18.44", "model": "QDC507", "requiresStockVendorLibraries": True}, "payloadFiles": len(inputs) + len(runtime_licenses) + 5, "payloadBytes": sum(p.stat().st_size for p in payload.rglob("*") if p.is_file()), "containsDeviceCredentials": False, "freshDeviceInstallLiveTested": False, "buildDate": "2026-10-13"}
    (release / "release.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    manifest(release, "SHA256SUMS")
    files = sorted(p for p in release.rglob("*") if p.is_file())
    tar_path, zip_path = output / (release_name + ".tar.gz"), output / (release_name + ".zip")
    with tarfile.open(tar_path, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        for p in files:
            info = archive.gettarinfo(str(p), arcname=release_name + "/" + p.relative_to(release).as_posix())
            info.uid = info.gid = 0; info.uname = info.gname = ""; info.mtime = 0
            with p.open("rb") as source:
                archive.addfile(info, source)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for p in files:
            info = zipfile.ZipInfo(release_name + "/" + p.relative_to(release).as_posix(), date_time=(2026, 10, 13, 0, 0, 0))
            info.create_system = 3; info.external_attr = (p.stat().st_mode & 0xffff) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, p.read_bytes())
    (output / (release_name + ".sha256")).write_text("".join(digest(p) + "  " + p.name + "\n" for p in (tar_path, zip_path)), encoding="utf-8")
    print(json.dumps({"directory": str(release), "tar": str(tar_path), "zip": str(zip_path), "files": len(files), "payloadBytes": metadata["payloadBytes"], "zipBytes": zip_path.stat().st_size}, ensure_ascii=False))
    return release


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "releases")
    build(parser.parse_args().output.resolve())
