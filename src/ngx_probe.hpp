// NGX interception probe — milestone 2 of the DLSSNR-on-Proton addon.
// Detours-hooks NVSDK_NGX_D3D12/D3D11 CreateFeature / EvaluateFeature /
// ReleaseFeature on whichever NGX module is loaded, and logs the feature id
// plus the creation/evaluation parameters (dimensions, quality, and the
// DXGI formats of Color/Output/Depth/MV resources). Those formats are the
// ground truth the sRGB color bridge for DLSSNR (feature 18) must match.
//
// Hook target priority: _nvngx.dll (NVIDIA driver shim in the Wine prefix,
// exports the exact documented app-facing API) then nvngx_dlss.dll (snippet).
// Detours patches function bodies, not IATs, so attaching works no matter how
// the caller obtained the pointer — we can install lazily from the present
// callback once the module shows up.

#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include <d3d11.h>
#include <d3d12.h>
#include <detours.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include <reshade.hpp>

namespace ngx_probe {

inline void Log(const char* msg) {
  reshade::log::message(reshade::log::level::info, msg);
}

inline void Logf(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  reshade::log::message(reshade::log::level::info, buf);
}

inline void Warnf(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  reshade::log::message(reshade::log::level::warning, buf);
}

inline const char* FeatureName(NVSDK_NGX_Feature id) {
  switch (id) {
    case NVSDK_NGX_Feature_SuperSampling: return "SuperSampling (DLSS-SR)";
    case NVSDK_NGX_Feature_FrameGeneration: return "FrameGeneration";
    case NVSDK_NGX_Feature_RayReconstruction: return "RayReconstruction (DLSS-RR)";
    case NVSDK_NGX_Feature_Reserved18: return "Reserved18 (DLSS-NR?)";
    default: return "other";
  }
}

// --- parameter dumping -------------------------------------------------

inline void DumpInt(const NVSDK_NGX_Parameter* params, const char* name) {
  int value = 0;
  if (params->Get(name, &value) == NVSDK_NGX_Result_Success) {
    Logf("ngx-probe:   %s = %d", name, value);
  }
}

inline void DumpUInt(const NVSDK_NGX_Parameter* params, const char* name) {
  unsigned int value = 0;
  if (params->Get(name, &value) == NVSDK_NGX_Result_Success) {
    Logf("ngx-probe:   %s = %u", name, value);
  }
}

inline void DumpFloat(const NVSDK_NGX_Parameter* params, const char* name) {
  float value = 0.f;
  if (params->Get(name, &value) == NVSDK_NGX_Result_Success) {
    Logf("ngx-probe:   %s = %f", name, value);
  }
}

// ID3D12Resource::GetDesc returns D3D12_RESOURCE_DESC by value from a COM
// virtual — safe only because we build with the MSVC ABI (clang -target
// x86_64-pc-windows-msvc).
inline void DumpD3D12Resource(const NVSDK_NGX_Parameter* params, const char* name) {
  ID3D12Resource* resource = nullptr;
  if (params->Get(name, &resource) != NVSDK_NGX_Result_Success || resource == nullptr) {
    Logf("ngx-probe:   %s = <not set>", name);
    return;
  }
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  Logf("ngx-probe:   %s = %p %llux%u format=%u (DXGI_FORMAT)", name,
       static_cast<void*>(resource),
       static_cast<unsigned long long>(desc.Width), desc.Height,
       static_cast<unsigned>(desc.Format));
}

inline void DumpD3D11Resource(const NVSDK_NGX_Parameter* params, const char* name) {
  ID3D11Resource* resource = nullptr;
  if (params->Get(name, &resource) != NVSDK_NGX_Result_Success || resource == nullptr) {
    Logf("ngx-probe:   %s = <not set>", name);
    return;
  }
  ID3D11Texture2D* tex = nullptr;
  if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&tex)))) {
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    Logf("ngx-probe:   %s = %p %ux%u format=%u (DXGI_FORMAT)", name,
         static_cast<void*>(resource), desc.Width, desc.Height,
         static_cast<unsigned>(desc.Format));
    tex->Release();
  } else {
    Logf("ngx-probe:   %s = %p (not a texture2d)", name, static_cast<void*>(resource));
  }
}

