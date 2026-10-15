#!/bin/sh
# Offline build using the prepared, pinned kernel and AOSP compiler caches.
set -eu
SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
TOOLS_INPUT=${QDC_USB_TOOLS:-"$SRC/../.tools/usb-compat"}
[ -d "$TOOLS_INPUT" ] || { echo '需要准备内核与 GCC 依赖目录；使用 QDC_USB_TOOLS 指定。' >&2; exit 1; }
TOOLS=$(CDPATH= cd -- "$TOOLS_INPUT" && pwd -P)
IMAGE=${QDC_USB_BUILD_IMAGE:-qdc-usb-compat-builder:20261010}
python3 - "$TOOLS" <<'PY'
import hashlib,pathlib,sys
root=pathlib.Path(sys.argv[1])
for name,wanted in {
 'kernel.tar.gz':'baaea65a96b0151fcc96e83c34415bc10b8443b0907ecb0769f25b0d04d809ee',
 'gcc.tar.gz':'2a0d82ce13e3c6c37c19e7f4c15b835e604a100a1a6f6b70e2b879a02a80d12d',
}.items():
    if hashlib.sha256((root/name).read_bytes()).hexdigest()!=wanted:
        raise SystemExit('Build input changed: '+name)
PY
python3 - "$SRC/module.config" <<'PY'
from pathlib import Path
import hashlib,sys
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest()!='6c08438c9441a6e9a29dc7c4f431ac8be015813861a5cdc883d257991e7f1fc5':
    raise SystemExit('Kernel configuration changed')
PY
docker run --rm --platform linux/amd64 --entrypoint /bin/sh \
  -v "$TOOLS:/work" -v "$SRC:/component" "$IMAGE" -c '
set -eu
source=/work/quectel_eg25_kernel-82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f
mkdir -p /work/kernel-build
cp /component/module.config /work/kernel-build/.config
build() {
  make -C "$source" O=/work/kernel-build ARCH=arm \
    CROSS_COMPILE=/work/gcc/bin/arm-linux-androideabi- \
    CC=/work/gcc/bin/real-arm-linux-androideabi-gcc \
    HOSTCFLAGS="-O2 -fcommon" "$@"
}
build olddefconfig
build -j2 modules_prepare
build -j2 M=/component modules
/work/gcc/bin/arm-linux-androideabi-nm -u /component/qdc_usb_compat.ko > /work/imports.txt
/work/gcc/bin/arm-linux-androideabi-readelf -r /component/qdc_usb_compat.ko > /work/relocations.txt
'
mkdir -p "$SRC/../dist/usb-compat"
cp "$SRC/qdc_usb_compat.ko" "$SRC/trial-remote.sh" "$SRC/../dist/usb-compat/"
(cd "$SRC/../dist/usb-compat" && shasum -a 256 qdc_usb_compat.ko trial-remote.sh > sha256sums)
printf '%s\n' 'Built experimental component only; no live module writes.'
