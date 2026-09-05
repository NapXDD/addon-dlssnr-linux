// Stub _nvngx.dll for the hook-layering repro (issue #3 / PR #4).
// Exports the five NGX entry points ngx_probe hooks, with distinct bodies so
// lld's ICF cannot fold them onto one address (Detours would then see the
// same target five times).

extern "C" {

volatile int g_sink;

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_CreateFeature(void*, int, void*, void** out) {
  g_sink += 1;
  if (out != nullptr) *out = const_cast<int*>(&g_sink);
  return 1;  // NVSDK_NGX_Result_Success
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_EvaluateFeature(void*, void*, void*, void*) {
  g_sink += 2;
  return 1;
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_ReleaseFeature(void*) {
  g_sink += 3;
  return 1;
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D11_CreateFeature(void*, int, void*, void** out) {
  g_sink += 4;
  if (out != nullptr) *out = const_cast<int*>(&g_sink);
  return 1;
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D11_EvaluateFeature(void*, void*, void*, void*) {
  g_sink += 5;
  return 1;
}

}  // extern "C"
