#!/usr/bin/env bash
# build-wolfssl.sh — build static wolfSSL for a target triple with zig, into
# cxx_modules/wolfssl/$SUFFIX. Replacement for build-openssl.sh: same zig
# cross-compile approach and cache-by-directory idempotency, but outputs live
# under cxx_modules/ (the home for from-source deps) instead of a top-level out/.
#
#   ./build-wolfssl.sh                        native host build   -> cxx_modules/wolfssl/host
#   ./build-wolfssl.sh x86_64-windows-gnu     Windows (mingw via zig)
#   ./build-wolfssl.sh x86_64-linux-musl      static Linux
#   ./build-wolfssl.sh aarch64-macos          cross macOS
#
# Needs curl + tar + make + autotools (autoconf/automake/libtool). The GitHub
# source archive has no generated ./configure, so autogen.sh builds it once
# (macOS: brew install autoconf automake libtool). Result is cached by directory
# existence + a build stamp; rm -rf cxx_modules/wolfssl cxx_modules/.cache/wolfssl-src
# to force a rebuild.
#
# Throughput note: the asm backends are what make wolfSSL competitive with
# OpenSSL on bulk AEAD. x86_64 gets --enable-intelasm --enable-aesni (AVX +
# AES-NI/PCLMULQDQ for AES-GCM, AVX2 for ChaCha20-Poly1305). aarch64 gets
# --enable-armasm=inline (ARMv8 crypto ext). The INLINE variant is what makes this
# link on Apple: wolfSSL's standalone .S emits ELF-style symbol names that don't
# link under mach-o (undefined _BlockSha3 / _Transform_Sha512_Len_crypto), whereas
# inline asm is compiled by clang/zig as ordinary C objects -- same crypto-ext
# instructions, same speed, but the symbols resolve. Works on ELF too, so all
# aarch64 uses it. Windows x86_64 uses the SAME Intel asm as Linux/mac: wolfSSL's
# .S carry Win64 ABI handling and Win64 doesn't underscore-prefix symbols, so it
# assembles under zig/mingw. A completed handshake + data transfer confirms the
# ABI is right (a wrong-ABI build would fail AES-GCM authentication, not link).
set -e
cd "$(dirname "$0")"

VER="${WOLFSSL_VER:-5.7.4}"          # override with WOLFSSL_VER=...; must be a real -stable release
TARGET="$1"
TARGETFLAG=""
SUFFIX="host"
if [ -n "$TARGET" ]; then
  TARGETFLAG="-target $TARGET"
  SUFFIX="$TARGET"
fi

# autotools --host triple + asm selection, keyed off the zig target (or host arch).
HOSTTRIPLE=""
ASMCONF=""
case "$TARGET" in
  "")
    case "$(uname -m)" in
      arm64|aarch64) ASMCONF="--enable-armasm=inline" ;;  # inline asm links under mach-o, keeps crypto-ext
      x86_64)        ASMCONF="--enable-intelasm --enable-aesni" ;;
      *) echo "[!] unsupported host arch: $(uname -m)"; exit 1 ;;
    esac
    ;;
  x86_64-macos)       HOSTTRIPLE="x86_64-apple-darwin";  ASMCONF="--enable-intelasm --enable-aesni" ;;
  aarch64-macos)      HOSTTRIPLE="aarch64-apple-darwin"; ASMCONF="--enable-armasm=inline" ;;
  x86_64-linux-musl)  HOSTTRIPLE="x86_64-linux-musl";    ASMCONF="--enable-intelasm --enable-aesni" ;;
  aarch64-linux-musl) HOSTTRIPLE="aarch64-linux-musl";   ASMCONF="--enable-armasm=inline" ;;
  x86_64-windows-gnu) HOSTTRIPLE="x86_64-w64-mingw32";   ASMCONF="--enable-intelasm --enable-aesni" ;;
  *) echo "[!] unsupported target: $TARGET"; exit 1 ;;
esac
HOSTFLAG=""
[ -n "$HOSTTRIPLE" ] && HOSTFLAG="--host=$HOSTTRIPLE"

DEST="$(pwd)/cxx_modules/wolfssl/$SUFFIX"
SRCDIR="$(pwd)/cxx_modules/.cache/wolfssl-src"
BUILDDIR="$SRCDIR/build-$SUFFIX"

# Feature set: modern TLS 1.3 (+1.2 fallback) client. AEAD ciphers + ECDHE/x25519
# key exchange + SNI/ALPN for handshake shaping. RSA stays enabled (default) so
# an RSA server cert still verifies at the handshake layer even under VERIFY_NONE;
# drop --enable-rsa-adjacent bits if your server cert is ECDSA-only and you want
# it smaller. sys-ca-certs is disabled: we set VERIFY_NONE and don't touch the OS
# trust store, and on Apple it otherwise drags in Security.framework/CoreFoundation
# and fails to link. -ffunction-sections/-fdata-sections so the linker --gc-sections.
CONFFLAGS="$HOSTFLAG --prefix=$DEST \
--enable-static --disable-shared \
--enable-tls13 \
--enable-sni --enable-alpn \
--enable-aesgcm --enable-chacha --enable-poly1305 \
--enable-ecc --enable-curve25519 --enable-supportedcurves \
--disable-examples --disable-crypttests --disable-sys-ca-certs \
$ASMCONF"

