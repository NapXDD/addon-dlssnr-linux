#!/usr/bin/env bash
# Build and run the hook-layering repro (issue #3 / PR #4) under plain wine.
# No GPU, Proton, or ReShade needed: harness.cpp stubs the ReShade addon API
# and stub_nvngx.cpp stands in for the driver's _nvngx.dll.
#
# Usage: ./run.sh [path/to/dlssnr-linux.addon64]   (default: ../../build/)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ADDON="${1:-$REPO/build/dlssnr-linux.addon64}"

RENODX="$HOME/projects/renodx"
NGX_INC="$RENODX/external/DLSS/include"
XWIN="$HOME/.xwin"

CLANGXX="clang++"
if [[ -x "$HOME/.local/llvm/bin/clang++" ]]; then
  CLANGXX="$HOME/.local/llvm/bin/clang++"
fi

CFLAGS=(
  -target x86_64-pc-windows-msvc -fuse-ld=lld -O2
  -isystem "$XWIN/crt/include"
  -isystem "$XWIN/sdk/include/ucrt"
  -isystem "$XWIN/sdk/include/um"
  -isystem "$XWIN/sdk/include/shared"
)
LDFLAGS=(
  -L "$XWIN/crt/lib/x86_64"
  -L "$XWIN/sdk/lib/um/x86_64"
  -L "$XWIN/sdk/lib/ucrt/x86_64"
)

[[ -f "$ADDON" ]] || { echo "addon not found: $ADDON (run ./build.sh first)"; exit 2; }

"$CLANGXX" -shared -std=c++20 "${CFLAGS[@]}" "${LDFLAGS[@]}" \
  -o "$HERE/_nvngx.dll" "$HERE/stub_nvngx.cpp"
"$CLANGXX" -std=c++20 "${CFLAGS[@]}" "${LDFLAGS[@]}" -I "$NGX_INC" \
  -o "$HERE/repro.exe" "$HERE/harness.cpp"
cp "$ADDON" "$HERE/dlssnr-linux.addon64"

cd "$HERE"
set +e
# shellcheck disable=SC2086 -- REPRO_ARGS is deliberately word-split
WINEDEBUG=-all wine ./repro.exe ${REPRO_ARGS:-}
rc=$?
set -e

echo
case "$rc" in
  0)  echo "RESULT: CLEAN — no hook leak (fixed build)" ;;
  42) echo "RESULT: REPRO — re-entrant hook loop (the GTA V Enhanced hang)" ;;
  43) echo "RESULT: REPRO — call faulted through a stale detour" ;;
  44) echo "RESULT: REPRO — detour leaked past addon unload" ;;
  45) echo "RESULT: FAIL — addon hooked despite the leak marker" ;;
  *)  echo "RESULT: harness/setup failure (rc=$rc)" ;;
esac
exit "$rc"
