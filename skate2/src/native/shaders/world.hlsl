// Native renderer: static world geometry with its diffuse texture and baked
// lightmap.
//
// Positions are world space (the static world needs no per-object matrix) and
// are transformed by the camera view-projection the game keeps at
// camera+0xA0, as row vectors (v * M, D3D convention).
//
// Shading is a port of the game's own world shader (Xenos pixel shader
// 5D2EA5FF45C0BC9A with vertex shader BE11E83C83EA4E92, read from the ucode):
//   - diffuse is squared (gamma 2 to linear);
//   - the lightmap packs up to four meshes' intensity maps, one per channel,
//     selected by the one-hot VS c7 (the material's lightmap channel +4): light =
//     intensity^2 * chromaticity * 4.5; lightmap and chromaticity share the
//     second texcoord and clamp;
//   - distance fog: f = saturate(d * a + b)^c, mixed in linear space;
//   - the game's tone curve with exposure (PS c6.x), then sqrt back.
// The game's shadow-map term (t0, a screen depth buffer) is left out.
// Alpha test, blending, culling and polygon offset come per draw from the
// game's own emulated draw (native_draw_state.cpp).
// The second texcoord arrives made absolute by the vertex decode.
//
// Bindings follow the native RHI's Vulkan plan: set 0 = the buffer parameters
// in order (the root constants, b0) followed by the static samplers; each
// texture table parameter gets its own set (t0..t1 = set 1).

[[vk::binding(0, 0)]] cbuffer C : register(b0) {
  row_major float4x4 view_proj;
  float4 params;     // x: 1 = textured; y: mode (skate2_native_lm_mode); z: exposure;
                     // w: lightmap channel
  float4 eye;        // xyz: camera position
  float4 fog;        // xyz: fog curve (scale, bias, power)
  float4 fog_color;  // xyz: linear fog colour
  float4 state;      // x: alpha test reference (0 = off); y: 1 = polygon offset (decal);
                     // z: 1 = the game's draw state was found
};

[[vk::binding(1, 0)]] SamplerState smp_wrap : register(s0);
[[vk::binding(2, 0)]] SamplerState smp_clamp : register(s1);
[[vk::binding(0, 1)]] Texture2D<float4> diffuse : register(t0);
[[vk::binding(1, 1)]] Texture2D<float4> lightmap : register(t1);
[[vk::binding(2, 1)]] Texture2D<float4> chroma : register(t2);

struct VsIn {
  float3 pos : POSITION;
  float2 uv : TEXCOORD0;
  float2 uv2 : TEXCOORD1;
};

struct VsOut {
  float4 pos : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
};

VsOut vs_main(VsIn i) {
  VsOut o;
  o.pos = mul(float4(i.pos, 1.0f), view_proj);
  // The game's polygon offset for decals, approximated by a small depth pull.
  o.pos.z -= state.y * 2e-5f * o.pos.w;
  o.world = i.pos;
  o.uv = i.uv;
  o.uv2 = i.uv2;
  return o;
}

float4 ps_main(VsOut i) : SV_Target {
  if (params.x < 0.5f) {
    const float3 n = normalize(cross(ddx(i.world), ddy(i.world)));
    const float light = 0.3f + 0.7f * abs(dot(n, normalize(float3(0.35f, 0.85f, 0.4f))));
    return float4(float3(0.75f, 0.75f, 0.75f) * light, 1.0f);
  }
  const float4 diffuse_texel = diffuse.Sample(smp_wrap, i.uv);
  // Alpha-tested and blended world shaders output the diffuse alpha.
  if (state.x > 0.0f) clip(diffuse_texel.a - state.x);
  const float3 albedo = diffuse_texel.rgb;
  const float4 lm = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0f);
  const float3 ch = chroma.SampleLevel(smp_clamp, i.uv2, 0.0f).rgb;
  const int channel = int(params.w + 0.5f);
  const float intensity = dot(lm, float4(channel == 0, channel == 1, channel == 2, channel == 3));
  const int mode = int(params.y + 0.5f);
  if (mode == 1) return float4(albedo, diffuse_texel.a);
  if (mode == 2) return float4(intensity.xxx, diffuse_texel.a);
  if (mode == 3) return float4(ch, diffuse_texel.a);
  if (mode == 4) return float4(state.z > 0.5f ? albedo * float3(0.3f, 1.0f, 0.3f) : float3(1.0f, 0.0f, 0.0f), 1.0f);

  float3 color = albedo * albedo * (intensity * intensity * 4.5f) * ch;
  const float f = pow(saturate(length(i.world - eye.xyz) * fog.x + fog.y), fog.z);
  color = color * (1.0f - f) + fog_color.rgb * f;
  // Tone curve (PS c6.x = exposure, c253.x = 0.25, c255.w = 0.75, c255.z = 0.5).
  const float e = params.z;
  const float3 toe = saturate(1.0f - color * e);
  const float3 tone = (max(color * 0.25f * e + 0.75f, 1.0f) - toe * toe) * 0.5f;
  color = sqrt(abs(tone));
  return float4(color, diffuse_texel.a);
}