STAMP="1|$VER|$SUFFIX|$CONFFLAGS"

if [ -f "$DEST/lib/libwolfssl.a" ] \
   && [ -f "$DEST/include/wolfssl/options.h" ] \
   && [ "$(cat "$DEST/.build-flags" 2>/dev/null)" = "$STAMP" ]; then
  echo "[=] wolfssl-$SUFFIX already built"
  exit 0
fi

mkdir -p "$SRCDIR"
if [ ! -d "$SRCDIR/wolfssl-$VER-stable" ]; then
  echo "[*] downloading wolfssl $VER ..."
  # GitHub only attaches .asc signatures to releases, not the pre-configured
  # tarball (that lives on wolfssl.com); pull the source archive instead. It
  # extracts to wolfssl-$VER-stable/ but ships no ./configure.
  curl -fsSL -o "$SRCDIR/wolfssl-$VER.tar.gz" \
    "https://github.com/wolfSSL/wolfssl/archive/refs/tags/v$VER-stable.tar.gz"
  tar xzf "$SRCDIR/wolfssl-$VER.tar.gz" -C "$SRCDIR"
fi

if [ ! -f "$SRCDIR/wolfssl-$VER-stable/configure" ]; then
  echo "[*] generating configure (autogen.sh) ..."
  ( cd "$SRCDIR/wolfssl-$VER-stable" && ./autogen.sh )
fi

# wolfSSL's generated Intel *_asm.S carry ELF-only .type/.size directives (with
# @function). Apple's mach-o assembler rejects them ("unknown directive .size",
# "expected absolute expression" at @function), breaking the x86_64-macos build.
# They are symbol metadata only, so strip them: the same instructions assemble
# under mach-o, and dropping them is harmless on ELF/PE. Idempotent; perl so it
# works the same on macOS and Linux hosts.
find "$SRCDIR/wolfssl-$VER-stable/wolfcrypt/src" -name '*_asm.S' -print0 \
  | xargs -0 perl -i -ne 'print unless /^\s*\.(type|size)\b/'

rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# libtool cross-to-mingw derives its archiver via AC_CHECK_TOOLS(AR,[ar lib
# "link -lib"]): it looks for $host-ar, then plain ar, then falls through to lib
# (the MSVC librarian) -> "lib: command not found". Passing AR= alone doesn't
# help because libtool re-derives tools from the host prefix, so drop single-exe
# shims for BOTH the plain and $host-prefixed tool names onto PATH; the $host-ar
# shim is found first and lib is never reached. (zig provides cc/ar/ranlib.)
SHIM="$BUILDDIR/.shim"
mkdir -p "$SHIM"
mkshim() { printf '#!/bin/sh\nexec %s "$@"\n' "$2" > "$SHIM/$1"; chmod +x "$SHIM/$1"; }
mkshim cc     "zig cc $TARGETFLAG"
mkshim ar     "zig ar"
mkshim ranlib "zig ranlib"
mkshim lib    "zig lib"   # libtool's mingw path insists on the MSVC librarian; zig ships a drop-in lib.exe
if [ -n "$HOSTTRIPLE" ]; then
  mkshim "$HOSTTRIPLE-gcc"    "zig cc $TARGETFLAG"
  mkshim "$HOSTTRIPLE-cc"     "zig cc $TARGETFLAG"
  mkshim "$HOSTTRIPLE-ar"     "zig ar"
  mkshim "$HOSTTRIPLE-ranlib" "zig ranlib"
fi
export PATH="$SHIM:$PATH"

echo "[*] configuring wolfssl $VER for $SUFFIX ($ASMCONF, static) ..."
"$SRCDIR/wolfssl-$VER-stable/configure" $CONFFLAGS \
  CC="$SHIM/cc" AR="$SHIM/ar" RANLIB="$SHIM/ranlib" \
  C_EXTRA_FLAGS="-ffunction-sections -fdata-sections"

echo "[*] building (asm crypto is the slow part) ..."
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"

echo "[*] installing into $DEST ..."
rm -rf "$DEST"
make install

# On the mingw target libtool archives in MSVC mode and installs libwolfssl.lib;
# rename to libwolfssl.a so downstream -lwolfssl resolves it the same as every
# other target (zig's lld reads the COFF archive regardless of extension).
if [ -f "$DEST/lib/libwolfssl.lib" ] && [ ! -f "$DEST/lib/libwolfssl.a" ]; then
  mv "$DEST/lib/libwolfssl.lib" "$DEST/lib/libwolfssl.a"
fi

echo "$STAMP" > "$DEST/.build-flags"

echo "[+] wolfssl-$SUFFIX ready at $DEST (lib/libwolfssl.a, include/wolfssl/options.h)"
