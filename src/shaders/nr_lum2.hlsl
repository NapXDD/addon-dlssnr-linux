// White-point measurement, second stage: fold every tile into one number.
// Result = exp(mean log luma) / 0.18 -- the frame's geometric-mean luminance placed at mid-grey,
// which is the white point the encode divides by. The host smooths it over time.
#include "nr_common.hlsli"

StructuredBuffer<float2> Tiles : register(t0);
StructuredBuffer<float2> Unused : register(t1);
RWStructuredBuffer<float> Result : register(u0);

groupshared float2 g_partial[256];

[numthreads(256, 1, 1)]
void main(uint gindex : SV_GroupIndex)
{
    const uint tiles_x = (g_width + 15) / 16;
    const uint tiles_y = (g_height + 15) / 16;
    const uint tile_count = tiles_x * tiles_y;

    float2 sum = float2(0.0, 0.0);
    for (uint i = gindex; i < tile_count; i += 256)
        sum += Tiles[i];
    g_partial[gindex] = sum;
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
        const float2 total = g_partial[0];
        Result[0] = (total.y > 0.0) ? exp(total.x / total.y) / 0.18 : 1.0;
    }
}
