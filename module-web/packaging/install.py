#!/usr/bin/env python3
"""USB ADB installer for the clean, first-install module release."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shlex
import shutil
import subprocess
import sys

BASE = Path(__file__).resolve().parent
REMOTE = "/usrdata/qdc-web"
ACTIONS = ("check", "install", "start", "set-domain", "enable-startup", "init-oray", "save-password")


def verify_package(base):
    manifest = base / "SHA256SUMS"
    expected = set()
    for line in manifest.read_text(encoding="utf-8").splitlines():
        digest, name = line.split("  ", 1)
        path = PurePosixPath(name)
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or not name or "\\" in name or ":" in name or path.as_posix() != name or path.is_absolute() or any(p in (".", "..") for p in path.parts) or name in expected:
            raise ValueError("文件校验清单无效。")
        file = base / name
        if any(p.is_symlink() for p in (file, *file.parents)) or not file.is_file():
            raise ValueError("安装文件缺失或为符号链接：" + name)
        if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            raise ValueError("安装文件校验失败：" + name)
        expected.add(name)
    actual = {p.relative_to(base).as_posix() for p in base.rglob("*") if p.is_file() and p != manifest and p.name != ".DS_Store" and not p.name.startswith("._")}
    # Python importing this helper may create __pycache__ locally. It is never
    # transferred: the installer pushes only the explicitly named payload.
    actual = {p for p in actual if "__pycache__" not in PurePosixPath(p).parts and not re.fullmatch(r"initial-password-[0-9a-f]{8}\.txt", p)}
    if actual != expected:
        raise ValueError("安装包包含清单以外的文件，或清单不完整。")
    return len(expected)


def select_device(output, serial=None):
    devices = [line.split()[0] for line in output.splitlines()[1:] if len(line.split()) >= 2 and line.split()[1] == "device" and ":" not in line.split()[0]]
    if serial:
        if serial not in devices:
            raise ValueError("指定的 USB 模块未连接或未授权。")
        return serial
    if len(devices) != 1:
        raise ValueError("请只连接一个可用的 USB ADB 模块；多设备时用 --serial 指定。")
    return devices[0]


class USB:
    def __init__(self, adb, serial):
        self.command = [adb, "-s", serial]

    def run(self, *args, timeout=180):
        result = subprocess.run([*self.command, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        if result.returncode:
            raise RuntimeError("USB ADB 操作失败，请检查连接。")
        return result.stdout.decode("utf-8", errors="replace").replace("\r", "")

    def shell(self, command, timeout=180):
        marker = "__QDC_RELEASE_" + secrets.token_hex(8)
        wrapped = "(\n" + command + "\n); qdc_result=$?; printf '\\n" + marker + "%s\\n' \"$qdc_result\""
        result = self.run("shell", wrapped, timeout=timeout)
        body, found, code = result.rpartition("\n" + marker)
        if not found or code.strip() != "0":
            raise RuntimeError(body.strip() if found else "模块未返回完整执行结果，请检查 USB 连接。")
        return body.strip()


def save_password(usb, output_dir):
    password = usb.shell("set -e; test -f " + REMOTE + "/private/initial-password.txt; cat " + REMOTE + "/private/initial-password.txt")
    if not re.fullmatch(r"[0-9a-f]{32}", password):
        raise RuntimeError("无法读取首次生成的密码；现有登录密码未修改。")
    filename = output_dir / ("initial-password-" + secrets.token_hex(4) + ".txt")
    fd = os.open(filename, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as file:
        file.write("登录地址：http://192.168.225.1/\n用户名：admin\n密码：" + password + "\n登录后请在设置中修改密码。\n")
        file.flush()
        os.fsync(file.fileno())
    usb.shell("set -e; rm " + REMOTE + "/private/initial-password.txt; sync")
    print("管理员密码已单独保存到：" + str(filename))


def main():
    parser = argparse.ArgumentParser(description="QDC507 模块网页安装工具（电脑只负责 USB 传输）")
    parser.add_argument("action", choices=ACTIONS, nargs="?", default="check")
    parser.add_argument("--apply", action="store_true", help="明确执行模块写入/服务启动")
    parser.add_argument("--serial", help="指定 USB ADB 设备编号")
    parser.add_argument("--adb", default="adb", help="ADB 可执行文件路径")
    parser.add_argument("--origin", help="自己的 HTTPS 公网域名，不带路径或端口")
    args = parser.parse_args()
    if args.action != "check" and not args.apply:
        parser.error("执行操作必须加 --apply；仅预检使用 check。")
    if args.origin and not re.fullmatch(r"https://[A-Za-z0-9][A-Za-z0-9.-]{0,252}", args.origin):
        parser.error("--origin 必须是 HTTPS 域名，不含路径或端口。")
    if args.action == "set-domain" and not args.origin:
        parser.error("set-domain 需要 --origin。")
    if args.origin and args.action not in ("install", "set-domain"):
        parser.error("--origin 仅用于 install 或 set-domain。")
    print("安装包校验通过：", verify_package(BASE), "个文件。")
    adb = shutil.which(args.adb)
    if not adb:
        raise RuntimeError("未找到 ADB。请先安装 Android platform-tools，或用 --adb 指定路径。")
    output = subprocess.check_output([adb, "devices"], text=True, timeout=20)
    usb = USB(adb, select_device(output, args.serial))
    if args.action == "save-password":
        save_password(usb, BASE)
        return
    script = (BASE / "module-install.sh").read_text(encoding="utf-8")
    if args.action == "check":
        print(usb.shell("sh -c " + shlex.quote(script) + " release check"))
        return
    if args.action == "install":
        # Read-only checks happen before uploading a single byte.
        print(usb.shell("sh -c " + shlex.quote(script) + " release check"))
    stage = "/tmp/qdc-release-" + secrets.token_hex(16)
    usb.shell("set -e; mkdir " + shlex.quote(stage) + "; chmod 700 " + shlex.quote(stage))
    try:
        usb.run("push", str(BASE / "module-install.sh"), stage + "/module-install.sh")
        if args.action == "install":
            usb.run("push", str(BASE / "payload"), stage + "/payload")
        command = "sh " + shlex.quote(stage + "/module-install.sh") + " " + args.action + " --apply"
        if args.action in ("install", "set-domain"):
            command += " " + shlex.quote(args.origin or "https://example.invalid")
        print(usb.shell(command, timeout=240))
        if args.action == "install":
            save_password(usb, BASE)
            print("打开 http://192.168.225.1/ 登录；设置中版本应为 0.3.43。")
    finally:
        usb.shell("rm -rf " + shlex.quote(stage))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("未完成：" + str(error), file=sys.stderr)
        print("若已安装但密码未保存，可运行 save-password --apply；不要重复首次安装。", file=sys.stderr)
        sys.exit(1)
