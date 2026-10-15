#!/usr/bin/env python3
"""Export only reviewed source files; never copy an entire development tree."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import sys
import zipfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("source_files", Path(__file__).with_name("source-files.py"))
sources = importlib.util.module_from_spec(spec); spec.loader.exec_module(sources)

parser = argparse.ArgumentParser(description="导出模块项目源码，不包含安装包或本地配置")
parser.add_argument("--output", type=Path, required=True, help="源码目录之外的输出文件夹")
args = parser.parse_args()
output = args.output.resolve()
if output == sources.ROOT or sources.ROOT in output.parents:
    parser.error("输出目录必须位于源码目录之外。")
files, findings = sources.inspect()
if findings:
    raise SystemExit("源码检查未通过，请先执行 check-source.py 查看问题类别。")
version = "0.3.43"
name = "qdc507-module-web-source-" + version
destination = output / name
archive = output / (name + ".zip")
if destination.exists() or archive.exists():
    raise SystemExit("输出中已有同版本源码，未覆盖。")
output.mkdir(parents=True, exist_ok=True)
destination.mkdir()
for file in files:
    target = destination / file.relative_to(sources.ROOT)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(file, target)
    target.chmod(0o755 if file.stat().st_mode & 0o111 else 0o644)
manifest = "".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(destination).as_posix() + "\n" for p in sorted(destination.rglob("*")) if p.is_file())
(destination / "SOURCE_SHA256SUMS").write_text(manifest, encoding="utf-8")
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zip_file:
    for file in sorted(destination.rglob("*")):
        if file.is_file():
            info = zipfile.ZipInfo(name + "/" + file.relative_to(destination).as_posix(), date_time=(2026, 10, 15, 0, 0, 0))
            info.create_system = 3; info.external_attr = (file.stat().st_mode & 0xffff) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zip_file.writestr(info, file.read_bytes())
(output / (name + ".sha256")).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n", encoding="utf-8")
print("源码目录：" + str(destination))
print("源码 ZIP：" + str(archive))
print("源码文件：" + str(len(files)) + "；ZIP 字节：" + str(archive.stat().st_size))