inline void DumpCreateParams(const NVSDK_NGX_Parameter* params) {
  if (params == nullptr) {
    Log("ngx-probe:   <null parameter block>");
    return;
  }
  DumpUInt(params, NVSDK_NGX_Parameter_Width);
  DumpUInt(params, NVSDK_NGX_Parameter_Height);
  DumpUInt(params, NVSDK_NGX_Parameter_OutWidth);
  DumpUInt(params, NVSDK_NGX_Parameter_OutHeight);
  DumpInt(params, NVSDK_NGX_Parameter_PerfQualityValue);
  DumpInt(params, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
}

inline void DumpEvalParams(const NVSDK_NGX_Parameter* params, bool d3d12) {
  if (params == nullptr) {
    Log("ngx-probe:   <null parameter block>");
    return;
  }
  const char* resources[] = {
      NVSDK_NGX_Parameter_Color,
      NVSDK_NGX_Parameter_Output,
      NVSDK_NGX_Parameter_Depth,
      NVSDK_NGX_Parameter_MotionVectors,
      NVSDK_NGX_Parameter_ExposureTexture,
      NVSDK_NGX_Parameter_TransparencyMask,
  };
  for (const char* name : resources) {
    if (d3d12) {
      DumpD3D12Resource(params, name);
    } else {
      DumpD3D11Resource(params, name);
    }
  }
  DumpFloat(params, NVSDK_NGX_Parameter_Jitter_Offset_X);
  DumpFloat(params, NVSDK_NGX_Parameter_Jitter_Offset_Y);
  DumpFloat(params, NVSDK_NGX_Parameter_MV_Scale_X);
  DumpFloat(params, NVSDK_NGX_Parameter_MV_Scale_Y);
  DumpInt(params, NVSDK_NGX_Parameter_Reset);
}

// DLSSNR (feature 18) parameter names, learned from OptiScaler_DLSSNR's
// DlssNr_Proxy.cpp (MIT). Setters on the driver's parameter block are
// vtable-quirky there, but typed Get() works normally — so dumping works
// with the ordinary API. Tuning values are read at create; resources,
// dimensions and MV scale are (re)written every evaluate.
inline void DumpNrCreateParams(const NVSDK_NGX_Parameter* params) {
  if (params == nullptr) return;
  DumpUInt(params, "DLSSNR.Enabled");
  DumpUInt(params, "DLSSNR.Width");
  DumpUInt(params, "DLSSNR.Height");
  DumpUInt(params, "DLSSNR.Hint.Render.Preset");
  DumpFloat(params, "DLSSNR.Intensity");
  DumpUInt(params, "DLSSNR.Style");
  DumpFloat(params, "DLSSNR.LocalStructureStrength");
  DumpFloat(params, "DLSSNR.LocalToneStrength");
  DumpFloat(params, "DLSSNR.SkinStructureStrength");
  DumpUInt(params, "DLSSNR.UseAutoMask");
  DumpUInt(params, "DLSSNR.UICorrection");
}

inline void DumpNrEvalParams(const NVSDK_NGX_Parameter* params) {
  if (params == nullptr) return;
  DumpD3D12Resource(params, "DLSSNR.Color");
  DumpD3D12Resource(params, "DLSSNR.Depth");
  DumpD3D12Resource(params, "DLSSNR.MVec");
  DumpD3D12Resource(params, "DLSSNR.Output");
  DumpUInt(params, "DLSSNR.Width");
  DumpUInt(params, "DLSSNR.Height");
  DumpUInt(params, "DLSSNR.DepthInverted");
  DumpUInt(params, "DLSSNR.Reset");
  DumpUInt(params, "DLSSNR.ColorSubrectWidth");
  DumpUInt(params, "DLSSNR.ColorSubrectHeight");
  DumpUInt(params, "DLSSNR.DepthSubrectWidth");
  DumpUInt(params, "DLSSNR.DepthSubrectHeight");
  DumpFloat(params, "DLSSNR.MVecScaleX");
  DumpFloat(params, "DLSSNR.MVecScaleY");
  DumpFloat(params, "DLSSNR.Intensity");
}

}  // namespace ngx_probe

// The milestone-4 runner drives feature 18 through the forwarder; it logs via ngx_probe::Logf, so
// it is included here, between the helpers and the hooks that call into it.
#include "nr_runner.hpp"

