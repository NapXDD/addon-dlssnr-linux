// Brings the model's answer back onto the frame.
//
// The answer is decoded to linear, then anchored: its luminance may only move a pixel g_max_ratio
// times brighter or darker than the original, and at g_colour 0 the model's brightness verdict
// lands on the game's own hue, so neutral surfaces cannot pick up a cast. Blend rather than
// replace: g_transfer 0 gives back exactly the original.
//
// Debug views write into the frame directly (they pass through the game's post-processing, so
// they are for judging the pass, not for pixel-exact readings): 1 = what the model is shown,
// 2 = its raw answer, 3 = what it changed, amplified twenty times -- flat grey means nothing.
#include "nr_common.hlsli"

Texture2D<float3> Orig : register(t0);
Texture2D<float3> Model : register(t1);
RWTexture2D<float3> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    const float3 o = max(Orig[id.xy], 0.0);
    float3 m = DecodeDisplay(Model[id.xy], g_white_point);

    if (g_debug == 1)
    {
        Out[id.xy] = EncodeDisplay(o, g_white_point);
        return;
    }
    if (g_debug == 2)
    {
        Out[id.xy] = Model[id.xy];
        return;
    }
    if (g_debug == 3)
    {
        Out[id.xy] = abs(m - o) * 20.0;
        return;
    }

    const float lo = Luma(o);
    const float lm = Luma(m);

    if (lm > 1e-8 && lo > 1e-8)
    {
        const float ratio = clamp(lm / lo, 1.0 / g_max_ratio, g_max_ratio);
        m *= (ratio * lo) / lm;  // luminance clamped to the guard, hue kept

        // Whether the model's colour arrives with its light.
        const float3 brightness_on_game_hue = o * (Luma(m) / lo);
        m = lerp(brightness_on_game_hue, m, g_colour);
    }
    else
    {
        m = o;  // true black stays true black; the model invents nothing there
    }

    // NaN guard: any non-finite lane falls back to the original.
    m = select(m == m, m, o);

    Out[id.xy] = lerp(o, m, g_transfer);
}
