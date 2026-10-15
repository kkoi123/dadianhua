#!/usr/bin/env python3
"""Import only three hash-pinned kernel dependencies; never execute an archive."""
import argparse
import hashlib
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = {
    "qdc507_aprv3.ko": ("build/dependencies/voice", "3d82d3dec4f1e323201bba87156df9d41438e08314097353f2607f9117211d4a"),
    "qdc507_voice.ko": ("build/dependencies/voice", "ed3821682d5309969a01c764192c83feff9669c61ef237c69475cd1619cf296c"),
    "qdc_usb_compat.ko": ("dist/usb-compat", "1c87ea075b4c622017f7c8a8273fb1743fe4f90d04ac7dbc059ad55a712cb581"),
}


def read_components(archive_path):
    result = {}
    with zipfile.ZipFile(archive_path) as archive:
        for name, (relative, digest) in EXPECTED.items():
            entries = [entry for entry in archive.infolist() if Path(entry.filename).name == name]
            if len(entries) != 1 or entries[0].file_size > 2 * 1024 * 1024:
                raise ValueError("组件缺失、重复或过大：" + name)
            data = archive.read(entries[0])
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError("组件校验失败：" + name)
            result[ROOT / relative / name] = data
    return result


def main():
    parser = argparse.ArgumentParser(description="从已有安装 ZIP 导入匹配的驱动依赖，只写电脑构建目录")
    parser.add_argument("--archive", type=Path, required=True)
    args = parser.parse_args()
    # Validate everything before writing any file. No extractall or executable.
    components = read_components(args.archive)
    for file in components:
        if file.is_symlink() or any(p.is_symlink() for p in file.parents):
            raise ValueError("目标路径存在符号链接。")
    for file, data in components.items():
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        file.chmod(0o644)
    print("已核对并导入 3 个驱动依赖；未连接硬件，未执行安装包内程序。")


if __name__ == "__main__":
    main()
