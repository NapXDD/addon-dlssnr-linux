// Milestone 5a: the visible loop. The DLSSNR model runs once per frame off the game's DLSS-SR
// output and its answer is copied back over that output, so the game's own post-processing and
// tone-mapping consume the enhanced frame. F10 toggles the pass for an instant A/B.
//
// (Milestone 4 proved the mechanism: the game-local nvngx_dlssnr.dll is loaded directly through a
// forwarder DLL whose name contains "nvngx.dll" -- the snippet's caller gate -- bypassing driver
// dispatch, which fails FAIL_OutOfDate under Proton because the NGX OTA updater is unavailable.)
//
// Recipe from OptiScaler_DLSSNR (MIT). The parameter block is the driver core's own capability
// block -- a freshly allocated one lacks the snippet/preset callbacks and create fails
// UnableToInitializeFeature. Its setters are not laid out the way the SDK header declares: the
// 64-bit setter is vtable slot 0 (resources go through it), the uint setter slot 3, and the float
// setter's slot is discovered by round-tripping a value (typed Get() works normally).
//
// This stage feeds the model the linear frame as-is. The display-referred encode with a measured
// white point -- the actual color bridge -- is stage 5b; the F10 A/B this stage provides is how
// that work gets judged.
//
// Inputs, from the fork and our WuWa capture: Color and Output at display resolution, Depth and
// MVec at render resolution (the guides the game hands DLSS), each with its own subrect; MV scale
// passed through from the game, never derived.

#pragma once

#include <windows.h>

#include <cstdint>

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include "nr_compose.hpp"

namespace nr_runner {

// --- driver parameter block, driven through its vtable ------------------

constexpr int kVtSetUll = 0;
constexpr int kVtSetUint = 3;

using PFN_SetUll = void(__thiscall*)(void*, const char*, unsigned long long);
using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);
using PFN_SetUint = void(__thiscall*)(void*, const char*, unsigned int);

inline int float_slot = -1;

inline void SetUInt(void* params, const char* name, unsigned int v) {
  void** vt = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetUint>(vt[kVtSetUint])(params, name, v);
}

inline void SetFloat(void* params, const char* name, float v) {
  void** vt = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetFloat>(vt[float_slot < 0 ? 1 : float_slot])(params, name, v);
}

inline void SetResource(void* params, const char* name, ID3D12Resource* v) {
  void** vt = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetUll>(vt[kVtSetUll])(params, name, (unsigned long long)v);
}

// Find the float slot by writing a known value through each candidate setter and reading it back
// with the typed getter. 0.3125 is exact in binary, so the comparison is clean.
inline void DiscoverFloatSlot(NVSDK_NGX_Parameter* params) {
  if (float_slot >= 0) return;
  void** vt = *reinterpret_cast<void***>(params);
  for (int slot = 0; slot < 8; ++slot) {
    const float probe = 0.3125f;
    float read_back = 0.0f;
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, "NRProbe.Float", probe);
    if (params->Get("NRProbe.Float", &read_back) == NVSDK_NGX_Result_Success && read_back == probe) {
      float_slot = slot;
      ngx_probe::Logf("nr-fwd: float parameters go through vtable slot %d", slot);
      return;
    }
  }
  float_slot = 1;  // the header's answer, as a last resort
  ngx_probe::Log("nr-fwd: no float slot round-tripped; falling back to the header's slot 1");
}

// --- state ---------------------------------------------------------------

using PFN_FwdInit = int(__cdecl*)(const wchar_t*, const wchar_t*, ID3D12Device*, void*);
using PFN_FwdCreate = void*(__cdecl*)(ID3D12GraphicsCommandList*, void*, int*);
using PFN_FwdEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*);
using PFN_FwdRelease = void(__cdecl*)(void*);
using PFN_GetCapabilityParams = int(__cdecl*)(NVSDK_NGX_Parameter**);

constexpr uint32_t kFullLogs = 3;        // first evals logged in full
constexpr uint32_t kHeartbeatEvery = 600;
constexpr uint32_t kMaxFailStreak = 3;   // consecutive eval failures before the pass disables itself
constexpr int kGraveyardFrames = 240;    // frames a retired resource waits before release

struct Grave {
  ID3D12Resource* resource = nullptr;
  void* feature = nullptr;
  int frames = 0;
};

struct State {
  bool gave_up = false;
  bool enabled = true;

