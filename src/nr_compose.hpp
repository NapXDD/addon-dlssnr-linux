// The compose pass around the model: encode before, resolve after, plus the white-point
// measurement that drives the encode.
//
//   copy game output -> colorCopy               (the original, o)
//   encode:  proxy = display-referred(o / wp)   (what the model is shown)
//   lum1/lum2: log-average luminance of o -> readback ring (the host smooths it into wp)
//   model:   Color = proxy, Output = modelOut
//   resolve: game output = anchor(decode(modelOut) * wp, o) blended by transfer
//
// Encode and resolve run in the same frame with the same constants, so the white point never
// mismatches across the round trip; the measurement itself is allowed to lag a few frames and is
// smoothed on the CPU. Descriptors live in a small ring so a frame still in flight never sees its
// table rewritten. The command-list state we clobber is state the game must rebind after an NGX
// evaluate anyway.

#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

#include <d3d12.h>

#pragma comment(lib, "d3d12")

#include "../build/shaders/nr_encode_dxil.h"
#include "../build/shaders/nr_resolve_dxil.h"
#include "../build/shaders/nr_lum1_dxil.h"
#include "../build/shaders/nr_lum2_dxil.h"

namespace nr_compose {

constexpr uint32_t kRingSize = 8;        // frames of descriptor tables in flight
constexpr uint32_t kSlotsPerFrame = 12;  // see the slot map in RecordPre/RecordPost
constexpr uint32_t kReadbackRing = 4;    // frames the measurement is allowed to lag

struct Compose {
  ID3D12RootSignature* root_sig = nullptr;
  ID3D12PipelineState* encode_pso = nullptr;
  ID3D12PipelineState* resolve_pso = nullptr;
  ID3D12PipelineState* lum1_pso = nullptr;
  ID3D12PipelineState* lum2_pso = nullptr;
  ID3D12DescriptorHeap* heap = nullptr;
  uint32_t descriptor_size = 0;

  ID3D12Resource* tile_buf = nullptr;    // float2 per 16x16 tile: (sum of log luma, count)
  ID3D12Resource* result_buf = nullptr;  // one float: the frame's raw white point
  ID3D12Resource* readback[kReadbackRing] = {};
  uint32_t tile_count = 0;
  uint32_t buf_w = 0, buf_h = 0;

  bool failed = false;
};

inline Compose c;

inline void Release() {
  for (auto*& r : c.readback)
    if (r != nullptr) r->Release();
  if (c.result_buf != nullptr) c.result_buf->Release();
  if (c.tile_buf != nullptr) c.tile_buf->Release();
  if (c.heap != nullptr) c.heap->Release();
  if (c.lum2_pso != nullptr) c.lum2_pso->Release();
  if (c.lum1_pso != nullptr) c.lum1_pso->Release();
  if (c.encode_pso != nullptr) c.encode_pso->Release();
  if (c.resolve_pso != nullptr) c.resolve_pso->Release();
  if (c.root_sig != nullptr) c.root_sig->Release();
  c = {};
}

inline bool MakePso(ID3D12Device* device, const unsigned char* dxil, size_t size,
                    ID3D12PipelineState** out) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = c.root_sig;
  pso.CS.pShaderBytecode = dxil;
  pso.CS.BytecodeLength = size;
  return SUCCEEDED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(out)));
}

// Root signature: [0] SRV table t0-t1, [1] UAV table u0, [2] seven root constants b0.
inline bool Init(ID3D12Device* device) {
  if (c.root_sig != nullptr) return true;
  if (c.failed) return false;

  D3D12_DESCRIPTOR_RANGE srv_range = {};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 2;
  srv_range.BaseShaderRegister = 0;
  D3D12_DESCRIPTOR_RANGE uav_range = {};
  uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  uav_range.NumDescriptors = 1;
  uav_range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &srv_range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &uav_range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants.ShaderRegister = 0;
  params[2].Constants.Num32BitValues = 7;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = 3;
  desc.pParameters = params;

  ID3DBlob* blob = nullptr;
  ID3DBlob* error = nullptr;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
    if (error != nullptr) error->Release();
    c.failed = true;
    return false;
  }
  const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                                 blob->GetBufferSize(), IID_PPV_ARGS(&c.root_sig));
  blob->Release();
  if (FAILED(hr)) {
    c.failed = true;
    return false;
  }

  if (!MakePso(device, nr_encode_dxil, sizeof(nr_encode_dxil), &c.encode_pso) ||
      !MakePso(device, nr_resolve_dxil, sizeof(nr_resolve_dxil), &c.resolve_pso) ||
      !MakePso(device, nr_lum1_dxil, sizeof(nr_lum1_dxil), &c.lum1_pso) ||
      !MakePso(device, nr_lum2_dxil, sizeof(nr_lum2_dxil), &c.lum2_pso)) {
    c.failed = true;
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kRingSize * kSlotsPerFrame;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&c.heap)))) {
    c.failed = true;
    return false;
  }
  c.descriptor_size =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

