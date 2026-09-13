#!/usr/bin/env bash
# release.sh — cross-compile client + server for every target with zig, into dist/.
# Run with bash:  bash release.sh
#
# Client ships for macOS (both arches), static Linux (musl, both arches) and
# Windows (with VSS). The server ships for macOS and static Linux — Windows
# server isn't built (its libuv win/ file set isn't wired up; the server targets
# the Linux VPS).
cd "$(dirname "$0")"

CLIENT_TARGETS=(x86_64-macos aarch64-macos x86_64-linux-musl aarch64-linux-musl x86_64-windows-gnu)
SERVER_TARGETS=(x86_64-macos aarch64-macos x86_64-linux-musl aarch64-linux-musl)

rm -rf dist && mkdir -p dist
ok=(); fail=()

build() {   # $1=dir  $2=target  $3=out-basename  $4=dist-name
  if ( cd "$1" && ./build.sh "$2" ) >"/tmp/rel-$2-$1.log" 2>&1; then
    if cp "$1/out/$3" "dist/$4"; then ok+=("$4"); else fail+=("$4 (copy)"); fi
  else
    fail+=("$4 (build — see /tmp/rel-$2-$1.log)")
  fi
}

for t in "${CLIENT_TARGETS[@]}"; do
  echo "== client $t =="
  case "$t" in
    *windows*) build client "$t" "dicker-$t.exe" "dicker-$t.exe" ;;
    *)         build client "$t" "dicker-$t"     "dicker-$t" ;;
  esac
done

for t in "${SERVER_TARGETS[@]}"; do
  echo "== server $t =="
  build server "$t" "dicker-server-$t" "dicker-server-$t"
done

echo
echo "=== built ==="; printf '  %s\n' "${ok[@]:-(none)}"
echo "=== failed ==="; printf '  %s\n' "${fail[@]:-(none)}"
echo "=== dist/ ==="; ls -la dist/ 2>/dev/null | awk 'NR>1{print $5, $NF}'