  HMODULE forwarder = nullptr;
  PFN_FwdInit init = nullptr;
  PFN_FwdCreate create = nullptr;
  PFN_FwdEvaluate evaluate = nullptr;
  PFN_FwdRelease release = nullptr;

  NVSDK_NGX_Parameter* caps = nullptr;
  bool snippet_inited = false;
  void* feature = nullptr;
  ID3D12Resource* output = nullptr;      // the model's answer
  ID3D12Resource* color_copy = nullptr;  // the original, copied aside for the resolve
  ID3D12Resource* proxy = nullptr;       // the display-referred picture the model is shown
  DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;

  // Compose controls (fork semantics: transfer 0 = bypass, 1 = the model's picture; max_ratio
  // caps how far the model may move any pixel's luminance either way; colour 0 keeps the game's
  // own hue exactly -- only brightness carries the model's verdict).
  float transfer = 1.0f;
  float max_ratio = 2.0f;
  float colour_strength = 0.0f;

  // White point: the measured log-average luminance, EMA-smoothed; -1 until the first reading
  // lands. wp_scale is the user's paper-white multiplier on top.
  float wp_ema = -1.0f;
  float wp_scale = 1.0f;

  // Debug view: 0 off, 1 proxy, 2 model answer, 3 difference x20.
  int debug_view = 0;

  // Model tuning, read once when the feature is built; changing them retires the feature.
  // The three strengths mirror the RenoDX DLSS5 addon's intensities: intensity is the model's
  // overall hand (DLSSNR.Intensity), local_structure is the fine-detail synthesis
  // (LocalStructureStrength), local_tone is the broad local-contrast/tone shaping (LocalToneStrength).
  float intensity = 1.0f;
  float local_structure = 1.0f;
  float local_tone = 1.0f;
  int preset = 0;
  int style = 0;

  // Geometry of the game's current DLSS-SR feature.
  unsigned render_w = 0, render_h = 0;
  unsigned out_w = 0, out_h = 0;
  int create_flags = 0;
  // Geometry the NR feature was built against; a mismatch retires it.
  unsigned feature_w = 0, feature_h = 0;

  uint32_t eval_count = 0;
  uint32_t fail_streak = 0;

  Grave graveyard[8] = {};

  wchar_t game_dir[MAX_PATH] = {};
};

inline State s;

inline const char* ResultName(int r) {
  switch ((unsigned)r) {
    case 0x1: return "Success";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    default: return "other";
  }
}

inline bool GiveUp(const char* why) {
  ngx_probe::Logf("nr-fwd: giving up -- %s", why);
  s.gave_up = true;
  return false;
}

// --- lifetime ------------------------------------------------------------

// The GPU may still be executing command lists that reference a retired feature or texture, so
// they wait in the graveyard for a couple hundred frames before release.
inline void Bury(ID3D12Resource* resource, void* feature) {
  for (auto& g : s.graveyard) {
    if (g.resource == nullptr && g.feature == nullptr) {
      g.resource = resource;
      g.feature = feature;
      g.frames = kGraveyardFrames;
      return;
    }
  }
  // Graveyard full: release the oldest-style way -- immediately. Better a rare hazard than a leak.
  if (resource != nullptr) resource->Release();
  if (feature != nullptr && s.release != nullptr) s.release(feature);
}

inline void TickGraveyard() {
  for (auto& g : s.graveyard) {
    if (g.resource == nullptr && g.feature == nullptr) continue;
    if (--g.frames > 0) continue;
    if (g.resource != nullptr) g.resource->Release();
    if (g.feature != nullptr && s.release != nullptr) s.release(g.feature);
    g = {};
  }
}

inline void RetireFeature() {
  if (s.feature == nullptr && s.output == nullptr) return;
  ngx_probe::Log("nr-fwd: retiring NR feature (geometry changed)");
  Bury(s.output, s.feature);
  Bury(s.color_copy, nullptr);
  Bury(s.proxy, nullptr);
  s.feature = nullptr;
  s.output = nullptr;
  s.color_copy = nullptr;
  s.proxy = nullptr;
  s.output_format = DXGI_FORMAT_UNKNOWN;
  s.feature_w = s.feature_h = 0;
}

// Remember the game's DLSS-SR geometry; the NR feature is built against the display resolution and
// its guides against the render resolution.
inline void OnDlssCreate(unsigned w, unsigned h, unsigned ow, unsigned oh, int flags) {
  s.render_w = w;
  s.render_h = h;
  s.out_w = ow;
  s.out_h = oh;
  s.create_flags = flags;
  if (s.feature != nullptr && (s.feature_w != ow || s.feature_h != oh)) RetireFeature();
}

