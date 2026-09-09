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

collect_dep liblz4 /opt/homebrew/opt/lz4 /usr/local/opt/lz4
collect_dep libzstd /opt/homebrew/opt/zstd /usr/local/opt/zstd

case "$LDFLAGS" in
  *-llz4*) ;;
  *) LDFLAGS="$LDFLAGS -llz4" ;;
esac
case "$LDFLAGS" in
  *-lzstd*) ;;
  *) LDFLAGS="$LDFLAGS -lzstd" ;;
esac


SRC="main.cxx"

mkdir -p out

echo "[*] CFLAGS=$CFLAGS"
echo "[*] LDFLAGS=$LDFLAGS"
zig c++ -std=c++20 -O2 -Wall -Wextra $SRC -o out/dicker $CFLAGS $LDFLAGS
echo "[+] built ./out/dicker"
