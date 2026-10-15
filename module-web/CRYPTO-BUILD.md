# Module admin build inputs

The module CGI is statically linked for ARMv7 Cortex-A7 Linux using the soft-float EABI5 calling convention. No dynamic library installation is required on the module.

## Pinned official inputs

- Zig 0.13.0, macOS ARM64: https://ziglang.org/download/0.13.0/zig-macos-aarch64-0.13.0.tar.xz
  - SHA-256: `46fae219656545dfaf4dce12fb4e8685cec5b51d721beee9389ab4194d43394c`
  - Published checksum: https://ziglang.org/download/index.json
- Mbed TLS 2.28.10: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-2.28.10/mbedtls-2.28.10.tar.bz2
  - SHA-256: `19e5b81fdac0fe22009b9e2bdcd52d7dcafbf62bc67fc59cf0a76b5b5540d149`
  - Published checksum: https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-2.28.10

`vendor/mbedtls/provenance.json` records each copied source file checksum. Sources are copied unchanged; configuration enables SHA-256, MD/HMAC and PKCS5/PBKDF2. The separate audio daemon configuration enables only SHA-1 and Base64 for the RFC6455 handshake; SHA-1 is never used for password authentication or tickets. The additional unchanged constant-time sources and headers are recorded in the same provenance manifest. TLS, certificate processing, ciphers, big-integer computation and upstream network code are excluded. Mbed TLS code is distributed under its Apache-2.0 option, with the upstream dual-license text included in the install package.

Mbed TLS 2.28.10 is the final, unsupported 2.28 release. This intentionally small subset has no TLS code. Dependency maintenance must track SHA-256, HMAC and PBKDF2 advisories and migrate this subset to a maintained Mbed TLS release before expanding its cryptographic scope.

## Commands

1. `./setup-toolchain.sh` downloads and verifies official tools and vendors the selected source files. Only this command requires internet access.
2. `./build.sh --crypto-only` compiles the native crypto test and compares 3 SHA-256 plus 6 PBKDF2 vectors against Python hashlib, including 600000 rounds, a long password, empty input and a derived key spanning two blocks.
3. `./build.sh --native-only` also compiles the native CGI to `build/api-native` for isolated host-side tests.
4. `./build.sh` builds native tests and the static module CGI to `dist/www/cgi-bin/api`, copies `static` and the license into `dist`, then audits ELF compatibility and limits the install package to 1 MiB.

The compiler cache remains under `build`. Build tools, source code and native test binaries are not included in `dist`. `WEB_ROOT` can override the frontend source directory; `NATIVE_CC` selects the host C compiler. The ARM audit is saved to `build/arm-audit.json`. None of these commands executes target code, changes module settings or installs to a module.
