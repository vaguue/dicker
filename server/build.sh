#!/usr/bin/env bash
# build.sh — self-contained server, built with zig. Statically links vendored
# libuv + lz4 + zstd (../cxx_modules), so no system libuv/lz4/zstd is needed.
#
#   ./build.sh                       native host build
#   ./build.sh x86_64-linux-musl     fully static Linux binary (for the VPS)
#
# libuv's platform source set + defines are taken from its CMakeLists (v1.51.0).
set -e
cd "$(dirname "$0")"

MODS=../cxx_modules
UV="$MODS/libuv"
STD="-std=c++17"
OPT="-O2"
SEC="-ffunction-sections -fdata-sections"

TARGET="$1"
TARGETFLAG=""
SUFFIX="host"
if [ -n "$TARGET" ]; then
  TARGETFLAG="-target $TARGET"
  SUFFIX="$TARGET"
fi

# OS comes from the target triple when cross-compiling, else from the host.
OSSEL="${TARGET:-$(uname -s)}"
case "$OSSEL" in
  *inux*) OS=linux ;;
  *)      OS=darwin ;;
esac

# dead-strip + symbol strip (ELF via lld: --gc-sections -s; mach-o: -dead_strip).
if [ "$OS" = linux ]; then
  SIZEOPT="-Wl,--gc-sections -s"
else
  SIZEOPT="-Wl,-dead_strip"
fi

mkdir -p "out/obj/$SUFFIX"

# ---------- libuv (static, cached) ----------
UVLIB="out/libuv-$SUFFIX.a"
if [ ! -f "$UVLIB" ]; then
  echo "[*] building libuv ($OS) for $SUFFIX"
  UVDEF="-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE"
  BASE="fs-poll idna inet random strscpy strtok thread-common threadpool timer uv-common uv-data-getter-setters version"
  UNIX="async core dl fs getaddrinfo getnameinfo loop-watcher loop pipe poll process random-devurandom signal stream tcp thread tty udp"

  SRC=""
  for f in $BASE; do SRC="$SRC $UV/src/$f.c"; done
  for f in $UNIX; do SRC="$SRC $UV/src/unix/$f.c"; done
  if [ "$OS" = linux ]; then
    UVDEF="$UVDEF -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112"
    for f in proctitle linux procfs-exepath random-getrandom random-sysctl-linux; do SRC="$SRC $UV/src/unix/$f.c"; done
  else
    UVDEF="$UVDEF -D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1"
    for f in proctitle bsd-ifaddrs kqueue random-getentropy darwin-proctitle darwin fsevents; do SRC="$SRC $UV/src/unix/$f.c"; done
  fi

  OBJS=""
  for c in $SRC; do
    o="out/obj/$SUFFIX/uv_$(basename "${c%.c}").o"
    zig cc $TARGETFLAG $OPT $SEC $UVDEF -I"$UV/include" -I"$UV/src" -c "$c" -o "$o"
    OBJS="$OBJS $o"
  done
  zig ar rcs "$UVLIB" $OBJS
fi

# ---------- lz4 + zstd (static, cached) ----------
VLIB="out/libvendor-$SUFFIX.a"
if [ ! -f "$VLIB" ]; then
  echo "[*] building lz4 + zstd for $SUFFIX"
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
  zig ar rcs "$VLIB" $OBJS
fi

# ---------- server ----------
SOURCES="main.cxx server.cxx connection.cxx byte_channel.cxx file_sink.cxx handshake.cxx decompressor.cxx lz4_decompressor.cxx zstd_decompressor.cxx"
INC="-I../shared -I$UV/include -I$MODS/lz4 -I$MODS/zstd/lib"

SYSLIBS="-lpthread"
if [ "$OS" = linux ]; then
  SYSLIBS="$SYSLIBS -ldl -lrt"
fi

SRVOUT="out/dicker-server"
[ -n "$TARGET" ] && SRVOUT="out/dicker-server-$SUFFIX"   # cross builds never clobber the host binary

echo "[*] linking $SRVOUT"
zig c++ $TARGETFLAG $STD $OPT $SEC -Wall $INC $SOURCES "$UVLIB" "$VLIB" $SYSLIBS $SIZEOPT -o "$SRVOUT"
echo "[+] built ./$SRVOUT"
