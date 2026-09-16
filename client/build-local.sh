#!/usr/bin/env bash
# build-local.sh — build the SDK-style example (main.cxx) -> out/dicker-local,
# for running dicker as a library locally. build.sh builds the shipped CLI
# (cli.cxx -> out/dicker); this is its sibling for main.cxx. Host-only; reuses
# the vendored lz4+zstd static lib that build.sh produces.
set -e
cd "$(dirname "$0")"

MODS=../cxx_modules
STD="-std=c++20"
OPT="-O2"
SEC="-ffunction-sections -fdata-sections"
WOLFDIR="$MODS/wolfssl/host"
INC="-I. -I../shared -I$MODS -I$MODS/lz4 -I$MODS/zstd/lib -I$WOLFDIR/include"

case "$(uname -s)" in
  Linux) SIZEOPT="-Wl,--gc-sections -s" ;;
  *)     SIZEOPT="-Wl,-dead_strip" ;;
esac

LIBV="out/libvendor-host.a"
if [ ! -f "$LIBV" ]; then
  ./build.sh >/dev/null   # produces the vendored static lib (and out/dicker)
fi

../build-wolfssl.sh ""

echo "[*] linking out/dicker-local"
zig c++ $STD $OPT $SEC -Wall $INC main.cxx "$LIBV" -L"$WOLFDIR/lib" -lwolfssl $SIZEOPT -o out/dicker-local
echo "[+] built ./out/dicker-local"
