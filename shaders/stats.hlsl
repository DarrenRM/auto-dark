// Coarse grid of (input luminance, estimated output luminance, gain, bright energy) for the meters.
#include "common.hlsli"

Texture2D<float> gGain : register(t1);
Texture2D<float4> gLuma : register(t2);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  gOut.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float2 uv = (id.xy + 0.5) / float2(w, h);
  float4 L = gLuma.SampleLevel(gLinear, uv, gPassA.x);
  float G = SampleBSpline(gGain, uv, gStateSize.xy);
  float g = gTime.w > 0.5 ? 1 : ResponseGain(G, L.b, L.g);
  gOut[id.xy] = float4(L.b, L.b * g, g, L.r);
}
