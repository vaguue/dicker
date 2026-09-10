#!/usr/bin/env bash
# build.sh — self-contained client, built entirely with zig (host or cross).
# Statically links vendored lz4 + zstd (../cxx_modules), so the only runtime
# dependency is the platform libc.
#
#   ./build.sh                         native host build
#   ./build.sh x86_64-windows-gnu      Windows (with VSS, cross-compiled)
#   ./build.sh aarch64-linux-musl      static Linux
#
# Windows links VSS (vssapi/ole32/oleaut32) + winsock (ws2_32); zig's bundled
# mingw-w64 ships the VSS SDK headers, so no MSVC is needed.
set -e
cd "$(dirname "$0")"

MODS=../cxx_modules
STD="-std=c++20"
OPT="-O2"
SEC="-ffunction-sections -fdata-sections"   # let the linker drop unused code
INC="-I. -I../shared -I$MODS/lz4 -I$MODS/zstd/lib"

TARGET="$1"
TARGETFLAG=""
SUFFIX="host"
if [ -n "$TARGET" ]; then
  TARGETFLAG="-target $TARGET"
  SUFFIX="$TARGET"
fi

# dead-strip + symbol strip: ELF/COFF via lld take --gc-sections -s; mach-o ld64
# uses -dead_strip.
case "${TARGET:-$(uname -s)}" in
  *inux*|*indows*) SIZEOPT="-Wl,--gc-sections -s" ;;
  *)               SIZEOPT="-Wl,-dead_strip" ;;
esac

OUT="out/dicker"
[ -n "$TARGET" ] && OUT="out/dicker-$SUFFIX"   # cross builds never clobber the host binary
EXTRA_LIBS=""
case "$TARGET" in
  *windows*)
    OUT="$OUT.exe"
    EXTRA_LIBS="-lws2_32 -lvssapi -lole32 -loleaut32"
    ;;
esac

mkdir -p "out/obj/$SUFFIX"
LIBV="out/libvendor-$SUFFIX.a"

if [ ! -f "$LIBV" ]; then
  echo "[*] compiling vendored lz4 + zstd for $SUFFIX (cached after first build)"
  OBJS=""

  for c in "$MODS"/lz4/lz4.c "$MODS"/lz4/lz4hc.c "$MODS"/lz4/lz4frame.c "$MODS"/lz4/xxhash.c; do
    o="out/obj/$SUFFIX/$(echo "$c" | sed 's#[./]#_#g').o"
    zig cc $TARGETFLAG $OPT $SEC -I"$MODS"/lz4 -c "$c" -o "$o"
    OBJS="$OBJS $o"
  done

  for c in $(ls "$MODS"/zstd/lib/common/*.c "$MODS"/zstd/lib/compress/*.c "$MODS"/zstd/lib/decompress/*.c); do
    o="out/obj/$SUFFIX/$(echo "$c" | sed 's#[./]#_#g').o"
    zig cc $TARGETFLAG $OPT $SEC -DZSTD_DISABLE_ASM -I"$MODS"/zstd/lib -I"$MODS"/zstd/lib/common -c "$c" -o "$o"
    OBJS="$OBJS $o"
  done

  zig ar rcs "$LIBV" $OBJS
fi

echo "[*] linking $OUT"
zig c++ $TARGETFLAG $STD $OPT $SEC -Wall $INC main.cxx "$LIBV" $EXTRA_LIBS $SIZEOPT -o "$OUT"
echo "[+] built ./$OUT"
