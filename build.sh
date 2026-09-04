#!/usr/bin/env bash
# Cross-compile the addon on Linux into a ReShade .addon64 with the MSVC ABI:
# clang -target x86_64-pc-windows-msvc + MS headers/libs fetched by xwin.
# This matches MSVC-built ReShade exactly (vtables, aggregate returns, mangling),
# unlike MinGW GCC, whose sret convention crashes on calls like get_resource_desc.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RENODX="$HOME/projects/renodx"
RESHADE_INC="$RENODX/external/reshade/include"
IMGUI_INC="$RENODX/external/reshade/deps/imgui"
NGX_INC="$RENODX/external/DLSS/include"
DETOURS_SRC="$RENODX/external/Detours/src"
XWIN="$HOME/.xwin"
OUT="$HERE/build"
mkdir -p "$OUT"

# Prefer a local prebuilt LLVM (no root needed) over a system clang.
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

# Compute shaders: HLSL -> DXIL with standalone dxc, embedded as C arrays with xxd.
DXC="$HOME/.local/dxc/bin/dxc"
SHADER_SRC="$HERE/src/shaders"
SHADER_OUT="$OUT/shaders"
mkdir -p "$SHADER_OUT"
for sh in nr_encode nr_resolve nr_lum1 nr_lum2; do
  hlsl="$SHADER_SRC/$sh.hlsl"
  hdr="$SHADER_OUT/${sh}_dxil.h"
  if [[ ! -f "$hdr" || "$hlsl" -nt "$hdr" || "$SHADER_SRC/nr_common.hlsli" -nt "$hdr" ]]; then
    echo "compiling shader: $sh"
    "$DXC" -T cs_6_0 -E main -O3 -Fo "$SHADER_OUT/${sh}_dxil" "$hlsl"
    python3 -c "
import sys
data = open('$SHADER_OUT/${sh}_dxil', 'rb').read()
with open('$hdr', 'w') as f:
    f.write('unsigned char ${sh}_dxil[] = {' + ','.join(str(b) for b in data) + '};\n')
"
  fi
done

# Detours (from the RenoDX submodule), compiled once and cached.
DETOURS_OBJ="$OUT/detours-obj"
mkdir -p "$DETOURS_OBJ"
for tu in detours modules disasm; do
  obj="$DETOURS_OBJ/$tu.obj"
  src="$DETOURS_SRC/$tu.cpp"
  if [[ ! -f "$obj" || "$src" -nt "$obj" ]]; then
    echo "compiling detours: $tu.cpp"
    "$CLANGXX" -c -std=c++17 "${CFLAGS[@]}" -w -D_CRT_SECURE_NO_WARNINGS \
      -I "$DETOURS_SRC" -o "$obj" "$src"
  fi
done

"$CLANGXX" -shared -std=c++20 "${CFLAGS[@]}" "${LDFLAGS[@]}" \
  -I "$RESHADE_INC" \
  -I "$IMGUI_INC" \
  -I "$NGX_INC" \
  -I "$DETOURS_SRC" \
  -o "$OUT/nr-linux-probe.addon64" \
  "$HERE/src/addon.cpp" \
  "$DETOURS_OBJ"/detours.obj "$DETOURS_OBJ"/modules.obj "$DETOURS_OBJ"/disasm.obj

echo "built: $OUT/nr-linux-probe.addon64"

# The DLSSNR forwarder: a bare DLL whose filename contains "nvngx.dll" so the snippet's caller gate
# accepts it. No ReShade or Detours dependency.
"$CLANGXX" -shared -std=c++20 "${CFLAGS[@]}" "${LDFLAGS[@]}" \
  -o "$OUT/nvngx.dll_nrfwd.dll" "$HERE/src/forwarder/nr_forwarder.cpp"
echo "built: $OUT/nvngx.dll_nrfwd.dll"

# Deploy straight into the DLSS demo testbed if it exists.
DEMO_DIR="$HOME/projects/dlss-testbed/DLSS_Sample_App/bin/ngx_dlss_demo"
if [[ -d "$DEMO_DIR" ]]; then
  cp "$OUT/nr-linux-probe.addon64" "$DEMO_DIR/"
  echo "deployed to: $DEMO_DIR"
fi

# Deploy into Wuthering Waves (milestone 3: capture Reserved18 create/eval).
WUWA_DIR="$HOME/.local/share/Steam/steamapps/common/Wuthering Waves/Client/Binaries/Win64"
if [[ -d "$WUWA_DIR" ]]; then
  cp "$OUT/nr-linux-probe.addon64" "$OUT/nvngx.dll_nrfwd.dll" "$WUWA_DIR/"
  echo "deployed to: $WUWA_DIR"
fi