// Process teardown: pointers in the shared block must not outlive us.
inline void Shutdown() {
  if (s.caps != nullptr) {
    void* p = s.caps;
    SetUInt(p, "DLSSNR.Enabled", 0u);
    SetResource(p, "DLSSNR.Color", nullptr);
    SetResource(p, "DLSSNR.Depth", nullptr);
    SetResource(p, "DLSSNR.MVec", nullptr);
    SetResource(p, "DLSSNR.Output", nullptr);
  }
  if (s.feature != nullptr && s.release != nullptr) s.release(s.feature);
  s.feature = nullptr;
  if (s.output != nullptr) s.output->Release();
  s.output = nullptr;
  if (s.color_copy != nullptr) s.color_copy->Release();
  s.color_copy = nullptr;
  if (s.proxy != nullptr) s.proxy->Release();
  s.proxy = nullptr;
  for (auto& g : s.graveyard) {
    if (g.resource != nullptr) g.resource->Release();
    if (g.feature != nullptr && s.release != nullptr) s.release(g.feature);
    g = {};
  }
  nr_compose::Release();
}

inline void SetEnabled(bool on) {
  if (s.enabled == on) return;
  s.enabled = on;
  s.fail_streak = 0;
  ngx_probe::Logf("nr-fwd: pass %s", on ? "ENABLED" : "DISABLED");
}

inline bool IsEnabled() { return s.enabled; }

// --- setup ---------------------------------------------------------------

inline bool EnsureSetup(ID3D12GraphicsCommandList* cmd) {
  if (s.caps != nullptr && s.forwarder != nullptr) return true;

  // Everything lives beside the game exe: the forwarder, the model, and our data path.
  if (s.game_dir[0] == 0) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash == nullptr) return GiveUp("could not resolve the game directory");
    *slash = 0;
    wcscpy_s(s.game_dir, exe);
  }

  if (s.forwarder == nullptr) {
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\nvngx.dll_nrfwd.dll", s.game_dir);
    s.forwarder = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (s.forwarder == nullptr) return GiveUp("nvngx.dll_nrfwd.dll not found beside the game exe");
    s.init = (PFN_FwdInit)GetProcAddress(s.forwarder, "nrfwd_init");
    s.create = (PFN_FwdCreate)GetProcAddress(s.forwarder, "nrfwd_create");
    s.evaluate = (PFN_FwdEvaluate)GetProcAddress(s.forwarder, "nrfwd_evaluate");
    s.release = (PFN_FwdRelease)GetProcAddress(s.forwarder, "nrfwd_release");
    if (s.init == nullptr || s.create == nullptr || s.evaluate == nullptr || s.release == nullptr)
      return GiveUp("forwarder exports did not resolve");
    ngx_probe::Log("nr-fwd: forwarder loaded");
  }

  if (s.caps == nullptr) {
    // The driver core's capability block. The game's own DLSS already initialised the core, so this
    // is a read of existing state, not an init.
    HMODULE nvngx = GetModuleHandleA("_nvngx.dll");
    if (nvngx == nullptr) return GiveUp("_nvngx.dll not resident");
    auto get_caps =
        (PFN_GetCapabilityParams)GetProcAddress(nvngx, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    if (get_caps == nullptr) return GiveUp("GetCapabilityParameters not exported by _nvngx.dll");
    if (get_caps(&s.caps) != NVSDK_NGX_Result_Success || s.caps == nullptr) {
      s.caps = nullptr;
      return GiveUp("the NGX core refused its capability parameters");
    }
    DiscoverFloatSlot(s.caps);
  }

  return true;
}

// Three display-resolution textures in the game's own DLSS output format (accepted by the model
// in milestone 5a): the model's answer, the copied-aside original, and the encoded proxy.
inline bool CreateTextures(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_output) {
  if (s.output != nullptr) return true;
  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    return GiveUp("could not reach the device from the command list");

  const D3D12_RESOURCE_DESC game_desc = game_output->GetDesc();

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = s.out_w;
  desc.Height = s.out_h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = game_desc.Format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  // colorCopy must match the game's output format exactly (CopyResource); the proxy and the
  // model's answer go FP16 -- R11G11B10's 5-bit blue mantissa rounds the encode toward green.
  struct {
    ID3D12Resource** texture;
    DXGI_FORMAT format;
  } textures[3] = {
      {&s.output, DXGI_FORMAT_R16G16B16A16_FLOAT},
      {&s.color_copy, game_desc.Format},
      {&s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT},
  };
  bool ok = true;
  for (auto& t : textures) {
    desc.Format = t.format;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(t.texture)))) {
      ok = false;
      break;
    }
  }
  device->Release();
  if (!ok) {
    if (s.output != nullptr) s.output->Release();
    if (s.color_copy != nullptr) s.color_copy->Release();
    s.output = s.color_copy = s.proxy = nullptr;
    return GiveUp("texture creation failed");
  }
  s.output_format = game_desc.Format;
  ngx_probe::Logf("nr-fwd: textures %ux%u format=%u (answer, original, proxy)", s.out_w, s.out_h,
                  (unsigned)s.output_format);
  return true;
}

