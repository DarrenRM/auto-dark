// Pass 2: per-level budget enforcement, run from the coarsest level down to the finest.
//   raw_k = min(1, budget_k / mean_k(E))
//   c_k   = min over a (2*spread+1)^2 neighbourhood of min(raw_k, upsampled c_{k+1})
// Eroding the *combined* field at every level (not just raw_k) cancels the smoothing that each
// upsample adds, so a bright region keeps one flat gain right up to its edge and the fade happens
// in the dark surroundings. Without this, the repeated upsampling leaks less-dimmed values inward
// and bright windows get a light rim ("bevel").
#include "common.hlsli"

Texture2D<float4> gLuma : register(t0);   // full pyramid
Texture2D<float> gCoarse : register(t1);  // combined gain of level k+1 (single mip)
RWTexture2D<float> gOut : register(u0);   // combined gain of level k

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  int level = (int)gPassA.x;
  bool isTop = gPassA.y > 0.5;
  int2 dim = int2(gPassA.zw);
  if (id.x >= (uint)dim.x || id.y >= (uint)dim.y) return;

  float budget = gPassB.z;
  int r = (int)spread;
  float g = 1;
  [loop] for (int dy = -r; dy <= r; dy++) {
    [loop] for (int dx = -r; dx <= r; dx++) {
      int2 p = clamp(int2(id.xy) + int2(dx, dy), 0, dim - 1);
      float e = gLuma.Load(int3(p, level)).r;
      g = min(g, saturate(budget / max(e, 1e-6)));
      if (!isTop) g = min(g, SampleBSpline(gCoarse, (p + 0.5) / float2(dim), gPassB.xy));
    }
  }
  gOut[id.xy] = g;
}
