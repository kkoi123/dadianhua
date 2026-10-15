#!/bin/sh
# Download pinned official build inputs; never contact a device.
set -eu
umask 022
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
TOOLS="$ROOT/.tools"
mkdir -p "$TOOLS"

fetch_checked()
{
    url=$1
    name=$2
    expected=$3
    file="$TOOLS/$name"
    if [ ! -f "$file" ] || [ "$(shasum -a 256 "$file" | awk '{print $1}')" != "$expected" ]; then
        curl --fail --location --silent --show-error --retry 2 --connect-timeout 20 --max-time 900 "$url" -o "$file.download"
        actual=$(shasum -a 256 "$file.download" | awk '{print $1}')
        [ "$actual" = "$expected" ] || { printf '%s\n' "Checksum mismatch for $name" >&2; exit 1; }
        mv "$file.download" "$file"
    fi
}

fetch_checked \
    https://ziglang.org/download/0.13.0/zig-macos-aarch64-0.13.0.tar.xz \
    zig-macos-aarch64-0.13.0.tar.xz \
    46fae219656545dfaf4dce12fb4e8685cec5b51d721beee9389ab4194d43394c
fetch_checked \
    https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-2.28.10/mbedtls-2.28.10.tar.bz2 \
    mbedtls-2.28.10.tar.bz2 \
    19e5b81fdac0fe22009b9e2bdcd52d7dcafbf62bc67fc59cf0a76b5b5540d149

if [ ! -x "$TOOLS/zig-macos-aarch64-0.13.0/zig" ]; then
    tar -xf "$TOOLS/zig-macos-aarch64-0.13.0.tar.xz" -C "$TOOLS"
fi
[ "$("$TOOLS/zig-macos-aarch64-0.13.0/zig" version)" = 0.13.0 ]
python3 "$ROOT/vendor-subset.py" "$TOOLS/mbedtls-2.28.10.tar.bz2"
printf '%s\n' 'Pinned official build inputs are ready.'
