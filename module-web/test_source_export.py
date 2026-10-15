#!/usr/bin/env python3
"""Check publication boundaries with temporary fake data, never device data."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("sources", ROOT / "scripts/source-files.py")
sources = importlib.util.module_from_spec(spec); spec.loader.exec_module(sources)
spec = importlib.util.spec_from_file_location("deps", ROOT / "scripts/import-release-deps.py")
deps = importlib.util.module_from_spec(spec); spec.loader.exec_module(deps)


class SourceExportTests(unittest.TestCase):
    def test_generated_and_private_files_never_selected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            for relative in ("build/old.c", ".tools/cache.h", "private/credential", "initial-password.txt", ".pairing-token", ".DS_Store", "old.zip", ".env", "module.c"):
                file = root / relative; file.parent.mkdir(parents=True, exist_ok=True); file.write_text("fixture")
            files, findings = sources.inspect(root)
            self.assertEqual([p.relative_to(root).as_posix() for p in files], ["module.c"])
            self.assertEqual(findings, [])

    def test_personal_path_or_supplied_secret_reported_without_values(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / "module.c").write_text("/" + "Users/fixture-user/project secret-fixture-123")
            _, findings = sources.inspect(root, [b"secret-fixture-123"])
            self.assertEqual({f["category"] for f in findings}, {"personal-home-path", "known-private-value"})
            self.assertNotIn("secret-fixture-123", str(findings))

    def test_unknown_records_and_symlinks_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve(); (root / "records.json").write_text("{}")
            (root / "record.sqlite").write_bytes(b"fixture")
            (root / "source.c").symlink_to(root / "records.json")
            _, findings = sources.inspect(root)
            self.assertEqual({f["category"] for f in findings}, {"unexpected-json-settings-or-records", "unrecognized-source-file", "symlink"})

    def test_invalid_dependency_archive_never_writes_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            archive = Path(temp) / "not-a-release.zip"
            with zipfile.ZipFile(archive, "w") as zipped:
                zipped.writestr("../../qdc507_aprv3.ko", b"not a driver")
            with self.assertRaises(ValueError): deps.read_components(archive)


if __name__ == "__main__":
    unittest.main()
