#!/usr/bin/env bash
# Install everything build.sh needs to cross-compile this addon on Linux:
#   * an LLVM/clang toolchain with LLD          (system packages)
#   * the MSVC CRT + Windows SDK via xwin        -> ~/.xwin
#   * a standalone Linux DirectX Shader Compiler -> ~/.local/dxc
#   * the RenoDX checkout (ReShade / DLSS / Detours / ImGui headers) -> ~/projects/renodx
#
# Idempotent: each step is skipped if it is already satisfied. Re-run any time.
set -euo pipefail

RENODX="$HOME/projects/renodx"
XWIN="$HOME/.xwin"
DXC_DIR="$HOME/.local/dxc"
LOCAL_BIN="$HOME/.local/bin"
mkdir -p "$LOCAL_BIN"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

# --- 1. system toolchain ---------------------------------------------------
install_system_packages() {
  say "Installing the LLVM/clang toolchain + helpers"
  local SUDO=""
  if [[ $EUID -ne 0 ]]; then SUDO="sudo"; fi

  if command -v dnf >/dev/null 2>&1; then
    # Fedora's clang is already >= 19 (what the MSVC STL asserts).
    $SUDO dnf install -y clang lld llvm python3 git curl tar
  elif command -v apt-get >/dev/null 2>&1; then
    # Ubuntu's default clang (18 on 24.04) is too old: the MSVC STL that xwin
    # splats asserts "Clang 19.0.0 or newer". Pull a recent clang/lld from
    # apt.llvm.org and expose it under the unversioned names build.sh calls.
    local LLVM_VER=19
    $SUDO apt-get update
    $SUDO apt-get install -y python3 git curl wget tar ca-certificates gnupg lsb-release
    curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
    chmod +x /tmp/llvm.sh
    $SUDO /tmp/llvm.sh "$LLVM_VER"
    $SUDO apt-get install -y "clang-$LLVM_VER" "lld-$LLVM_VER" "llvm-$LLVM_VER"
    for t in clang clang++ lld ld.lld; do
      $SUDO ln -sf "/usr/bin/${t}-$LLVM_VER" "/usr/local/bin/${t}"
    done
  elif command -v pacman >/dev/null 2>&1; then
    $SUDO pacman -Sy --needed --noconfirm clang lld llvm python git curl tar
  elif command -v zypper >/dev/null 2>&1; then
    $SUDO zypper install -y clang lld llvm python3 git curl tar
  else
    echo "!! No known package manager (dnf/apt/pacman/zypper). Install manually:"
    echo "   clang lld llvm python3 git curl tar"
    exit 1
  fi
}

# --- 2. xwin: MSVC CRT + Windows SDK --------------------------------------
# clang -target x86_64-pc-windows-msvc needs the real MSVC headers/libs. xwin
# fetches the redistributable Microsoft packages and splats them into ~/.xwin.
install_xwin() {
  if [[ -d "$XWIN/crt/include" && -d "$XWIN/sdk/include/um" ]]; then
    say "xwin SDK already present at $XWIN -- skipping"
    return
  fi
  say "Fetching the xwin tool"
  local xwin_bin
  xwin_bin="$(command -v xwin || true)"
  if [[ -z "$xwin_bin" && -x "$LOCAL_BIN/xwin" ]]; then xwin_bin="$LOCAL_BIN/xwin"; fi
  if [[ -z "$xwin_bin" ]]; then
    # Grab the latest prebuilt static Linux binary from GitHub releases.
    local url
    url="$(curl -fsSL https://api.github.com/repos/Jake-Shadle/xwin/releases/latest \
      | python3 -c 'import sys,json;print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if "x86_64-unknown-linux-musl" in a["name"] and a["name"].endswith(".tar.gz")))')"
    echo "downloading: $url"
    local tmp; tmp="$(mktemp -d)"
    curl -fsSL "$url" -o "$tmp/xwin.tar.gz"
    tar -xzf "$tmp/xwin.tar.gz" -C "$tmp"
    cp "$(find "$tmp" -type f -name xwin | head -1)" "$LOCAL_BIN/xwin"
    chmod +x "$LOCAL_BIN/xwin"
    rm -rf "$tmp"
    xwin_bin="$LOCAL_BIN/xwin"
  fi

  say "Splatting the MSVC CRT + Windows SDK into $XWIN (a few hundred MB)"
  "$xwin_bin" --accept-license splat --output "$XWIN"
}

# --- 3. standalone Linux DXC ----------------------------------------------
# HLSL -> DXIL for vkd3d-proton. The official DirectXShaderCompiler releases
# ship a Linux x86_64 tarball (bin/dxc + lib/libdxcompiler.so).
install_dxc() {
  if [[ -x "$DXC_DIR/bin/dxc" ]]; then
    say "DXC already present at $DXC_DIR -- skipping"
    return
  fi
  say "Fetching a standalone Linux DXC"
  local url
  url="$(curl -fsSL https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/latest \
    | python3 -c 'import sys,json;print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if "linux_dxc" in a["name"] and a["name"].endswith(".tar.gz")))')"
  echo "downloading: $url"
  local tmp; tmp="$(mktemp -d)"
  curl -fsSL "$url" -o "$tmp/dxc.tar.gz"
  tar -xzf "$tmp/dxc.tar.gz" -C "$tmp"
  local dxcbin; dxcbin="$(find "$tmp" -type f -name dxc -path '*/bin/*' | head -1)"
  if [[ -z "$dxcbin" ]]; then echo "!! dxc binary not found in the archive"; exit 1; fi
  local root; root="$(dirname "$(dirname "$dxcbin")")"
  mkdir -p "$DXC_DIR"
  cp -r "$root"/. "$DXC_DIR/"
  chmod +x "$DXC_DIR/bin/dxc"
  rm -rf "$tmp"
}

# --- 4. RenoDX headers -----------------------------------------------------
# build.sh reads ReShade / DLSS / Detours / ImGui straight out of the RenoDX
# checkout's submodules.
install_renodx() {
  if [[ -f "$RENODX/external/reshade/include/reshade.hpp" ]]; then
    say "RenoDX already checked out at $RENODX -- skipping"
    return
  fi
  say "Cloning RenoDX (with submodules) into $RENODX"
  mkdir -p "$(dirname "$RENODX")"
  if [[ -d "$RENODX/.git" ]]; then
    git -C "$RENODX" submodule update --init --recursive
  else
    git clone --recurse-submodules https://github.com/clshortfuse/renodx "$RENODX"
  fi
}

install_system_packages
install_xwin
install_dxc
install_renodx

say "Done. Verifying:"
printf '  clang++ : %s\n' "$(command -v clang++ || echo MISSING)"
printf '  xwin SDK: %s\n' "$([[ -d "$XWIN/crt/include" ]] && echo "$XWIN" || echo MISSING)"
printf '  dxc     : %s\n' "$([[ -x "$DXC_DIR/bin/dxc" ]] && echo "$DXC_DIR/bin/dxc" || echo MISSING)"
printf '  renodx  : %s\n' "$([[ -f "$RENODX/external/reshade/include/reshade.hpp" ]] && echo "$RENODX" || echo MISSING)"
echo
echo "Now build with:  bash build.sh"
