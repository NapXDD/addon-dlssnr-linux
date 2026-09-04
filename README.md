# addon-dlssnr-linux

[![build](https://github.com/NapXDD/addon-dlssnr-linux/actions/workflows/build.yml/badge.svg)](https://github.com/NapXDD/addon-dlssnr-linux/actions/workflows/build.yml)
[![license: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

A ReShade add-on that makes **NVIDIA DLSS 5 Neural Rendering** (DLSSNR — NGX feature 18)
run on **Linux / Proton**.

The closed RenoDX DLSS5 add-on produces a black screen on Linux because the driver-dispatched
feature 18 fails `FAIL_OutOfDate`: the NGX OTA updater it relies on is unavailable under Proton.
This add-on instead drives the game-local `nvngx_dlssnr.dll` snippet **directly** as feature 18,
through a tiny forwarder DLL whose filename contains `nvngx.dll` to satisfy the snippet's caller
gate — bypassing driver dispatch entirely.

Built on Linux with clang targeting the MSVC ABI (`-target x86_64-pc-windows-msvc`) so the
vtables and by-value aggregate returns match MSVC-built ReShade.

## What it does

1. Hooks the game's NGX `CreateFeature` / `EvaluateFeature` (via Microsoft Detours) to learn the
   live DLSS-SR geometry and formats.
2. After each DLSS-SR evaluate, runs the DLSSNR model once on that frame and composites its answer
   back over the game's output, so the game's own tone-mapping consumes the enhanced frame.
3. A colour bridge (display-referred encode with a measured white point, anchored resolve that
   preserves the game's hue) keeps the model — trained on display-referred data — from producing a
   veil, noise, or a colour cast.

The overlay (ReShade *Add-ons* tab, or F10 to A/B the pass) exposes detail/colour/highlight
controls, a measured white-point readout, debug views, model **Style** (Default / Natural /
Cinematic), and the three DLSS5 model intensities: **NR intensity** (`DLSSNR.Intensity`),
**Structure intensity** (`DLSSNR.LocalStructureStrength`), **Global intensity**
(`DLSSNR.LocalToneStrength`).

## Installing the build

You need two files — `dlssnr-linux.addon64` (the add-on) and `nvngx.dll_nrfwd.dll` (the
forwarder). Grab them from a [GitHub Release](../../releases) (every version tag attaches both;
every push also uploads them as a downloadable CI artifact), or build them yourself (see below —
`build.sh` copies them straight into the game folder for you).

### Prerequisites

- An **RTX 50-series** GPU — DLSSNR is RTX 50 only — with a recent NVIDIA driver (≥ 616.56).
- **Proton** with NVAPI/NGX enabled, and **DLSS (Super Resolution) turned on in-game**: this add-on
  runs off the game's DLSS-SR output, so DLSS must be active.
- **ReShade with add-on support** installed for the game's DX12 renderer (the `dxgi` variant).
- The DLSS Neural Rendering model **`nvngx_dlssnr.dll`** present beside the game executable. It is
  NVIDIA's and is *not* shipped here; the add-on only drives it.

> **First, make sure the game itself runs on Proton.** Check
> [ProtonDB](https://www.protondb.com/) for the game's rating before trying this add-on — if the
> game isn't Playable/Gold/Platinum (or is blocked by anti-cheat) under Proton, the add-on can't
> help. This add-on assumes the game already launches and runs under Proton with DLSS working.

### Steps

1. Install ReShade (add-on support enabled) for the game. Its `dxgi.dll` and `ReShade.ini` should
   sit in the same folder as the game executable.
2. Copy **both** `dlssnr-linux.addon64` and `nvngx.dll_nrfwd.dll` into that folder, next to the
   game executable and `nvngx_dlssnr.dll`.
3. Set Steam launch options so Proton exposes NVAPI/NGX and loads ReShade's `dxgi`, e.g.:

   ```
   PROTON_ENABLE_NVAPI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```

4. Launch the game, enable **DLSS** in the graphics settings, then open the ReShade overlay
   (**Home** key) → **Add-ons** tab → **DLSSNR Linux**. Press **F10** any time to A/B toggle the
   pass. `ReShade.log` beside the exe records `nr-fwd:` lines if you need to check it loaded.

> ⚠️ Third-party add-ons in an online game with anti-cheat carry a risk to your account. Use at
> your own risk.

## Building

Install the toolchain and dependencies once (LLVM/clang + LLD, the xwin-provided MSVC SDK at
`~/.xwin`, a standalone Linux DXC at `~/.local/dxc`, and the RenoDX checkout at `~/projects/renodx`
for the ReShade / DLSS / Detours / ImGui headers):

```bash
bash install-deps.sh
```

It's idempotent — it skips anything already present. Then build:

```bash
bash build.sh
```

`install-deps.sh` supports dnf / apt / pacman / zypper for the system packages (needs `sudo` for
those) and downloads the rest into your home directory. See `build.sh` for the exact paths it
expects.

`build.sh` writes the two files to `build/` and, if it finds the configured target folders,
**auto-deploys** them there — so a local build drops straight into place with no manual copy.
Adjust those deploy paths at the bottom of `build.sh` for your install.

## Credits & acknowledgements

This project was studied from, and stands on, the following work. Please support the originals.

- **OptiScaler** and the **OptiScaler_DLSSNR** fork
  (<https://github.com/Dagherbou/OptiScaler_DLSSNR>, <https://github.com/optiscaler/OptiScaler>) —
  **GPL-3.0**. The DLSSNR-as-feature-18 recipe comes from here: the forwarder caller-gate trick,
  reuse of the driver core's capability parameter block, discovering the parameter setter vtable
  slots by round-tripping a value, the `DLSSNR.*` parameter names, the subrect/guide wiring, and
  the model Style names. **Because this add-on is derived from GPL-3.0 code, it is licensed
  GPL-3.0 too** (see [LICENSE](LICENSE)).
- **RenoDX** by Carlos Lopez Jr. (<https://github.com/clshortfuse/renodx>) — MIT. The ReShade
  add-on approach, the DLSS5 colour-bridge composition concepts (display-referred encode, anchored
  resolve, measured white point), and the three DLSS5 model-intensity controls.
- **ReShade** by Patrick Mours (<https://github.com/crosire/reshade>) — the add-on SDK / API this
  loads into.
- **Microsoft Detours** (<https://github.com/microsoft/Detours>) — MIT. Used to hook NGX.
- **Dear ImGui** by Omar Ocornut (<https://github.com/ocornut/imgui>) — MIT. Overlay UI.
- **NVIDIA NGX / DLSS SDK** — headers only, used under NVIDIA's SDK licence. The DLSS runtime and
  models are NVIDIA's; this add-on ships none of them.

## Disclaimer

Unofficial, unaffiliated with NVIDIA. Use of third-party add-ons in online games can carry a risk
of anti-cheat action on your account — use at your own risk.
