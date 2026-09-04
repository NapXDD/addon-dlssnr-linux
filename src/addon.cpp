// DLSSNR Linux — ReShade addon entry point.
// Registers the overlay and present hook, logs swapchain ground truth
// (color space, back buffer format, dimensions, Wine/Proton detection),
// and installs the NGX CreateFeature/EvaluateFeature interception
// (ngx_probe.hpp) that the runner and colour bridge are driven by.

#include <windows.h>

#include <cstdio>

#pragma comment(lib, "user32")  // GetAsyncKeyState for the F10 toggle

// ImGui before reshade.hpp so the overlay function table lights up; ImTextureID must be 8 bytes
// to match reshade::api::resource_view.
#define ImTextureID ImU64
#define ImDrawIdx unsigned int
#include <imgui.h>

#include <reshade.hpp>

#include "ngx_probe.hpp"

namespace {

const char* ColorSpaceName(reshade::api::color_space cs) {
  switch (cs) {
    case reshade::api::color_space::unknown: return "unknown (treat as SDR sRGB)";
    case reshade::api::color_space::srgb: return "srgb (SDR)";
    case reshade::api::color_space::scrgb: return "scrgb (linear HDR BT.709)";
    case reshade::api::color_space::hdr10_pq: return "hdr10_pq (PQ BT.2020)";
    case reshade::api::color_space::hdr10_hlg: return "hdr10_hlg (HLG BT.2020)";
    default: return "unrecognized";
  }
}

void LogWineVersion() {
  char buf[256];
  const auto* ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto wine_get_version = reinterpret_cast<const char* (*)()>(
      GetProcAddress((HMODULE)ntdll, "wine_get_version"));
  if (wine_get_version != nullptr) {
    std::snprintf(buf, sizeof(buf), "environment: Wine/Proton %s", wine_get_version());
  } else {
    std::snprintf(buf, sizeof(buf), "environment: native Windows");
  }
  reshade::log::message(reshade::log::level::info, buf);
}

// get_resource_desc returns a large struct by value from a virtual method —
// only ABI-safe because we build with clang targeting x86_64-pc-windows-msvc
// (matching MSVC-built ReShade). A MinGW GCC build crashes on this call.
void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  auto* device = swapchain->get_device();
  const auto back_buffer = swapchain->get_back_buffer(0);
  const auto desc = device->get_resource_desc(back_buffer);
  const auto color_space = swapchain->get_color_space();

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "swapchain %s: %ux%u format=%u color_space=%u [%s] buffers=%u api=%u",
                resize ? "resized" : "created",
                desc.texture.width, desc.texture.height,
                static_cast<unsigned>(desc.texture.format),
                static_cast<unsigned>(color_space), ColorSpaceName(color_space),
                swapchain->get_back_buffer_count(),
                static_cast<unsigned>(device->get_api()));
  reshade::log::message(reshade::log::level::info, buf);
}

// The NGX module (_nvngx.dll / nvngx_dlss.dll) loads well after the addon,
// so retry the Detours attach on every present until it lands. Detours
// patches function bodies, so attaching after the game cached the function
// pointers still works.
void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*,
               const reshade::api::rect*, const reshade::api::rect*,
               uint32_t, const reshade::api::rect*) {
  ngx_probe::TryInstall();

  // F10 toggles the NR pass, edge-triggered, for instant A/B comparison.
  static bool f10_was_down = false;
  const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  if (f10_down && !f10_was_down) nr_runner::SetEnabled(!nr_runner::IsEnabled());
  f10_was_down = f10_down;
}

// A slider with a reset-to-default button on its right.
bool SliderWithReset(const char* label, float* value, float min, float max, const char* fmt,
                     float default_value) {
  ImGui::PushID(label);
  bool changed = ImGui::SliderFloat(label, value, min, max, fmt);
  ImGui::SameLine();
  if (ImGui::SmallButton("reset")) {
    *value = default_value;
    changed = true;
  }
  ImGui::PopID();
  return changed;
}

// The settings panel, under ReShade's Add-ons tab. Compose controls apply live; the model's own
// settings are read once at feature creation, so they take a rebuild (the Apply button).
void OnDrawOverlay(reshade::api::effect_runtime*) {
  auto& s = nr_runner::s;

  bool enabled = nr_runner::IsEnabled();
  if (ImGui::Checkbox("Enable Neural Rendering (F10)", &enabled)) nr_runner::SetEnabled(enabled);

  SliderWithReset("Detail strength", &s.transfer, 0.0f, 1.5f, "%.2f", 1.0f);
  SliderWithReset("Colour strength", &s.colour_strength, 0.0f, 1.0f, "%.2f", 0.0f);
  SliderWithReset("Highlight guard", &s.max_ratio, 1.0f, 8.0f, "%.1fx", 2.0f);
  SliderWithReset("White point scale", &s.wp_scale, 0.1f, 4.0f, "%.2f", 1.0f);
  if (s.wp_ema > 0.0f) {
    ImGui::Text("Measured white point: %.4f", s.wp_ema);
  } else {
    ImGui::TextUnformatted("Measured white point: (measuring...)");
  }

  ImGui::Combo("Debug view", &s.debug_view,
               "Off\0What the model sees\0Model answer\0Difference x20\0");

  ImGui::Separator();
  ImGui::TextUnformatted("Model settings (need Apply, rebuilds the feature)");
  // The three DLSSNR intensities, as in the RenoDX DLSS5 addon. NR intensity is the model's overall
  // hand; Structure intensity drives the fine detail it synthesises; Global intensity shapes the
  // broad local contrast/tone. All default to 1.0 (the model's neutral).
  SliderWithReset("NR intensity", &s.intensity, 0.0f, 2.0f, "%.2f", 1.0f);
  SliderWithReset("Structure intensity", &s.local_structure, 0.0f, 2.0f, "%.2f", 1.0f);
  SliderWithReset("Global intensity", &s.local_tone, 0.0f, 2.0f, "%.2f", 1.0f);
  ImGui::SliderInt("Preset", &s.preset, 0, 7);
  ImGui::SameLine();
  ImGui::PushID("preset-reset");
  if (ImGui::SmallButton("reset")) s.preset = 0;
  ImGui::PopID();

  // The model's own processing profiles (DLSSNR.Style). Names from community testing via
  // OptiScaler_DLSSNR -- NVIDIA ships none. Default boosts local contrast hardest and can look
  // stylised; Natural is the same detail with a gentler hand; Cinematic tones down the shine for
  // a film-like look.
  if (s.style > 2) s.style = 2;
  ImGui::Combo("Style", &s.style, "Default (standard)\0Natural\0Cinematic\0");
  ImGui::SameLine();
  ImGui::PushID("style-reset");
  if (ImGui::SmallButton("reset")) s.style = 0;
  ImGui::PopID();

  if (ImGui::Button("Apply model settings")) nr_runner::ApplyModelSettings();
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "DLSSNR Linux";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Runs NVIDIA DLSS 5 Neural Rendering (NGX feature 18) under Linux/Proton by driving the "
    "game-local snippet directly, with a display-referred colour bridge";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::log::message(reshade::log::level::info,
                            "DLSSNR Linux loaded (clang x86_64-pc-windows-msvc, built on Linux)");
      LogWineVersion();
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay("DLSSNR Linux", OnDrawOverlay);
      ngx_probe::TryInstall();  // in case NGX is already resident
      break;
    case DLL_PROCESS_DETACH:
      nr_runner::Shutdown();
      reshade::unregister_overlay("DLSSNR Linux", OnDrawOverlay);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_addon(h_module);
      break;
  }
  return TRUE;
}
