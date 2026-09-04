// White-point measurement, first stage: each 16x16 tile's log-luminance sum and pixel count.
// Log-average (geometric mean) rather than arithmetic, so a handful of extreme pixels cannot own
// the answer -- the standard auto-exposure choice.
#include "nr_common.hlsli"

Texture2D<float3> Src : register(t0);
Texture2D<float3> Unused : register(t1);
RWStructuredBuffer<float2> Tiles : register(u0);  // x = sum of log luma, y = count

groupshared float2 g_partial[256];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gindex : SV_GroupIndex)
{
    float2 sample_value = float2(0.0, 0.0);
    if (id.x < g_width && id.y < g_height)
    {
        const float l = Luma(max(Src[id.xy], 0.0));
        sample_value = float2(log(max(l, 1e-6)), 1.0);
    }
    g_partial[gindex] = sample_value;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (gindex < stride)
            g_partial[gindex] += g_partial[gindex + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (gindex == 0)
    {
        const uint tiles_x = (g_width + 15) / 16;
        Tiles[gid.y * tiles_x + gid.x] = g_partial[0];
    }
}
