// Pass 1: captured sRGB frame -> luminance pyramid base.
//   r = E (bright energy = Y * mask), g = mask (coverage), b = Y (raw luminance)
// GenerateMips afterwards turns each mip level into box means over 2^k x 2^k pixel cells.
#include "common.hlsli"

Texture2D<float4> gFrame : register(t0);
RWTexture2D<float4> gOut : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)gScreen.x || id.y >= (uint)gScreen.y) return;
  float3 lin = SrgbToLinear(gFrame.Load(int3(id.xy, 0)).rgb);
  float Y = Luminance(lin);
  // Counting starts at the threshold and ramps up over `softness` above it, so nothing darker
  // than the threshold ever counts as bright (even with threshold 0).
  float m = smoothstep(threshold, threshold + max(softness, 1e-4), Y);
  gOut[id.xy] = float4(Y * m, m, Y, 1);
}