namespace ngx_probe {

// --- hooks --------------------------------------------------------------

// Dump the first few evaluations in full, then a one-liner heartbeat.
constexpr uint32_t kFullDumps = 3;
constexpr uint32_t kHeartbeatEvery = 600;

// The Reserved18 handle, so evaluates of the NR feature get their own dump.
inline NVSDK_NGX_Handle* nr_handle = nullptr;

static decltype(&NVSDK_NGX_D3D12_CreateFeature) real_D3D12_CreateFeature = nullptr;
inline NVSDK_NGX_Result NVSDK_CONV HookD3D12CreateFeature(
    ID3D12GraphicsCommandList* cmd_list, NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out_handle) {
  Logf("ngx-probe: D3D12_CreateFeature(feature=%d [%s])",
       static_cast<int>(feature_id), FeatureName(feature_id));
  DumpCreateParams(params);
  if (feature_id == NVSDK_NGX_Feature_Reserved18) DumpNrCreateParams(params);
  const auto result = real_D3D12_CreateFeature(cmd_list, feature_id, params, out_handle);
  Logf("ngx-probe: D3D12_CreateFeature => 0x%x handle=%p", static_cast<unsigned>(result),
       (out_handle != nullptr) ? static_cast<void*>(*out_handle) : nullptr);
  if (feature_id == NVSDK_NGX_Feature_Reserved18 && result == NVSDK_NGX_Result_Success &&
      out_handle != nullptr) {
    nr_handle = *out_handle;
    Log("ngx-probe: Reserved18 (DLSS-NR) handle captured — evals will be dumped");
  }
  if (feature_id == NVSDK_NGX_Feature_SuperSampling && result == NVSDK_NGX_Result_Success &&
      params != nullptr) {
    unsigned w = 0, h = 0, ow = 0, oh = 0;
    int flags = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);
    params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);
    nr_runner::OnDlssCreate(w, h, ow, oh, flags);
  }
  return result;
}

static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_D3D12_EvaluateFeature = nullptr;
inline NVSDK_NGX_Result NVSDK_CONV HookD3D12EvaluateFeature(
    ID3D12GraphicsCommandList* cmd_list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback) {
  if (nr_handle != nullptr && handle == nr_handle) {
    static uint32_t nr_count = 0;
    const uint32_t m = ++nr_count;
    if (m <= kFullDumps) {
      Logf("ngx-probe: D3D12_EvaluateFeature[NR] #%u handle=%p", m,
           static_cast<const void*>(handle));
      DumpNrEvalParams(params);
    } else if (m % kHeartbeatEvery == 0) {
      Logf("ngx-probe: D3D12_EvaluateFeature[NR] #%u (alive)", m);
    }
    return real_D3D12_EvaluateFeature(cmd_list, handle, params, callback);
  }
  static uint32_t count = 0;
  const uint32_t n = ++count;
  if (n <= kFullDumps) {
    Logf("ngx-probe: D3D12_EvaluateFeature #%u handle=%p", n, static_cast<const void*>(handle));
    DumpEvalParams(params, /*d3d12=*/true);
  } else if (n % kHeartbeatEvery == 0) {
    Logf("ngx-probe: D3D12_EvaluateFeature #%u (alive)", n);
  }
  const auto eval_result = real_D3D12_EvaluateFeature(cmd_list, handle, params, callback);
  if (eval_result == NVSDK_NGX_Result_Success)
    nr_runner::OnDlssEvaluated(cmd_list, params);
  return eval_result;
}

static decltype(&NVSDK_NGX_D3D12_ReleaseFeature) real_D3D12_ReleaseFeature = nullptr;
inline NVSDK_NGX_Result NVSDK_CONV HookD3D12ReleaseFeature(NVSDK_NGX_Handle* handle) {
  Logf("ngx-probe: D3D12_ReleaseFeature handle=%p", static_cast<void*>(handle));
  return real_D3D12_ReleaseFeature(handle);
}

static decltype(&NVSDK_NGX_D3D11_CreateFeature) real_D3D11_CreateFeature = nullptr;
inline NVSDK_NGX_Result NVSDK_CONV HookD3D11CreateFeature(
    ID3D11DeviceContext* ctx, NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** out_handle) {
  Logf("ngx-probe: D3D11_CreateFeature(feature=%d [%s])",
       static_cast<int>(feature_id), FeatureName(feature_id));
  DumpCreateParams(params);
  return real_D3D11_CreateFeature(ctx, feature_id, params, out_handle);
}

static decltype(&NVSDK_NGX_D3D11_EvaluateFeature) real_D3D11_EvaluateFeature = nullptr;
inline NVSDK_NGX_Result NVSDK_CONV HookD3D11EvaluateFeature(
    ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback) {
  static uint32_t count = 0;
  const uint32_t n = ++count;
  if (n <= kFullDumps) {
    Logf("ngx-probe: D3D11_EvaluateFeature #%u handle=%p", n, static_cast<const void*>(handle));
    DumpEvalParams(params, /*d3d12=*/false);
  } else if (n % kHeartbeatEvery == 0) {
    Logf("ngx-probe: D3D11_EvaluateFeature #%u (alive)", n);
  }
  return real_D3D11_EvaluateFeature(ctx, handle, params, callback);
}

// --- installation --------------------------------------------------------

struct HookEntry {
  const char* export_name;
  void** real;
  void* hook;
};

