#!/bin/sh
# Build pinned libeuicc as a replaceable shared library; no device access.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ZIG=${ZIG:-"$ROOT/.tools/zig-macos-aarch64-0.13.0/zig"}
LPAC=$ROOT/vendor/lpac
CJSON=$ROOT/vendor/cjson
python3 - "$LPAC" "$CJSON" <<'PY'
import hashlib,json,pathlib,sys
for name in sys.argv[1:]:
    root=pathlib.Path(name);record=json.loads((root/'provenance.json').read_text())
    for file,expected in {**record['files'],**record.get('localPatches',{})}.items():
        if hashlib.sha256((root/file).read_bytes()).hexdigest()!=expected:
            raise SystemExit('Modified eSIM source: '+file)
PY
mkdir -p "$ROOT/build/esim-native" "$ROOT/dist/lib" "$ROOT/dist/www/bin" "$ROOT/dist/licenses"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/build/zig-global-cache"
export ZIG_LOCAL_CACHE_DIR="$ROOT/build/zig-local-cache"
SOURCES=$(find "$LPAC/euicc" -name '*.c' -maxdepth 1 | sort)
clang -shared -fPIC -std=gnu11 -D_DEFAULT_SOURCE -O2 -DCJSON_NESTING_LIMIT=32 \
    -I"$LPAC" -I"$LPAC/cjson-ext" -I"$CJSON" \
    $SOURCES "$LPAC/cjson-ext/cjson-ext/cJSON_ex.c" "$CJSON/cjson/cJSON.c" \
    -Wl,-install_name,@rpath/libqdc-euicc.dylib -o "$ROOT/build/esim-native/libqdc-euicc.dylib"
clang -std=c11 -O2 -Wall -Wextra -Werror -DCOMM_TEST -DHISTORY_TEST -DNOTIFY_TEST \
    -I"$LPAC" -I"$CJSON" "$ROOT/esim-worker.c" "$ROOT/notify.c" "$ROOT/call-history.c" \
    -L"$ROOT/build/esim-native" -lqdc-euicc -Wl,-rpath,"$ROOT/build/esim-native" \
    -o "$ROOT/build/esim-native/qdc-esim"
[ "${1:-}" != --native-only ] || exit 0
"$ZIG" build-lib --name qdc-euicc -target arm-linux-gnueabi.2.17 -O ReleaseSmall -dynamic \
    -lc -lm -fno-compiler-rt -fPIC -fstrip -fstack-protector \
    -z relro -z lazy -z max-page-size=4096 -z common-page-size=4096 \
    -femit-bin="$ROOT/dist/lib/libqdc-euicc.so" \
    -I"$LPAC" -I"$LPAC/cjson-ext" -I"$CJSON" \
    -cflags -std=gnu11 -D_DEFAULT_SOURCE -DCJSON_NESTING_LIMIT=32 -- \
    $SOURCES "$LPAC/cjson-ext/cjson-ext/cJSON_ex.c" "$CJSON/cjson/cJSON.c"
"$ZIG" build-exe -target arm-linux-gnueabi.2.17 -O ReleaseSmall \
    -lc -ldl -fno-compiler-rt -fPIE -fstrip -fstack-protector \
    -L"$ROOT/dist/lib" -lqdc-euicc -rpath /usrdata/qdc-web/lib \
    -z relro -z lazy -z max-page-size=4096 -z common-page-size=4096 \
    --gc-sections -femit-bin="$ROOT/dist/www/bin/qdc-esim" \
    -I"$LPAC" -I"$CJSON" -cflags -std=c11 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -- "$ROOT/esim-worker.c" "$ROOT/notify.c" "$ROOT/call-history.c"
cp "$LPAC/euicc/LICENSE" "$ROOT/dist/licenses/libeuicc-LGPL-2.1.txt"
cp "$LPAC/LICENSES/MIT.txt" "$ROOT/dist/licenses/lpac-cjson-ext-MIT.txt"
cp "$CJSON/LICENSE" "$ROOT/dist/licenses/cJSON-MIT.txt"
