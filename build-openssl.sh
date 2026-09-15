#!/usr/bin/env bash
# build-openssl.sh — build static OpenSSL for a target triple with zig, into
# out/openssl-$SUFFIX. Shared by client/build.sh and server/build.sh (and every
# release.sh target) so presets and the cache location stay in one place.
# Idempotent: if out/openssl-$SUFFIX already has the libs it does nothing.
#
#   ./build-openssl.sh                        native host build
#   ./build-openssl.sh x86_64-windows-gnu     Windows (mingw64 via zig)
#   ./build-openssl.sh x86_64-linux-musl      static Linux
#   ./build-openssl.sh aarch64-macos          cross macOS (needs Rosetta for conftests)
#
# Needs curl + tar + perl + make. First build takes a few minutes (OpenSSL is
# large); the result is cached by directory existence. rm -rf out/openssl-*
# out/.openssl-src to force a rebuild (also after a zig upgrade).
#
# The default provider's dispatch tables are trimmed by cxx_modules/openssl-trim.patch
# (applied after extraction; its hash is part of the cache stamp, so a changed
# patch rebuilds every target automatically). Kept: AES-CBC/GCM, ChaCha20-Poly1305,
# SHA-1/2, HMAC, EC/ECDHE/ECDSA, HKDF/TLS-PRF/PBKDF2, CTR-DRBG. Dropped: RSA, DSA,
# DH, Ed/X25519, SHA-3, ARIA/Camellia/DES/RC4/SM2/3/4, ML-KEM/ML-DSA/SLH-DSA, legacy
# provider, key encoders.
set -e
cd "$(dirname "$0")"

VER="${OPENSSL_VER:-3.5.4}"
TARGET="$1"
TARGETFLAG=""
SUFFIX="host"
if [ -n "$TARGET" ]; then
  TARGETFLAG="-target $TARGET"
  SUFFIX="$TARGET"
fi

PRESET=""
ASMFLAG=""
case "$TARGET" in
  "")
    case "$(uname -m)" in
      arm64) PRESET="darwin64-arm64-cc" ;;
      x86_64) PRESET="darwin64-x86_64-cc" ;;
      *) echo "[!] unsupported host arch: $(uname -m)"; exit 1 ;;
    esac
    ;;
  x86_64-macos) PRESET="darwin64-x86_64-cc" ;;
  aarch64-macos) PRESET="darwin64-arm64-cc" ;;
  x86_64-linux-musl|aarch64-linux-musl) PRESET="linux-generic64" ;;
  x86_64-windows-gnu) PRESET="mingw64"; ASMFLAG="no-asm" ;;
  *) echo "[!] unsupported target: $TARGET"; exit 1 ;;
esac

DEST="$(pwd)/out/openssl-$SUFFIX"
SRCDIR="$(pwd)/out/.openssl-src"
BUILDDIR="$SRCDIR/build-$SUFFIX"

FLAGS="no-shared no-tests no-apps no-docs no-engine no-dso no-deprecated no-comp no-sock no-dtls \
no-cmp no-des no-sm3 no-legacy no-blake2 no-cmac no-jitter \
-ffunction-sections -fdata-sections $ASMFLAG"
PATCHSUM=$(shasum -a 256 cxx_modules/openssl-trim.patch | awk '{print $1}')
STAMP="$PRESET|$VER|$FLAGS|$PATCHSUM"

if [ -f "$DEST/lib/libcrypto.a" ] && [ -f "$DEST/lib/libssl.a" ] \
   && [ -f "$DEST/include/openssl/ssl.h" ] \
   && [ "$(cat "$DEST/.build-flags" 2>/dev/null)" = "$STAMP" ]; then
  echo "[=] openssl-$SUFFIX already built"
  exit 0
fi

mkdir -p "$SRCDIR"
if [ ! -d "$SRCDIR/openssl-$VER" ]; then
  echo "[*] downloading openssl $VER ..."
  curl -fsSL -o "$SRCDIR/openssl-$VER.tar.gz" \
    "https://github.com/openssl/openssl/releases/download/openssl-$VER/openssl-$VER.tar.gz"
  tar xzf "$SRCDIR/openssl-$VER.tar.gz" -C "$SRCDIR"
fi

patch -p1 -N -s -d "$SRCDIR/openssl-$VER" < "$(pwd)/cxx_modules/openssl-trim.patch" || [ $? -eq 1 ]

rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

echo "[*] configuring openssl $VER for $SUFFIX ($PRESET, static) ..."
../openssl-$VER/Configure "$PRESET" $FLAGS \
  CC="zig cc $TARGETFLAG" AR="zig ar" RANLIB="zig ranlib"

echo "[*] building libs (this is the slow part) ..."
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)" build_libs

echo "[*] installing into $DEST ..."
mkdir -p "$DEST/lib"
cp libssl.a libcrypto.a "$DEST/lib/"
rm -rf "$DEST/include"
cp -R ../openssl-$VER/include "$DEST/include"
cp include/openssl/* "$DEST/include/openssl/"
echo "$STAMP" > "$DEST/.build-flags"

echo "[+] openssl-$SUFFIX ready at $DEST"
