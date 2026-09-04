# e2e testing against the DLSS SDK sample app

The e2e test runs the add-on inside NVIDIA's **NGX DLSS sample app** (`ngx_dlss_demo`, from the
[NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) SDK) under Proton — a real D3D12 renderer with real
DLSS-SR, no game and no anti-cheat in the loop. `test/e2e-preset-crash.sh` regression-tests the
retire/rebuild path that used to kill the device (issue #1).

## One-time testbed setup

Everything lives in `~/projects/dlss-testbed/` (the e2e script expects this path):

```
~/projects/dlss-testbed/
├── DLSS_Sample_App/bin/ngx_dlss_demo/   # the sample app (see below)
├── prefix-0/                            # Proton prefix, created on first run
├── logs/                                # dxvk-nvapi / run logs
└── run-demo.sh                          # launcher (listing below)
```

### 1. The sample app

The NGX DLSS sample ships with NVIDIA's DLSS SDK — a prebuilt Windows package
(`ngx_dlss_demo_windows.zip`) containing `DLSS_Sample_App/bin/ngx_dlss_demo/ngx_dlss_demo.exe`
with `nvngx_dlss.dll` beside it. Unzip it into the testbed directory. (It can also be built from
the SDK source with CMake + Visual Studio; the prebuilt package is much less effort.)

### 2. Proton and runtime

- **GE-Proton** in `~/.local/share/Steam/compatibilitytools.d/` (tested: GE-Proton11-1). It
  bundles dxvk-nvapi — that is what exposes NGX/DLSS inside the prefix. On GE-Proton (and
  proton-cachyos) NVAPI is on by default; `PROTON_FORCE_NVAPI=1` forces it even for blocklisted
  titles. (`PROTON_ENABLE_NVAPI` is a Valve-Proton variable and does not exist on these builds.)
- **SteamLinuxRuntime_sniper** installed via Steam (it is a free "tool" download; GE-Proton runs
  inside it).
- An RTX GPU with a driver that supports the DLSS features you want to exercise.

### 3. Files beside `ngx_dlss_demo.exe`

- **ReShade with add-on support**, as `dxgi.dll`, plus a `ReShade.ini` (any minimal one works).
- **VC140 runtime DLLs** — `concrt140.dll`, `msvcp140.dll`, `vcruntime140.dll`,
  `vcruntime140_1.dll` — copied from any Windows game install or the MSVC redistributable.
  Wine's builtin `concrt140` stub aborts the demo, so these must be native (the launcher's
  `WINEDLLOVERRIDES` handles that).
- **`nvngx_dlssnr.dll`** (the DLSSNR model). The e2e script copies it from the Wuthering Waves
  install automatically if it is missing; place it manually if your model lives elsewhere.
- The add-on itself (`dlssnr-linux.addon64` + `nvngx.dll_nrfwd.dll`) — the e2e script deploys
  these for you from `build/`.

### 4. The launcher — `run-demo.sh`

```bash
#!/usr/bin/env bash
# Launch the NGX DLSS sample app under GE-Proton with dxvk-nvapi (DLSS active)
# and ReShade injected via the native dxgi.dll in the demo folder.
# Usage: ./run-demo.sh [-d3d11|-d3d12|-vulkan] [extra demo args]
set -euo pipefail

STEAM="$HOME/.local/share/Steam"
PROTON="$STEAM/compatibilitytools.d/GE-Proton11-1/proton"
SLR="$STEAM/steamapps/common/SteamLinuxRuntime_sniper/run"
HERE="$(cd "$(dirname "$0")" && pwd)"
DEMO="$HERE/DLSS_Sample_App/bin/ngx_dlss_demo/ngx_dlss_demo.exe"

mkdir -p "$HERE/logs"

# Note: STEAM_COMPAT_DATA_PATH must contain a digit somewhere, or GE-Proton's
# protonfixes crashes in get_game_id() and the launch silently no-ops.
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM"
export STEAM_COMPAT_DATA_PATH="$HERE/prefix-0"
export PROTON_FORCE_NVAPI=1  # GE-Proton/cachyos; use PROTON_ENABLE_NVAPI=1 on Valve Proton
export WINEDLLOVERRIDES="dxgi=n,b;concrt140,msvcp140,vcruntime140,vcruntime140_1=n"
export DXVK_NVAPI_LOG_PATH="$HERE/logs"
export DXVK_NVAPI_LOG_LEVEL=info
export DXVK_LOG_PATH="$HERE/logs"

cd "$(dirname "$DEMO")"
exec "$SLR" -- "$PROTON" waitforexitandrun "$DEMO" "${@:--d3d12}"
```

Adjust the `PROTON` path to your GE-Proton version. The prefix (`prefix-0`) is created by Proton
on the first launch. A quick sanity check that the stack works:

```bash
cd ~/projects/dlss-testbed && ./run-demo.sh -d3d12
```

A window with the Donut scene should appear, and
`DLSS_Sample_App/bin/ngx_dlss_demo/ReShade.log` should show `[DLSSNR Linux]` lines with
`nr-fwd: EvaluateFeature #N => 0x1 (Success)`.

## Running the e2e test

From the repository root:

```bash
./test/e2e-preset-crash.sh
```

What it does:

1. Builds the add-on with the e2e hooks compiled in (`build.sh --test`, which defines
   `DLSSNR_TEST_HOOKS` and marks the output with `build/.test-build` — release builds contain
   none of this code) and deploys it into the demo folder. All hook code lives in
   `src/nr_test_hooks.hpp`; the runner only carries `NR_TEST_HOOK_*` macro call sites that
   expand to nothing in release builds, so grepping `NR_TEST_HOOK` shows every lever.
2. Runs the demo for 60 s with `DLSSNR_TEST_RETIRE_EVERY=100`: the hook forces the same
   feature retire the overlay's Apply button triggers, every 100 evaluates — deterministically
   inside the graveyard's 240-frame hold window at any framerate.
3. Asserts from `ReShade.log` that the pass survived: the process reached the timeout, at least
   20 retire/rebuild cycles completed, and evaluates were still advancing past #2000.

Outcomes:

- **PASS** — e.g. `survived 62 retire/rebuild cycles, still evaluating at #6000`.
- **FAIL** — on the pre-fix bug, rendering froze after 3 retires (evaluates stop, log ends on a
  retire, process idles until the timeout kills it); any early exit or stalled evaluate count
  fails the test.
- **SKIP (exit 77)** — testbed, launcher, or model not found on this machine.

The hook env var also works for manual experiments against a `--test` build (never against a
release build, which ignores it):

```bash
cd ~/projects/dlss-testbed && DLSSNR_TEST_RETIRE_EVERY=100 ./run-demo.sh -d3d12
```