inline bool EnsureBuffers(ID3D12Device* device, uint32_t w, uint32_t h) {
  if (c.tile_buf != nullptr && c.buf_w == w && c.buf_h == h) return true;
  for (auto*& r : c.readback)
    if (r != nullptr) { r->Release(); r = nullptr; }
  if (c.result_buf != nullptr) { c.result_buf->Release(); c.result_buf = nullptr; }
  if (c.tile_buf != nullptr) { c.tile_buf->Release(); c.tile_buf = nullptr; }

  c.tile_count = ((w + 15) / 16) * ((h + 15) / 16);

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  desc.Width = (UINT64)c.tile_count * 8;  // float2 per tile
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&c.tile_buf))))
    return false;
  desc.Width = 256;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&c.result_buf))))
    return false;

  heap.Type = D3D12_HEAP_TYPE_READBACK;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  for (auto*& r : c.readback) {
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&r))))
      return false;
  }
  c.buf_w = w;
  c.buf_h = h;
  return true;
}

// --- helpers -------------------------------------------------------------

inline void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  cmd->ResourceBarrier(1, &b);
}

struct Slot {
  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

inline Slot SlotAt(uint32_t index) {
  Slot s;
  s.cpu = c.heap->GetCPUDescriptorHandleForHeapStart();
  s.gpu = c.heap->GetGPUDescriptorHandleForHeapStart();
  s.cpu.ptr += (SIZE_T)index * c.descriptor_size;
  s.gpu.ptr += (UINT64)index * c.descriptor_size;
  return s;
}

inline void WriteSrv(ID3D12Device* device, uint32_t index, ID3D12Resource* res) {
  D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
  d.Format = res->GetDesc().Format;
  d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  d.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(res, &d, SlotAt(index).cpu);
}

inline void WriteUav(ID3D12Device* device, uint32_t index, ID3D12Resource* res) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
  d.Format = res->GetDesc().Format;
  d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(res, nullptr, &d, SlotAt(index).cpu);
}

inline void WriteBufferSrv(ID3D12Device* device, uint32_t index, ID3D12Resource* res,
                           uint32_t elements, uint32_t stride) {
  D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
  d.Format = DXGI_FORMAT_UNKNOWN;
  d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  d.Buffer.NumElements = elements;
  d.Buffer.StructureByteStride = stride;
  device->CreateShaderResourceView(res, &d, SlotAt(index).cpu);
}

inline void WriteBufferUav(ID3D12Device* device, uint32_t index, ID3D12Resource* res,
                           uint32_t elements, uint32_t stride) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
  d.Format = DXGI_FORMAT_UNKNOWN;
  d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  d.Buffer.NumElements = elements;
  d.Buffer.StructureByteStride = stride;
  device->CreateUnorderedAccessView(res, nullptr, &d, SlotAt(index).cpu);
}

struct Params {
  float transfer;
  float max_ratio;
  float colour;
  float white_point;
  uint32_t debug;
  uint32_t width;
  uint32_t height;
};

inline void Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso, uint32_t srv_slot,
                     uint32_t uav_slot, const Params& p, uint32_t groups_x, uint32_t groups_y) {
  cmd->SetDescriptorHeaps(1, &c.heap);
  cmd->SetComputeRootSignature(c.root_sig);
  cmd->SetPipelineState(pso);
  cmd->SetComputeRootDescriptorTable(0, SlotAt(srv_slot).gpu);
  cmd->SetComputeRootDescriptorTable(1, SlotAt(uav_slot).gpu);
  cmd->SetComputeRoot32BitConstants(2, 7, &p, 0);
  cmd->Dispatch(groups_x, groups_y, 1);
}