// Model tuning, all defaults. Read at create, re-written at evaluate because the block is shared
// with the game's own DLSS, which overwrites values between frames.
inline void WriteTuning(void* p) {
  SetFloat(p, "DLSSNR.Intensity", s.intensity);
  SetUInt(p, "DLSSNR.Style", (unsigned)s.style);
  SetFloat(p, "DLSSNR.LocalStructureStrength", s.local_structure);
  SetFloat(p, "DLSSNR.LocalToneStrength", s.local_tone);
  SetFloat(p, "DLSSNR.SkinStructureStrength", -1.0f);
  SetUInt(p, "DLSSNR.UseAutoMask", 1u);
}

// Called by the overlay after the create-time model settings changed: the model reads them only
// while building the feature, so it is retired and rebuilt on the next frame.
inline void ApplyModelSettings() { RetireFeature(); }

inline bool EnsureFeature(ID3D12GraphicsCommandList* cmd) {
  if (s.feature != nullptr) return true;

  if (!s.snippet_inited) {
    wchar_t snippet[MAX_PATH] = {};
    swprintf_s(snippet, L"%s\\nvngx_dlssnr.dll", s.game_dir);
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
      return GiveUp("could not reach the device for init");
    const int init_result = s.init(snippet, s.game_dir, device, s.caps);
    device->Release();
    ngx_probe::Logf("nr-fwd: snippet init => 0x%x (%s)", (unsigned)init_result,
                    ResultName(init_result));
    if (init_result != 1) return GiveUp("snippet init failed");
    s.snippet_inited = true;
  }

  void* p = s.caps;
  SetUInt(p, "DLSSNR.Enabled", 1u);
  SetUInt(p, "DLSSNR.Width", s.out_w);
  SetUInt(p, "DLSSNR.Height", s.out_h);
  SetUInt(p, "CreationNodeMask", 1u);
  SetUInt(p, "VisibilityNodeMask", 1u);
  SetUInt(p, "DLSSNR.Hint.Render.Preset", (unsigned)s.preset);
  SetUInt(p, "DLSSNR.UICorrection", 1u);
  WriteTuning(p);

  int create_result = 0;
  s.feature = s.create(cmd, s.caps, &create_result);
  ngx_probe::Logf("nr-fwd: CreateFeature(18) => 0x%x (%s) handle=%p", (unsigned)create_result,
                  ResultName(create_result), s.feature);
  if (s.feature == nullptr) return GiveUp("feature creation failed");
  s.feature_w = s.out_w;
  s.feature_h = s.out_h;
  // Evaluate from the next frame on, so the init work this create recorded executes first.
  return false;
}

// --- the per-frame pass ---------------------------------------------------

