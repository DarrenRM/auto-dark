// Pass 3: temporal smoothing of the finest combined gain.
// Dimming follows `attackMs` (0 = instant: the current frame is always protected),
// brightening follows `releaseMs`, so scrolling and tab switches don't pump.
#include "common.hlsli"

Texture2D<float> gTarget : register(t0);  // combined gain at minLevel
RWTexture2D<float> gState : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)gStateSize.x || id.y >= (uint)gStateSize.y) return;
  float target = gTarget.Load(int3(id.xy, 0));
  float s = gState[id.xy];
  float tau = target < s ? attackMs : releaseMs;
  float a = tau <= 0 ? 1 : 1 - exp(-gTime.x / tau);
  s = gTime.z > 0.5 ? target : lerp(s, target, a);
  gState[id.xy] = s;
}
