// Pass 4: draws the overlay. Output is premultiplied: final = desktop * (1 - alpha) + rgb.
// Plain dimming is black with alpha = 1 - gain, i.e. desktop * gain.
#include "common.hlsli"

Texture2D<float4> gFrame : register(t0);
Texture2D<float> gGain : register(t1);   // temporal state
Texture2D<float4> gLuma : register(t2);

struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
  VSOut o;
  o.uv = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PSMain(VSOut i) : SV_Target {
  if (gTime.w > 0.5) return 0;  // bypass

  int2 px = int2(i.pos.xy);
  float4 L = gLuma.Load(int3(px, 0));
  float G = SampleBSpline(gGain, i.uv, gStateSize.xy);
  float g = ResponseGain(G, L.b, SelectCoverage(gLuma, i.uv));

  int view = (int)(debugView + 0.5);
  float ge = EncodedGain(g);
  if (view == 1) {  // selection mask tinted red
    float t = L.g * debugOpacity;
    return float4(float3(1.0, 0.1, 0.05) * t, 1 - ge * (1 - t));
  }
  if (view == 2) {  // gain heatmap where any dimming happens
    float t = g < 0.995 ? debugOpacity : 0;
    return float4(HeatColor(g) * t, 1 - ge * (1 - t));
  }
  if (view == 3) {  // split: filtered left, raw right
    float d = (i.uv.x - splitPos) * gScreen.x;
    if (abs(d) < 1.5) return float4(0.8, 0.45, 0.05, 0.8);
    if (d > 0) return 0;
  }
  float3 srcE = gFrame.Load(int3(px, 0)).rgb;
  float3 srcSmooth = LinearToSrgb(gLuma.SampleLevel(gLinear, i.uv, 3).bbb);
  return OverlayOut(g, srcE, srcSmooth);
}