// If the model fails after RecordPre already ran, put every state back so the frame proceeds
// with the original picture untouched.
inline void AbortAfterPre(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out) {
  nr_compose::Barrier(cmd, game_out, D3D12_RESOURCE_STATE_COPY_SOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  nr_compose::Barrier(cmd, s.color_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  nr_compose::Barrier(cmd, s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

inline void Evaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* game_params) {
  // The game's DLSS output is the frame; the model sees its encoded proxy, and the game's DLSS
  // guides are the model's guides.
  ID3D12Resource* color = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  game_params->Get(NVSDK_NGX_Parameter_Output, &color);
  game_params->Get(NVSDK_NGX_Parameter_Depth, &depth);
  game_params->Get(NVSDK_NGX_Parameter_MotionVectors, &motion);
  if (color == nullptr || depth == nullptr || motion == nullptr) return;

  if (!CreateTextures(cmd, color)) return;

  // Fold the latest luminance reading into the smoothed white point. The reading lags a few
  // frames, which the EMA absorbs; until the first one lands, 1.0 preserves 5b behaviour.
  const float raw_wp = nr_compose::ReadWhitePoint(s.eval_count);
  if (raw_wp > 0.0f && raw_wp == raw_wp) {
    s.wp_ema = (s.wp_ema <= 0.0f) ? raw_wp : s.wp_ema + 0.05f * (raw_wp - s.wp_ema);
  }
  float wp = (s.wp_ema > 0.0f) ? s.wp_ema : 1.0f;
  wp = (wp < 0.01f ? 0.01f : (wp > 1000.0f ? 1000.0f : wp)) * s.wp_scale;

  // Copy the original aside, encode the display-referred proxy, record the measurement.
  if (!nr_compose::RecordPre(cmd, color, s.color_copy, s.proxy, s.out_w, s.out_h, s.eval_count,
                             wp)) {
    GiveUp("compose pipeline initialisation failed");
    return;
  }

  float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
  game_params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mv_scale_x);
  game_params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mv_scale_y);
  int reset = 0;
  game_params->Get(NVSDK_NGX_Parameter_Reset, &reset);
  const bool depth_inverted =
      (s.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

  void* p = s.caps;
  SetResource(p, "DLSSNR.Color", s.proxy);
  SetResource(p, "DLSSNR.Depth", depth);
  SetResource(p, "DLSSNR.MVec", motion);
  SetResource(p, "DLSSNR.Output", s.output);

  SetUInt(p, "DLSSNR.Enabled", 1u);
  SetUInt(p, "DLSSNR.Width", s.out_w);
  SetUInt(p, "DLSSNR.Height", s.out_h);
  SetUInt(p, "DLSSNR.DepthInverted", depth_inverted ? 1u : 0u);
  SetUInt(p, "DLSSNR.Reset", reset != 0 ? 1u : 0u);

  SetUInt(p, "DLSSNR.ColorSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.ColorSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.ColorSubrectWidth", s.out_w);
  SetUInt(p, "DLSSNR.ColorSubrectHeight", s.out_h);
  SetUInt(p, "DLSSNR.OutputSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.OutputSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.OutputSubrectWidth", s.out_w);
  SetUInt(p, "DLSSNR.OutputSubrectHeight", s.out_h);
  SetUInt(p, "DLSSNR.DepthSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.DepthSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.DepthSubrectWidth", s.render_w);
  SetUInt(p, "DLSSNR.DepthSubrectHeight", s.render_h);
  SetUInt(p, "DLSSNR.MVecSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.MVecSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.MVecSubrectWidth", s.render_w);
  SetUInt(p, "DLSSNR.MVecSubrectHeight", s.render_h);

  SetFloat(p, "DLSSNR.MVecScaleX", mv_scale_x);
  SetFloat(p, "DLSSNR.MVecScaleY", mv_scale_y);
  WriteTuning(p);

  const uint32_t n = ++s.eval_count;
  const int result = s.evaluate(cmd, s.feature, s.caps);

  if (n <= kFullLogs || n % kHeartbeatEvery == 0) {
    ngx_probe::Logf("nr-fwd: EvaluateFeature #%u => 0x%x (%s)", n, (unsigned)result,
                    ResultName(result));
  }

  if (result != 1) {
    AbortAfterPre(cmd, color);
    if (++s.fail_streak >= kMaxFailStreak) {
      ngx_probe::Logf("nr-fwd: %u consecutive failures (last: %s) -- pass disabled",
                      s.fail_streak, ResultName(result));
      s.enabled = false;
    }
    return;
  }
  s.fail_streak = 0;

  // Anchor the model's answer to the original and write the blend into the game's output, which
  // its post-processing reads next.
  nr_compose::RecordPost(cmd, color, s.color_copy, s.proxy, s.output, s.out_w, s.out_h,
                         s.eval_count, s.transfer, s.max_ratio, s.colour_strength, wp,
                         (uint32_t)s.debug_view);
}

// Called after every successful game DLSS-SR evaluate, on the game's own command list.
inline void OnDlssEvaluated(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* game_params) {
  if (s.gave_up || cmd == nullptr || game_params == nullptr) return;
  if (s.out_w == 0 || s.out_h == 0) return;  // no DLSS-SR create seen yet

  TickGraveyard();
  if (!s.enabled) return;

  if (!EnsureSetup(cmd)) return;
  if (!EnsureFeature(cmd)) return;
  Evaluate(cmd, game_params);
}

}  // namespace nr_runner
