# addon-dlssnr-linux

A ReShade add-on that makes **NVIDIA DLSS 5 Neural Rendering** (DLSSNR — NGX feature 18)
run on **Linux / Proton**, tested in *Wuthering Waves*.

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

## Building

```bash
bash build.sh
```

Requires clang/LLD with an xwin-provided MSVC SDK (`~/.xwin`), a standalone DXC
(`~/.local/dxc`), and the RenoDX checkout beside this repo for the ReShade / DLSS / Detours /
ImGui headers. See `build.sh` for the exact paths.

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
