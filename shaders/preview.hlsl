// Small preview image for the web UI. Mode (gLevels.w): 0 output, 1 input, 2 selection, 3 gain.
#include "common.hlsli"

Texture2D<float4> gFrame : register(t0);
Texture2D<float> gGain : register(t1);
Texture2D<float4> gLuma : register(t2);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  gOut.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 uv = (id.xy + 0.5) / float2(w, h);
  float lumaLevel = max(0, log2(gScreen.x / w) - 0.5);

  float3 src = gFrame.SampleLevel(gLinear, uv, 0).rgb;
  float4 L = gLuma.SampleLevel(gLinear, uv, lumaLevel);
  float G = SampleBSpline(gGain, uv, gStateSize.xy);
  float g = gTime.w > 0.5 ? 1 : ResponseGain(G, L.b, SelectCoverage(gLuma, uv));

  int mode = (int)(gLevels.w + 0.5);
  float3 o;
  if (mode == 1) {
    o = src;
  } else if (mode == 2) {
    float grey = dot(src, 0.333) * 0.35;
    o = lerp(float3(grey, grey, grey), float3(1.0, 0.15, 0.05), L.g * 0.85);
  } else if (mode == 3) {
    float grey = dot(src, 0.333) * 0.3;
    o = g < 0.995 ? lerp(float3(grey, grey, grey), HeatColor(g), 0.75) : float3(grey, grey, grey);
  } else {
    // Same composite the desktop gets: desktop * (1 - alpha) + rgb.
    float4 ov = gTime.w > 0.5 ? float4(0, 0, 0, 0) : OverlayOut(g, src, LinearToSrgb(L.bbb));
    o = src * (1 - ov.a) + ov.rgb;
  }
  gOut[id.xy] = float4(o, 1);
}
