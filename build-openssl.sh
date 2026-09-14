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

if [ -f "$DEST/lib/libcrypto.a" ] && [ -f "$DEST/lib/libssl.a" ]; then
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

mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

echo "[*] configuring openssl $VER for $SUFFIX ($PRESET, static) ..."
../openssl-$VER/Configure "$PRESET" no-shared $ASMFLAG no-tests no-apps no-docs no-engine \
  CC="zig cc $TARGETFLAG" AR="zig ar" RANLIB="zig ranlib"

echo "[*] building libs (this is the slow part) ..."
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)" build_libs

echo "[*] installing into $DEST ..."
mkdir -p "$DEST/lib"
cp libssl.a libcrypto.a "$DEST/lib/"
rm -rf "$DEST/include"
cp -R include "$DEST/include"

echo "[+] openssl-$SUFFIX ready at $DEST"