inline const HookEntry kHooks[] = {
    {"NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void**>(&real_D3D12_CreateFeature), reinterpret_cast<void*>(&HookD3D12CreateFeature)},
    {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&real_D3D12_EvaluateFeature), reinterpret_cast<void*>(&HookD3D12EvaluateFeature)},
    {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void**>(&real_D3D12_ReleaseFeature), reinterpret_cast<void*>(&HookD3D12ReleaseFeature)},
    {"NVSDK_NGX_D3D11_CreateFeature", reinterpret_cast<void**>(&real_D3D11_CreateFeature), reinterpret_cast<void*>(&HookD3D11CreateFeature)},
    {"NVSDK_NGX_D3D11_EvaluateFeature", reinterpret_cast<void**>(&real_D3D11_EvaluateFeature), reinterpret_cast<void*>(&HookD3D11EvaluateFeature)},
};

inline bool installed = false;

// Try to attach to the first NGX module present in the process. Returns true
// once hooks are installed (further calls are no-ops). Cheap when nothing is
// loaded yet — safe to call every present until it succeeds.
inline bool TryInstall() {
  if (installed) return true;

  // _nvngx.dll first: the driver shim exports the documented app-facing API,
  // so our header signatures are exact. Snippet DLLs are the fallback.
  const char* candidates[] = {"_nvngx.dll", "nvngx.dll", "nvngx_dlss.dll", "nvngx_dlssd.dll"};

  HMODULE module = nullptr;
  const char* module_name = nullptr;
  for (const char* candidate : candidates) {
    module = GetModuleHandleA(candidate);
    if (module != nullptr && GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature") != nullptr) {
      module_name = candidate;
      break;
    }
    module = nullptr;
  }
  if (module == nullptr) return false;

  if (DetourTransactionBegin() != NO_ERROR) {
    Log("ngx-probe: DetourTransactionBegin failed");
    return false;
  }
  DetourUpdateThread(GetCurrentThread());

  int attached = 0;
  for (const auto& entry : kHooks) {
    FARPROC proc = GetProcAddress(module, entry.export_name);
    if (proc == nullptr) {
      Logf("ngx-probe: %s not exported by %s", entry.export_name, module_name);
      continue;
    }
    // Never patch over a jmp that is already there. If a detour is still in
    // place, Detours copies that jmp into the new trampoline as if it were the
    // original prologue, and calling the trampoline lands back in the detour
    // region -- a cycle. Uninstall() below is what normally prevents this; this
    // check is the backstop.
    if (*reinterpret_cast<const unsigned char*>(proc) == 0xe9) {
      Logf("ngx-probe: %s already begins with a jmp - refusing to stack a detour",
           entry.export_name);
      continue;
    }
    *entry.real = reinterpret_cast<void*>(proc);
    if (DetourAttach(entry.real, entry.hook) != NO_ERROR) {
      Logf("ngx-probe: DetourAttach failed for %s", entry.export_name);
      *entry.real = nullptr;
      continue;
    }
    ++attached;
  }

  if (DetourTransactionCommit() != NO_ERROR) {
    Log("ngx-probe: DetourTransactionCommit failed");
    DetourTransactionAbort();
    return false;
  }

  if (attached == 0) return false;
  installed = true;
  Logf("ngx-probe: hooked %d NGX exports on %s", attached, module_name);
  return true;
}


// Detach everything TryInstall attached, and let a later load re-attach cleanly.
//
// ReShade loads and unloads an add-on several times while the device is being
// created -- six register/unregister cycles in GTA V Enhanced under Proton.
// DllMain(DLL_PROCESS_DETACH) unregistered from ReShade but left the detours in
// place, and because the DLL itself is unloaded the `installed` flag went back
// to false, so the next load patched the export again, on top of its own detour.
//
// From the third install the exported entry read
//
//     e9 63 01 fd bf  57 41 56 41 57 ...      (a jmp, not the prologue)
//
// and the trampoline Detours built from it began with that copied jmp, so
// calling the trampoline re-entered an older hook. Depth reached 8000 in one
// millisecond and the game hung.
inline void Uninstall() {
  if (!installed) return;
  if (DetourTransactionBegin() != NO_ERROR) {
    Log("ngx-probe: DetourTransactionBegin failed on uninstall");
    return;
  }
  DetourUpdateThread(GetCurrentThread());
  int detached = 0;
  for (const auto& entry : kHooks) {
    if (*entry.real == nullptr) continue;
    if (DetourDetach(entry.real, entry.hook) != NO_ERROR) {
      Logf("ngx-probe: DetourDetach failed for %s", entry.export_name);
      continue;
    }
    ++detached;
  }
  if (DetourTransactionCommit() != NO_ERROR) {
    Log("ngx-probe: DetourTransactionCommit failed on uninstall");
    DetourTransactionAbort();
    return;
  }
  for (const auto& entry : kHooks) *entry.real = nullptr;
  installed = false;
  Logf("ngx-probe: detached %d NGX hooks", detached);
}

}  // namespace ngx_probe
