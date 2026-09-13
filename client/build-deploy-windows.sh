#!/usr/bin/env bash
# build-deploy-windows.sh — cross-compile deploy.cxx -> out/deploy.exe for Windows
# (x86_64-windows-gnu) with zig: statically linked vendored lz4+zstd, plus VSS and
# winsock. Does not touch deploy.cxx. Mirrors the client's Windows build.
set -e
cd "$(dirname "$0")"

MODS=../cxx_modules
TARGET=x86_64-windows-gnu
STD="-std=c++20"
OPT="-O2"
SEC="-ffunction-sections -fdata-sections"
INC="-I. -I../shared -I$MODS -I$MODS/lz4 -I$MODS/zstd/lib"
LIBS="-lws2_32 -lvssapi -lole32 -loleaut32"
SIZEOPT="-Wl,--gc-sections -s"

# Reuse the per-target vendored static lib; build.sh creates it (as a side effect
# of building the client .exe) if it isn't there yet.
LIBV="out/libvendor-$TARGET.a"
if [ ! -f "$LIBV" ]; then
  ./build.sh "$TARGET" >/dev/null
fi

echo "[*] linking out/deploy.exe"
zig c++ -target "$TARGET" $STD $OPT $SEC -Wall $INC deploy.cxx "$LIBV" $LIBS $SIZEOPT -o out/deploy.exe
echo "[+] built ./out/deploy.exe"
