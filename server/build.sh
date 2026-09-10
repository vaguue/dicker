#!/usr/bin/env bash
# build.sh — build the dicker server with `zig c++` against system libuv / liblz4 / libzstd.
set -e
cd "$(dirname "$0")"

CFLAGS="-I../shared"
LDFLAGS=""

collect_dep() {
  local pkg="$1"
  shift
  if pkg-config --exists "$pkg" 2>/dev/null; then
    CFLAGS="$CFLAGS $(pkg-config --cflags "$pkg")"
    LDFLAGS="$LDFLAGS $(pkg-config --libs "$pkg")"
    return
  fi
  for prefix in "$@"; do
    if [ -d "$prefix/include" ]; then
      CFLAGS="$CFLAGS -I$prefix/include"
      LDFLAGS="$LDFLAGS -L$prefix/lib"
      break
    fi
  done
}

collect_dep libuv /opt/homebrew/opt/libuv /usr/local/opt/libuv
collect_dep liblz4 /opt/homebrew/opt/lz4 /usr/local/opt/lz4
collect_dep libzstd /opt/homebrew/opt/zstd /usr/local/opt/zstd

case "$LDFLAGS" in
  *-luv*) ;;
  *) LDFLAGS="$LDFLAGS -luv" ;;
esac
case "$LDFLAGS" in
  *-llz4*) ;;
  *) LDFLAGS="$LDFLAGS -llz4" ;;
esac
case "$LDFLAGS" in
  *-lzstd*) ;;
  *) LDFLAGS="$LDFLAGS -lzstd" ;;
esac

SOURCES="main.cxx \
  server.cxx \
  connection.cxx \
  byte_channel.cxx \
  file_sink.cxx \
  handshake.cxx \
  decompressor.cxx \
  lz4_decompressor.cxx \
  zstd_decompressor.cxx"

mkdir -p out

echo "[*] CFLAGS=$CFLAGS"
echo "[*] LDFLAGS=$LDFLAGS"
zig c++ -std=c++17 -O2 -Wall -Wextra $SOURCES -o out/dicker-server $CFLAGS $LDFLAGS
echo "[+] built ./out/dicker-server"
