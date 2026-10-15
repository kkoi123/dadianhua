#!/bin/sh
# Run in a directory containing the extracted third-party source archive.
# Use official Zig 0.13.0; only the replaceable library is rebuilt.
set -eu
ZIG=${ZIG:-zig}
[ "$("$ZIG" version)" = 0.13.0 ] || { echo '需要 Zig 0.13.0。' >&2; exit 1; }
LPAC=vendor/lpac
CJSON=vendor/cjson
[ -f "$LPAC/euicc/es10b.c" ] && [ -f "$CJSON/cjson/cJSON.c" ]
"$ZIG" build-lib --name qdc-euicc -target arm-linux-gnueabi.2.17 -O ReleaseSmall -dynamic \
    -lc -lm -fno-compiler-rt -fPIC -fstrip -fstack-protector \
    -z relro -z lazy -z max-page-size=4096 -z common-page-size=4096 \
    -femit-bin=libqdc-euicc.so -I"$LPAC" -I"$LPAC/cjson-ext" -I"$CJSON" \
    -cflags -std=gnu11 -D_DEFAULT_SOURCE -DCJSON_NESTING_LIMIT=32 -- \
    "$LPAC"/euicc/*.c "$LPAC/cjson-ext/cjson-ext/cJSON_ex.c" "$CJSON/cjson/cJSON.c"
echo '已生成 libqdc-euicc.so。'
