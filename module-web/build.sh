#!/bin/sh
# Offline native verification and ARMv7 Linux module package build.
set -eu
umask 022
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
MODE=${1:-all}
case "$MODE" in
    all|--all|--native-only|--crypto-only) ;;
    *) printf '%s\n' 'usage: build.sh [--all|--native-only|--crypto-only]' >&2; exit 2 ;;
esac
ZIG=${ZIG:-"$ROOT/.tools/zig-macos-aarch64-0.13.0/zig"}
NATIVE_CC=${NATIVE_CC:-clang}
MBED="$ROOT/vendor/mbedtls"
mkdir -p "$ROOT/build"
export ZIG_GLOBAL_CACHE_DIR="$ROOT/build/zig-global-cache"
export ZIG_LOCAL_CACHE_DIR="$ROOT/build/zig-local-cache"
export SOURCE_DATE_EPOCH=1720000000

[ -f "$MBED/provenance.json" ] || { printf '%s\n' 'Run setup-toolchain.sh first.' >&2; exit 1; }
python3 - "$MBED" <<'PY'
import hashlib,json,pathlib,sys
root=pathlib.Path(sys.argv[1])
record=json.loads((root/'provenance.json').read_text())
for name,expected in record['files'].items():
    if hashlib.sha256((root/name).read_bytes()).hexdigest()!=expected:
        raise SystemExit('Modified vendored source: '+name)
PY

crypto_build()
{
    "$NATIVE_CC" -std=c11 -O2 -Wall -Wextra -Werror -fstack-protector-strong \
        -I"$ROOT" -I"$MBED/include" '-DMBEDTLS_CONFIG_FILE="mbedtls_config.h"' \
        "$ROOT/test_corecrypto.c" "$MBED/library/md.c" \
        "$MBED/library/sha256.c" "$MBED/library/pkcs5.c" \
        "$MBED/library/platform_util.c" -o "$ROOT/build/test-corecrypto"
    python3 "$ROOT/test_crypto.py" "$ROOT/build/test-corecrypto"
}
crypto_build
[ "$MODE" != --crypto-only ] || exit 0
[ -f "$ROOT/api.c" ] || { printf '%s\n' 'api.c is not ready yet; crypto verification completed.' >&2; exit 1; }

"$NATIVE_CC" -std=c11 -O2 -Wall -Wextra -fstack-protector-strong \
    -I"$ROOT" -I"$MBED/include" '-DMBEDTLS_CONFIG_FILE="mbedtls_config.h"' \
    -DHISTORY_TEST -DORAY_TEST -DTRAFFIC_TEST -DNOTIFY_TEST -DEVENTS_TEST -DDATA_TEST "$ROOT/api.c" "$ROOT/traffic.c" "$ROOT/notify.c" "$ROOT/call-history.c" "$MBED/library/md.c" "$MBED/library/sha256.c" \
    "$MBED/library/pkcs5.c" "$MBED/library/platform_util.c" \
    -o "$ROOT/build/api-native"
"$NATIVE_CC" -std=c11 -O2 -Wall -Wextra -Werror -DCOMM_TEST \
    -DHISTORY_TEST -DNOTIFY_TEST "$ROOT/comm.c" "$ROOT/notify.c" "$ROOT/call-history.c" -o "$ROOT/build/comm-native"
"$NATIVE_CC" -std=c11 -O2 -Wall -Wextra -Werror -DAUDIO_TEST \
    -I"$ROOT" -I"$MBED/include" '-DMBEDTLS_CONFIG_FILE="audio_config.h"' \
    "$ROOT/audio.c" "$MBED/library/sha1.c" "$MBED/library/base64.c" \
    "$MBED/library/constant_time.c" "$MBED/library/platform_util.c" -o "$ROOT/build/audio-native"
"$NATIVE_CC" -std=c11 -O2 -Wall -Wextra -Werror "$ROOT/audio-pcm.c" \
    -lpthread -ldl -o "$ROOT/build/pcm-native"
printf 'Native CGI: %s\n' "$ROOT/build/api-native"
if [ "$MODE" = --native-only ]; then sh "$ROOT/build-esim-management.sh" --native-only; exit 0; fi
[ -x "$ZIG" ] && [ "$("$ZIG" version)" = 0.13.0 ] || { printf '%s\n' 'Pinned Zig 0.13.0 is unavailable; run setup-toolchain.sh.' >&2; exit 1; }

mkdir -p "$ROOT/dist/www/bin" "$ROOT/dist/www/cgi-bin" "$ROOT/dist/licenses"
WEB_ROOT=${WEB_ROOT:-"$ROOT/static"}
if [ -d "$WEB_ROOT" ]; then
    python3 "$ROOT/stage-public-assets.py" --source "$WEB_ROOT" --dest "$ROOT/dist/www"
