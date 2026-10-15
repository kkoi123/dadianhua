#!/usr/bin/env python3
"""Offline distribution verification; no ADB calls or hardware writes."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
RELEASE = (Path(os.environ.get("QDC_RELEASE_DIR", str(ROOT / "releases"))) / "qdc507-module-web-0.3.43").resolve()
spec = importlib.util.spec_from_file_location("release_installer", RELEASE / "install.py")
installer = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(installer)


def fixture_manifest(base):
    (base / "SHA256SUMS").write_text(hashlib.sha256((base / "payload.bin").read_bytes()).hexdigest() + "  payload.bin\n")


class ReleaseTests(unittest.TestCase):
    def test_complete_package_and_both_archives(self):
        self.assertEqual(installer.verify_package(RELEASE), 56)
        files = {p.relative_to(RELEASE).as_posix(): p.read_bytes() for p in RELEASE.rglob("*") if p.is_file()}
        prefix = RELEASE.name + "/"
        with zipfile.ZipFile(RELEASE.parent / (RELEASE.name + ".zip")) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual({name[len(prefix):]: archive.read(name) for name in archive.namelist()}, files)
        with tarfile.open(RELEASE.parent / (RELEASE.name + ".tar.gz")) as archive:
            self.assertTrue(all(m.isfile() and m.uid == 0 and m.gid == 0 for m in archive.getmembers()))
            self.assertEqual({m.name[len(prefix):]: archive.extractfile(m).read() for m in archive.getmembers()}, files)
        for sub, manifest in (("payload", "PAYLOAD_SHA256"), ("payload/www", "www.sha256"), ("payload/standalone", "sha256sums"), ("payload/standalone/voice", "sha256sums")):
            directory = RELEASE / sub
            for line in (directory / manifest).read_text().splitlines():
                expected, name = line.split("  ", 1)
                self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), expected)

    def test_tampered_missing_extra_or_symlink_files_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            file = base / "payload.bin"; file.write_bytes(b"expected")
            fixture_manifest(base)
            self.assertEqual(installer.verify_package(base), 1)
            file.write_bytes(b"tampered")
            with self.assertRaises(ValueError): installer.verify_package(base)
            file.write_bytes(b"expected")
            extra = base / "private.json"; extra.write_text("{}")
            with self.assertRaises(ValueError): installer.verify_package(base)
            extra.unlink(); file.unlink()
            with self.assertRaises(ValueError): installer.verify_package(base)
            extra.write_bytes(b"expected"); file.symlink_to(extra)
            with self.assertRaises(ValueError): installer.verify_package(base)

    def test_cross_platform_manifest_traversal_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp).resolve()
            for name in ("../outside", "..\\outside", "/outside", "C:\\outside", "a/../b", "a//b"):
                (base / "SHA256SUMS").write_text("0" * 64 + "  " + name + "\n")
                with self.assertRaises(ValueError): installer.verify_package(base)

    def test_usb_selection_refuses_ambiguous_or_network_devices(self):
        one = "List of devices attached\nUSB001\tdevice\n"
        self.assertEqual(installer.select_device(one), "USB001")
        self.assertEqual(installer.select_device(one + "USB002\tdevice\n", "USB002"), "USB002")
        for devices in ("List of devices attached\n", one + "USB002\tdevice\n", "List of devices attached\n192.168.1.2:5555\tdevice\n", "List of devices attached\nUSB001\tunauthorized\n"):
            with self.assertRaises(ValueError): installer.select_device(devices)
        with self.assertRaises(ValueError): installer.select_device(one, "USB002")

    def test_module_script_refuses_existing_install_before_writing(self):
        # Run the real freshness guard against an isolated local directory.
        # Only platform prerequisites are replaced; no target script is sent.
        source = (RELEASE / "module-install.sh").read_text().split('case "${1:-}" in')[0]
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp); existing = base / "qdc-web"; existing.mkdir()
            record = existing / "keep.txt"; record.write_text("keep credentials and records")
            source = source.replace("ROOT=/usrdata/qdc-web", "ROOT=" + str(existing)).replace("RUN=/run/qdc-web", "RUN=" + str(base / "run"))
            source += '\npreflight() { :; }\nfresh_check\n'
            result = subprocess.run(["/bin/sh"], input=source, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("首次安装包拒绝覆盖", result.stderr)
            self.assertEqual(record.read_text(), "keep credentials and records")
            self.assertEqual(sorted(p.relative_to(base).as_posix() for p in base.rglob("*")), ["qdc-web", "qdc-web/keep.txt"])

    def test_no_apply_refuses_before_finding_adb(self):
        result = subprocess.run([sys.executable, str(RELEASE / "install.py"), "install", "--adb", "/not-an-adb"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("必须加 --apply", result.stderr)
        self.assertNotIn("未找到 ADB", result.stderr)

    def test_password_saved_privately_before_remote_removal(self):
        class FakeUSB:
            removed = False
            def shell(self, command):
                if "cat " in command: return "a" * 32
                self.removed = True
                self.assert_saved()
                return ""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            fake = FakeUSB()
            def saved():
                paths = list(directory.glob("initial-password-*.txt"))
                self.assertEqual(len(paths), 1)
                self.assertIn("a" * 32, paths[0].read_text())
                self.assertEqual(stat.S_IMODE(paths[0].stat().st_mode), 0o600)
            fake.assert_saved = saved
            installer.save_password(fake, directory)
            self.assertTrue(fake.removed)

    def test_private_material_and_metadata_excluded(self):
        forbidden = (".DS_Store", "initial-password.txt", "module-oray.json", "credential", "call-history", "notify-settings", "usb-header-probe")
        personal = tuple(value.encode() for value in os.environ.get("QDC_PRIVATE_MARKERS", "").splitlines() if value)
        for file in RELEASE.rglob("*"):
            if file.is_file():
                self.assertNotIn(file.name, forbidden)
                self.assertNotIn("private", file.relative_to(RELEASE).parts)
                self.assertFalse(any(value in file.read_bytes() for value in personal), file.relative_to(RELEASE).as_posix())
        metadata = json.loads((RELEASE / "release.json").read_text())
        self.assertFalse(metadata["containsDeviceCredentials"])
        self.assertFalse(metadata["freshDeviceInstallLiveTested"])
        for file in (RELEASE / "payload").rglob("*"):
            if file.is_file() and file.read_bytes().startswith(b"\x7fELF"):
                content = file.read_bytes()
                self.assertEqual(content[4:6], b"\x01\x01")
                self.assertEqual(int.from_bytes(content[18:20], "little"), 40)
        self.assertIn(b"0.3.43", (RELEASE / "payload/www/cgi-bin/api").read_bytes())


if __name__ == "__main__":
    unittest.main()
