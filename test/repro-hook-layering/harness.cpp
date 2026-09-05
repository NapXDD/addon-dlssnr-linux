// Repro harness for issue #3 / PR #4: NGX detours leak across addon unload.
//
// Pretends to be ReShade (exports the addon API the reshade.hpp header looks
// up by module enumeration), loads a stub _nvngx.dll, then cycles
// LoadLibrary/FreeLibrary on dlssnr-linux.addon64 the way ReShade cycles
// addons during device creation. After each unload it inspects the first byte
// of NVSDK_NGX_D3D12_CreateFeature; finally it calls the export to
// demonstrate the user-visible failure.
//
// Exit codes:
//   0  = clean: hooks detached on unload, final call reached the stub (fixed)
//   42 = REPRO: re-entrant hook loop (watchdog tripped at depth 50)
//   43 = REPRO: calling the export faulted (detour into freed memory)
//   44 = REPRO: detour leaked past unload (static evidence only)
//   2  = harness setup failure

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

// --- ReShade addon API stubs (found via K32EnumProcessModules) -------------

static volatile LONG g_create_log_depth = 0;
static bool g_watch = false;

extern "C" {

__declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t api_version) {
  std::printf("[host] ReShadeRegisterAddon (api %u)\n", api_version);
  return true;
}
__declspec(dllexport) void ReShadeUnregisterAddon(void*) {
  std::printf("[host] ReShadeUnregisterAddon\n");
}
__declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::printf("[addon] %s\n", message);
  std::fflush(stdout);
  if (g_watch && std::strstr(message, "D3D12_CreateFeature(feature=") != nullptr) {
    if (InterlockedIncrement(&g_create_log_depth) >= 50) {
      std::printf("\nREPRO: re-entrant hook loop — CreateFeature re-entered 50 times in one call\n");
      std::fflush(stdout);
      TerminateProcess(GetCurrentProcess(), 42);
    }
  }
}
// The addon is built with ImGui, so register_addon also insists on a non-null
// function table. The overlay callback is never invoked here, so the table's
// contents are never read.
__declspec(dllexport) void* ReShadeGetImGuiFunctionTable(uint32_t version) {
  static char dummy_table[65536];
  std::printf("[host] ReShadeGetImGuiFunctionTable (imgui %u)\n", version);
  return dummy_table;
}
__declspec(dllexport) void ReShadeRegisterEvent(uint32_t, void*) {}
__declspec(dllexport) void ReShadeUnregisterEvent(uint32_t, void*) {}
__declspec(dllexport) void ReShadeRegisterOverlay(const char*, void*) {}
__declspec(dllexport) void ReShadeUnregisterOverlay(const char*, void*) {}
__declspec(dllexport) bool ReShadeGetConfigValue(void*, void*, const char*, const char*, char*,
                                                 size_t* value_size) {
  if (value_size != nullptr) *value_size = 0;
  return false;
}
__declspec(dllexport) void ReShadeSetConfigValue(void*, void*, const char*, const char*,
                                                 const char*) {}

}  // extern "C"

// --- fake NGX parameter object (every Get fails soft) ----------------------

struct FakeParams : NVSDK_NGX_Parameter {
  void Set(const char*, unsigned long long) override {}
  void Set(const char*, float) override {}
  void Set(const char*, double) override {}
  void Set(const char*, unsigned int) override {}
  void Set(const char*, int) override {}
  void Set(const char*, ID3D11Resource*) override {}
  void Set(const char*, ID3D12Resource*) override {}
  void Set(const char*, void*) override {}
  NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, unsigned int*) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, int*) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_Fail; }
  NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Fail; }
  void Reset() override {}
};

// --- helpers ---------------------------------------------------------------

