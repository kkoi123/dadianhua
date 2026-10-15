#!/usr/bin/env python3
"""Inspect source selection only. No network, ADB, SSH, deletion or secret output."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("source_files", Path(__file__).with_name("source-files.py"))
sources = importlib.util.module_from_spec(spec); spec.loader.exec_module(sources)

parser = argparse.ArgumentParser(description="检查准备上传的源码；只输出问题类别与文件名")
parser.add_argument("--private-value-file", type=Path, action="append", default=[], help="额外比对的本地密码/令牌文件，不会复制或打印其内容")
args = parser.parse_args()
private_values = [file.read_bytes().strip() for file in args.private_value_file]
files, findings = sources.inspect(private_values=private_values)
print(json.dumps({"sourceFiles": len(files), "sourceBytes": sum(p.stat().st_size for p in files), "findings": findings, "hardwareAccess": False}, ensure_ascii=False, indent=2))
sys.exit(bool(findings))