// --- the two halves around the model -------------------------------------
//
// Descriptor slot map per ring frame:
//   0-1  SRV colorCopy x2      (encode t0-t1, lum1 t0-t1)
//   2    UAV proxy             (encode)
//   3    UAV tile_buf          (lum1)
//   4-5  SRV colorCopy, model  (resolve t0-t1)
//   6    UAV game output       (resolve)
//   7-8  SRV tile_buf x2       (lum2 t0-t1)
//   9    UAV result_buf        (lum2)
//
// Persistent states between frames: colorCopy, proxy, modelOut, tile_buf, result_buf all
// UNORDERED_ACCESS; the game's output is in UNORDERED_ACCESS around DLSS evaluation.

// Copy the original aside, encode the proxy, and record the luminance measurement. Leaves:
// game_out COPY_SOURCE, colorCopy and proxy NON_PIXEL_SHADER_RESOURCE (ready for the model).
inline bool RecordPre(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out,
                      ID3D12Resource* color_copy, ID3D12Resource* proxy, uint32_t w, uint32_t h,
                      uint32_t frame, float white_point) {
  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return false;
  if (!Init(device) || !EnsureBuffers(device, w, h)) {
    device->Release();
    return false;
  }

  const uint32_t base = (frame % kRingSize) * kSlotsPerFrame;
  WriteSrv(device, base + 0, color_copy);
  WriteSrv(device, base + 1, color_copy);
  WriteUav(device, base + 2, proxy);
  WriteBufferUav(device, base + 3, c.tile_buf, c.tile_count, 8);
  WriteBufferSrv(device, base + 7, c.tile_buf, c.tile_count, 8);
  WriteBufferSrv(device, base + 8, c.tile_buf, c.tile_count, 8);
  WriteBufferUav(device, base + 9, c.result_buf, 1, 4);

  Params p = {0.0f, 0.0f, 0.0f, white_point, 0, w, h};

  Barrier(cmd, game_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(cmd, color_copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  cmd->CopyResource(color_copy, game_out);
  Barrier(cmd, color_copy, D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // The proxy the model will see.
  Dispatch(cmd, c.encode_pso, base + 0, base + 2, p, (w + 7) / 8, (h + 7) / 8);

  // The measurement for future frames' white point.
  Dispatch(cmd, c.lum1_pso, base + 0, base + 3, p, (w + 15) / 16, (h + 15) / 16);
  Barrier(cmd, c.tile_buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Dispatch(cmd, c.lum2_pso, base + 7, base + 9, p, 1, 1);
  Barrier(cmd, c.result_buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
  cmd->CopyBufferRegion(c.readback[frame % kReadbackRing], 0, c.result_buf, 0, 4);
  Barrier(cmd, c.result_buf, D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Barrier(cmd, c.tile_buf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  Barrier(cmd, proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  device->Release();
  return true;
}

// Resolve the model's answer over the game's output and restore every state for the next frame.
inline void RecordPost(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out,
                       ID3D12Resource* color_copy, ID3D12Resource* proxy,
                       ID3D12Resource* model_out, uint32_t w, uint32_t h, uint32_t frame,
                       float transfer, float max_ratio, float colour, float white_point,
                       uint32_t debug) {
  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;

  const uint32_t base = (frame % kRingSize) * kSlotsPerFrame;
  WriteSrv(device, base + 4, color_copy);
  WriteSrv(device, base + 5, model_out);
  WriteUav(device, base + 6, game_out);

  Barrier(cmd, model_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(cmd, game_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  Params p = {transfer, max_ratio, colour, white_point, debug, w, h};
  Dispatch(cmd, c.resolve_pso, base + 4, base + 6, p, (w + 7) / 8, (h + 7) / 8);

  Barrier(cmd, model_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Barrier(cmd, color_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Barrier(cmd, proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  device->Release();
}

// The oldest readback slot's raw white point, or a negative number when none is ready yet. Safe
// to call every frame; the slot being read is kReadbackRing frames old, long past GPU use.
inline float ReadWhitePoint(uint32_t frame) {
  if (frame < kReadbackRing || c.readback[0] == nullptr) return -1.0f;
  ID3D12Resource* slot = c.readback[(frame + 1) % kReadbackRing];
  void* mapped = nullptr;
  const D3D12_RANGE read_range = {0, 4};
  if (FAILED(slot->Map(0, &read_range, &mapped)) || mapped == nullptr) return -1.0f;
  float value = -1.0f;
  std::memcpy(&value, mapped, sizeof(value));
  const D3D12_RANGE no_write = {0, 0};
  slot->Unmap(0, &no_write);
  return value;
}

}  // namespace nr_compose