fi
python3 "$ROOT/prepare-web-assets.py"
cp "$MBED/LICENSE" "$ROOT/dist/licenses/MbedTLS-LICENSE.txt"
# arm-linux-musleabi selects the portable ARM EABI soft-float ABI used by the
# module.  Do not also pass -mcpu=cortex_a7 -mfloat-abi=soft: Zig 0.13's LLVM
# backend aborts for that combination on macOS hosts.
"$ZIG" cc -target arm-linux-musleabi \
    -std=c11 -O2 -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -ffunction-sections -fdata-sections -ffile-prefix-map="$ROOT"=/src/qdc-web \
    -I"$ROOT" -I"$MBED/include" '-DMBEDTLS_CONFIG_FILE="mbedtls_config.h"' \
    "$ROOT/api.c" "$ROOT/traffic.c" "$ROOT/notify.c" "$ROOT/call-history.c" "$MBED/library/md.c" "$MBED/library/sha256.c" \
    "$MBED/library/pkcs5.c" "$MBED/library/platform_util.c" \
    -static -no-pie -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now,--build-id=sha1 \
    -s -o "$ROOT/dist/www/cgi-bin/api"
# CCI libraries belong to the module and use glibc, so this small helper is
# dynamic ARM EABI / glibc 2.17. Never bundle proprietary vendor libraries.
# Use system libc routines (no exported compiler-rt interposition) and lazy
# binding for compatibility with the module's glibc 2.22 ARM loader.
"$ZIG" build-exe -target arm-linux-gnueabi.2.17 -O ReleaseSmall \
    -lc -ldl -fno-compiler-rt -fPIE -fstrip -fstack-protector \
    -z relro -z lazy -z max-page-size=4096 -z common-page-size=4096 \
    --gc-sections -femit-bin="$ROOT/dist/www/cgi-bin/qdc-comm" \
    -cflags -std=c11 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -ffile-prefix-map="$ROOT"=/src/qdc-web \
    -- "$ROOT/comm.c" "$ROOT/notify.c" "$ROOT/call-history.c"
"$ZIG" cc -target arm-linux-musleabi \
    -std=c11 -O2 -Wall -Wextra -Werror -fstack-protector-strong \
    -ffunction-sections -fdata-sections -ffile-prefix-map="$ROOT"=/src/qdc-web \
    -I"$ROOT" -I"$MBED/include" '-DMBEDTLS_CONFIG_FILE="audio_config.h"' \
    "$ROOT/audio.c" "$MBED/library/sha1.c" "$MBED/library/base64.c" \
    "$MBED/library/constant_time.c" "$MBED/library/platform_util.c" \
    -static -no-pie -Wl,--gc-sections,-z,noexecstack,-z,relro,-z,now -s \
    -o "$ROOT/dist/www/bin/qdc-audio"
# libql_lib_audio loads vendor liblog, which imports executable main.
# All bridge functions except main are static; audit rejects libc interposition.
"$ZIG" build-exe -target arm-linux-gnueabi.2.17 -mcpu=cortex_a7 -O ReleaseSmall \
    -lc -ldl -lpthread -fno-compiler-rt -fPIE -fstrip -fstack-protector -rdynamic \
    -z relro -z lazy -z max-page-size=4096 -z common-page-size=4096 \
    --gc-sections -femit-bin="$ROOT/dist/www/bin/qdc-pcm" \
    -cflags -std=c11 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -ffile-prefix-map="$ROOT"=/src/qdc-web \
    -- "$ROOT/audio-pcm.c"
sh "$ROOT/build-esim-management.sh"
(
    cd "$ROOT/dist/www"
    shasum -a 256 index.html app.css app.js events.js sms-codec.js communications.js call-audio.js audio.js pcm-worklet.js bin/qdc-audio bin/qdc-pcm bin/qdc-esim cgi-bin/api cgi-bin/qdc-comm *.gz > ../www.sha256
)
python3 "$ROOT/audit_binary.py" "$ROOT/dist/www/cgi-bin/api" "$ROOT/dist" "$ROOT/build/arm-audit.json"
python3 "$ROOT/audit_binary.py" "$ROOT/dist/www/cgi-bin/qdc-comm" "$ROOT/dist" "$ROOT/build/comm-audit.json" --dynamic
printf 'Static module CGI: %s\n' "$ROOT/dist/www/cgi-bin/api"

python3 "$ROOT/audit_binary.py" "$ROOT/dist/www/bin/qdc-audio" "$ROOT/dist" "$ROOT/build/audio-audit.json"
python3 "$ROOT/audit_binary.py" "$ROOT/dist/www/bin/qdc-pcm" "$ROOT/dist" "$ROOT/build/pcm-audit.json" --dynamic
python3 "$ROOT/audit_binary.py" "$ROOT/dist/www/bin/qdc-esim" "$ROOT/dist" "$ROOT/build/esim-worker-audit.json" --dynamic
python3 "$ROOT/audit_binary.py" "$ROOT/dist/lib/libqdc-euicc.so" "$ROOT/dist" "$ROOT/build/esim-library-audit.json" --esim-library
