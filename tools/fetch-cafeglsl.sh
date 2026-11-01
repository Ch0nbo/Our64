#!/usr/bin/env bash
# Downloads the CafeGLSL Wii U static library into third_party/cafeglsl and checks its hash.
set -euo pipefail
VER=1.0.0-rc1
SHA=4c18928e9f0c56d041352d6b676c8bdd5b9f89a9c0f61b25fa56207a45aec0fc
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/cafeglsl"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/c.tar.xz" "https://github.com/Exzap/CafeGLSL/releases/download/v$VER/cafeglsl-$VER-wiiu-ppc.tar.xz"
echo "$SHA  $TMP/c.tar.xz" | sha256sum -c -
rm -rf "$DEST" && mkdir -p "$DEST"
tar -xf "$TMP/c.tar.xz" -C "$DEST" --strip-components=1
echo "CafeGLSL $VER -> $DEST"
