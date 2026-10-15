#!/bin/sh
set -eu
umask 077
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
cd "$ROOT"
ZIG=${ZIG:-"$ROOT/.tools/zig-macos-aarch64-0.13.0/zig"}
MODULE_VOICE_DIR=${MODULE_VOICE_DIR:-"$ROOT/build/dependencies/voice"}
mkdir -p build dist/standalone/voice
export ZIG_GLOBAL_CACHE_DIR="$ROOT/build/zig-global-cache"
export ZIG_LOCAL_CACHE_DIR="$ROOT/build/zig-local-cache"
[ -x "$ZIG" ] && [ "$("$ZIG" version)" = 0.13.0 ] || { echo '需要 Zig 0.13.0；可通过 ZIG 指定路径。' >&2; exit 1; }
python3 - "$MODULE_VOICE_DIR" <<'PY'
from pathlib import Path
import hashlib,sys
directory=Path(sys.argv[1])
for name,wanted in {
    'qdc507_aprv3.ko':'3d82d3dec4f1e323201bba87156df9d41438e08314097353f2607f9117211d4a',
    'qdc507_voice.ko':'ed3821682d5309969a01c764192c83feff9669c61ef237c69475cd1619cf296c',
}.items():
    file=directory/name
    if not file.is_file() or file.is_symlink() or hashlib.sha256(file.read_bytes()).hexdigest()!=wanted:
        raise SystemExit('缺少或不匹配的语音组件：'+name+'；先运行 scripts/import-release-deps.py 或设置 MODULE_VOICE_DIR。')
PY
python3 setup-standalone-client.py
python3 prepare-web-assets.py
clang -std=c11 -O2 -Wall -Wextra -Werror -DENTRY_TEST -DTRAFFIC_TEST -DRELAY_IDLE_MS=500 entry.c traffic.c -o build/entry-native
python3 test_entry.py
clang -std=c11 -O2 -Wall -Wextra -Werror test_network_watch_state.c -o build/test-network-watch-state
build/test-network-watch-state
python3 test_network_reconnect.py
clang -std=c11 -O2 -Wall -Wextra -Werror -DCONFIG_STORE_TEST config-store.c -o build/config-store-native
python3 test_config_store.py
for script in standalone-service.sh standalone-init.sh standalone-boot-control.sh; do sh -n "$script"; done
"$ZIG" cc -target arm-linux-musleabi -std=c11 -O2 \
    -Wall -Wextra -Werror -fstack-protector-strong -static -no-pie -s \
    -Wl,-z,noexecstack,-z,relro,-z,now entry.c traffic.c -o dist/standalone/qdc-entry
"$ZIG" cc -target arm-linux-musleabi -std=c11 -O2 \
    -Wall -Wextra -Werror -fstack-protector-strong -static -no-pie -s \
    -Wl,-z,noexecstack,-z,relro,-z,now network-watch.c -o dist/standalone/qdc-network-watch
"$ZIG" cc -target arm-linux-musleabi -std=c11 -O2 \
    -Wall -Wextra -Werror -fstack-protector-strong -static -no-pie -s \
    -Wl,-z,noexecstack,-z,relro,-z,now config-store.c -o dist/standalone/qdc-config-store
cp standalone-service.sh dist/standalone/service.sh
cp standalone-init.sh dist/standalone/init.sh
cp standalone-boot-control.sh dist/standalone/boot-control.sh
mkdir -p dist/standalone/voice
# Exactly the two existing, previously validated voice drivers, not new ones.
for driver in qdc507_aprv3.ko qdc507_voice.ko; do cp "$MODULE_VOICE_DIR/$driver" "dist/standalone/voice/$driver"; done
chmod 700 dist/standalone/qdc-entry dist/standalone/qdc-network-watch dist/standalone/qdc-config-store dist/standalone/phtunnel dist/standalone/lib/libc.so dist/standalone/*.sh
(cd dist/standalone/voice && shasum -a 256 *.ko > sha256sums)
python3 - <<'PY'
from pathlib import Path
import hashlib,json
root=Path('dist/standalone')
files=sorted(p for p in root.rglob('*') if p.is_file() and p!=root/'sha256sums' and p.name!='.DS_Store' and not p.name.startswith('._'))
(root/'sha256sums').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(root).as_posix()+'\n' for p in files))
print('Standalone files:',len(files),'bytes:',sum(p.stat().st_size for p in files))
PY
