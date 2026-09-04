// Builds the proxy the model is shown: a display-referred version of the upscaler's linear output.
#include "nr_common.hlsli"

Texture2D<float3> Src : register(t0);
Texture2D<float3> Unused : register(t1);
RWTexture2D<float3> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;
    Dst[id.xy] = EncodeDisplay(Src[id.xy], g_white_point);
}
