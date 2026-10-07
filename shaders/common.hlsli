// Shared declarations for all auto-dark shaders.
//
// To add a tunable parameter:
//   1. add an entry to shaders/params.json (it becomes a slider in the web UI)
//   2. declare it in the Params cbuffer below with the same name (float, int or uint)
//   3. use it. The engine fills Params by name via shader reflection, so no C++ change is needed.

cbuffer Frame : register(b0) {
  float4 gScreen;     // width, height, 1/width, 1/height
  float4 gStateSize;  // temporal-state width, height, 1/width, 1/height
  float4 gTime;       // dt (ms), time (s), resetTemporal, bypass
  float4 gLevels;     // minLevel, maxLevel, numLevels, previewMode
  float4 gColorM[3];  // treated-area color recipe, rows of a 3x3 matrix (encoded space)
  float4 gColorCtl;   // time-of-day amount, enabled
};

cbuffer PassConsts : register(b2) {
  float4 gPassA;      // level, isTop, levelWidth, levelHeight
  float4 gPassB;      // coarseWidth, coarseHeight, budget, unused
};

cbuffer Params : register(b1) {
  float threshold;
  float softness;
  float minLevel;
  float maxLevel;
  float spread;
  float strength;
  float minGain;
  float protectDarks;
  float darkFloor;
  float attackMs;
  float releaseMs;
  float debugView;
  float debugOpacity;
  float splitPos;
  float selective;
  float selectScale;
  float selectGrow;
  float followFullAt;
  float colorDetail;
};

SamplerState gLinear : register(s0);  // bilinear, clamp

float3 SrgbToLinear(float3 c) {
  return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0), 2.4);
}

float3 LinearToSrgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float Luminance(float3 lin) { return dot(lin, float3(0.2126, 0.7152, 0.0722)); }

// Smooth cubic B-spline upsampling with 4 bilinear taps (Sigg & Hadwiger).
// Avoids the blocky look of plain bilinear upsampling of coarse gain maps.
float SampleBSpline(Texture2D<float> t, float2 uv, float2 size) {
  float2 coord = uv * size - 0.5;
  float2 i = floor(coord);
  float2 f = coord - i;
  float2 f2 = f * f, f3 = f2 * f;
  float2 w0 = (-f3 + 3 * f2 - 3 * f + 1) / 6;
  float2 w1 = (3 * f3 - 6 * f2 + 4) / 6;
  float2 w2 = (-3 * f3 + 3 * f2 + 3 * f + 1) / 6;
  float2 w3 = f3 / 6;
  float2 g0 = w0 + w1, g1 = w2 + w3;
  float2 h0 = (i - 1 + w1 / g0 + 0.5) / size;
  float2 h1 = (i + 1 + w3 / g1 + 0.5) / size;
  return g0.y * (g0.x * t.SampleLevel(gLinear, float2(h0.x, h0.y), 0) +
                 g1.x * t.SampleLevel(gLinear, float2(h1.x, h0.y), 0)) +
         g1.y * (g0.x * t.SampleLevel(gLinear, float2(h0.x, h1.y), 0) +
                 g1.x * t.SampleLevel(gLinear, float2(h1.x, h1.y), 0));
}

// Per-pixel response: turns the smoothed gain map value G into the final gain for a pixel with
// linear luminance Y and bright-mask coverage `cover` (sampled at selectScale).
float ResponseGain(float G, float Y, float cover) {
  float w = selective * (1 - cover);
  w = max(w, protectDarks * (1 - smoothstep(darkFloor * 0.5, max(darkFloor, 1e-4), Y)));
  float g = lerp(G, 1, w);
  g = lerp(1, g, strength);
  return clamp(g, minGain, 1);
}

// Bright-mask coverage at selectScale, grown outward by selectGrow cells (max filter) so pixels at
// the edge of a bright region count as fully bright instead of getting a lighter rim.
float SelectCoverage(Texture2D<float4> luma, float2 uv) {
  float2 cell = exp2(selectScale) * gScreen.zw;
  int r = (int)selectGrow;
  float c = 0;
  [loop] for (int dy = -r; dy <= r; dy++)
    [loop] for (int dx = -r; dx <= r; dx++)
      c = max(c, luma.SampleLevel(gLinear, uv + float2(dx, dy) * cell, selectScale).g);
  return c;
}

// The overlay blends in gamma-encoded space (SDR desktop), so a linear-light gain g must be
// applied to encoded values as g^(1/2.2).
float EncodedGain(float g) { return pow(saturate(g), 1.0 / 2.2); }

// Final overlay output (premultiplied) for an encoded source pixel srcE and linear gain g:
//   final = desktop * (1 - alpha) + rgb  ==  ge * lerp(src, M * src, s)
// where M is the colour recipe and s = time amount * how hard this pixel is dimmed. A scalar alpha
// scales all channels equally, so it passes through ge * min(diag) of the blended matrix (a safe
// lower bound when the off-diagonal terms are >= 0) and the remaining colour is added back from
// the captured pixel. srcSmooth (a blurred estimate) is mixed in when colorDetail < 1, trading text
// crispness for no fringes while scrolling.
float4 OverlayOut(float g, float3 srcE, float3 srcSmooth) {
  float ge = EncodedGain(g);
  float s = 0;
  if (gColorCtl.y > 0.5) s = gColorCtl.x * smoothstep(0, max(followFullAt, 1e-3), 1 - g);
  if (s <= 0) return float4(0, 0, 0, 1 - ge);

  float3x3 M = float3x3(gColorM[0].xyz, gColorM[1].xyz, gColorM[2].xyz);
  float3 diag = lerp(1, float3(M._11, M._22, M._33), s);
  float k = ge * saturate(min(diag.r, min(diag.g, diag.b)));
  float3 src = lerp(srcSmooth, srcE, colorDetail);
  float3 target = ge * lerp(src, mul(M, src), s);
  return float4(max(target - k * src, 0), 1 - k);
}

// Attenuation heatmap: light dimming = yellow, strong dimming = red/magenta.
float3 HeatColor(float g) {
  float a = saturate(1 - g);
  float3 c0 = float3(1.0, 0.85, 0.2), c1 = float3(1.0, 0.25, 0.05), c2 = float3(0.75, 0.0, 0.6);
  return a < 0.5 ? lerp(c0, c1, a * 2) : lerp(c1, c2, a * 2 - 1);
}