static void DumpProlog(const char* when, const void* fn) {
  const auto* b = static_cast<const unsigned char*>(fn);
  std::printf("[host] %s: %p bytes = %02x %02x %02x %02x %02x %02x %02x %02x",
              when, fn, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
  if (b[0] == 0xe9) {
    const auto rel = *reinterpret_cast<const int32_t*>(b + 1);
    std::printf("  -> jmp %p", static_cast<const void*>(b + 5 + rel));
  }
  std::printf("\n");
}

using PfnCreate = NVSDK_NGX_Result(__cdecl*)(void*, int, NVSDK_NGX_Parameter*, void**);

static int GuardedCreateCall(PfnCreate create, NVSDK_NGX_Parameter* params) {
  void* handle = nullptr;
  __try {
    const NVSDK_NGX_Result r = create(nullptr, NVSDK_NGX_Feature_RayReconstruction, params, &handle);
    std::printf("[host] CreateFeature returned 0x%x, handle=%p\n",
                static_cast<unsigned>(r), handle);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    std::printf("\nREPRO: CreateFeature faulted (exception 0x%08lx) — detour into freed memory\n",
                GetExceptionCode());
    return 1;
  }
}

// --pretend-leak: simulate a previous load that was unloaded without
// detaching, by pre-setting the leak marker the addon keeps in a named
// mapping (see ngx_probe::AttachedMarker). The addon must refuse to hook.
static int PretendLeak() {
  char name[64];
  std::snprintf(name, sizeof(name), "Local\\dlssnr-ngx-hooks-%lu", GetCurrentProcessId());
  const HANDLE mapping =
      CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), name);
  auto* marker = static_cast<LONG*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)));
  if (marker == nullptr) {
    std::printf("[host] could not create leak marker\n");
    return 2;
  }
  *marker = 1;

  const HMODULE ngx = LoadLibraryA("_nvngx.dll");
  const FARPROC create = (ngx != nullptr) ? GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature")
                                          : nullptr;
  if (create == nullptr) {
    std::printf("[host] failed to load stub _nvngx.dll\n");
    return 2;
  }
  const unsigned char pristine = *reinterpret_cast<unsigned char*>(create);
  const HMODULE addon = LoadLibraryA("dlssnr-linux.addon64");
  if (addon == nullptr) {
    std::printf("[host] failed to load dlssnr-linux.addon64 (err %lu)\n", GetLastError());
    return 2;
  }
  DumpProlog("with leak marker set", reinterpret_cast<void*>(create));
  if (*reinterpret_cast<unsigned char*>(create) != pristine) {
    std::printf("\nFAIL: addon hooked on top of a (pretend) leaked detour\n");
    return 45;
  }
  std::printf("\nCLEAN: addon refused to hook while the leak marker was set\n");
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--pretend-leak") == 0) return PretendLeak();
  // Pin the stub NGX module for the whole process lifetime, like the driver's
  // _nvngx.dll in a real game.
  const HMODULE ngx = LoadLibraryA("_nvngx.dll");
  if (ngx == nullptr) {
    std::printf("[host] failed to load stub _nvngx.dll (err %lu)\n", GetLastError());
    return 2;
  }
  const FARPROC create = GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
  if (create == nullptr) {
    std::printf("[host] stub lacks NVSDK_NGX_D3D12_CreateFeature\n");
    return 2;
  }
  DumpProlog("pristine export", reinterpret_cast<void*>(create));

  // ReShade-style cycling: load, let it hook, unload. Yagz saw six cycles in
  // GTA V Enhanced; two is enough to layer a detour on a stale one.
  bool leaked = false;
  for (int cycle = 1; cycle <= 2; ++cycle) {
    std::printf("\n[host] --- addon load cycle %d ---\n", cycle);
    const HMODULE addon = LoadLibraryA("dlssnr-linux.addon64");
    if (addon == nullptr) {
      std::printf("[host] failed to load dlssnr-linux.addon64 (err %lu)\n", GetLastError());
      return 2;
    }
    std::printf("[host] addon base = %p\n", static_cast<void*>(addon));
    DumpProlog("after install", reinterpret_cast<void*>(create));
    FreeLibrary(addon);
    DumpProlog("after unload ", reinterpret_cast<void*>(create));
    if (*reinterpret_cast<unsigned char*>(create) == 0xe9) {
      std::printf("[host] detour still present with the addon unloaded — hook leaked\n");
      leaked = true;
    }
  }

  // Final cycle: install on top of whatever the previous cycles left behind,
  // then make the call a game would make.
  std::printf("\n[host] --- final load + CreateFeature call ---\n");
  const HMODULE addon = LoadLibraryA("dlssnr-linux.addon64");
  if (addon == nullptr) {
    std::printf("[host] failed to reload addon (err %lu)\n", GetLastError());
    return 2;
  }
  DumpProlog("before call", reinterpret_cast<void*>(create));
  FakeParams params;
  g_watch = true;
  const int faulted = GuardedCreateCall(reinterpret_cast<PfnCreate>(create), &params);
  g_watch = false;

  if (faulted != 0) return 43;
  if (leaked) {
    std::printf("\nREPRO: detour leaked past unload (call survived, but any late NGX call after\n"
                "a real unload would land in freed memory)\n");
    return 44;
  }
  std::printf("\nCLEAN: hooks detached on every unload and the final call reached the stub\n");
  return 0;
}
