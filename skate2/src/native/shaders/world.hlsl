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
  float4 material;   // x: macro overlay UV scale; y: detail UV scale; z: 1 = main material;
                     // w: 1 = environment cube bound
  float4 sun;        // xyz: direction to the sun
  float4 shadow_row[4];  // the game's shadow projection (PS c0..c3), see SunVisibility
  float4 shadow_flags;   // x: 1 = shadow atlas drawn
};

[[vk::binding(1, 0)]] SamplerState smp_wrap : register(s0);
[[vk::binding(2, 0)]] SamplerState smp_clamp : register(s1);
[[vk::binding(3, 0)]] SamplerState smp_point : register(s2);
[[vk::binding(0, 1)]] Texture2D<float4> diffuse : register(t0);
[[vk::binding(1, 1)]] Texture2D<float4> lightmap : register(t1);
[[vk::binding(2, 1)]] Texture2D<float4> chroma : register(t2);
[[vk::binding(3, 1)]] Texture2D<float4> normal_map : register(t3);
[[vk::binding(4, 1)]] Texture2D<float4> detail_map : register(t4);
[[vk::binding(5, 1)]] Texture2D<float4> macro_map : register(t5);
[[vk::binding(6, 1)]] Texture2D<float4> specular_map : register(t6);
[[vk::binding(0, 2)]] Texture2D<float4> shadow_map : register(t8);

// The game's sun shadow test (world pixel shaders' t0 lookup): light-space
// X = row0.p, Y = row2.p pick the first cascade (left half of the atlas)
// while |X|, |Y| < 0.99, else the second (right half, row1-scaled); depth
// row3.p + 0.005 against the stored nearest caster (0.5 + 0.05 z). 1 = lit.
float SunVisibility(float3 p) {
  if (shadow_flags.x < 0.5f) return 1.0f;
  const float4 hp = float4(p, 1.0f);
  const float x = dot(shadow_row[0], hp), y = dot(shadow_row[2], hp);
  float2 uv;
  if (max(abs(x), abs(y)) < 0.99f) {
    uv = float2(0.25f * x + 0.25f, -0.5f * y + 0.5f);
  } else {
    const float x2 = shadow_row[1].x * x + shadow_row[1].z;
    const float y2 = shadow_row[1].y * y + shadow_row[1].w;
    if (max(abs(x2), abs(y2)) >= 0.99f) return 1.0f;
    uv = float2(0.25f * x2 + 0.75f, -0.5f * y2 + 0.5f);
  }
  const float depth = 0.5f + 0.05f * (dot(shadow_row[3], hp) + 0.005f);
  return shadow_map.SampleLevel(smp_point, uv, 0.0f).r >= depth ? 1.0f : 0.0f;
}

// The environment cube as its six face averages (+X, -X, +Y, -Y, +Z, -Z).
[[vk::binding(7, 1)]] Texture2D<float4> environment_faces : register(t7);

float3 EnvironmentAverage(float3 d) {
  const float3 w = d * d / max(dot(d, d), 1e-6f);
  return w.x * environment_faces.Load(int3(d.x >= 0.0f ? 0 : 1, 0, 0)).rgb +
         w.y * environment_faces.Load(int3(d.y >= 0.0f ? 2 : 3, 0, 0)).rgb +
         w.z * environment_faces.Load(int3(d.z >= 0.0f ? 4 : 5, 0, 0)).rgb;
}

struct VsIn {
  float3 pos : POSITION;
  float2 uv : TEXCOORD0;
  float2 uv2 : TEXCOORD1;
  float3 nrm : NORMAL;
  float4 tangent : TANGENT;  // w: binormal sign
};

struct VsOut {
  float4 pos : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
  float3 nrm : TEXCOORD3;
  float3 tangent : TEXCOORD4;
  float3 binormal : TEXCOORD5;
};

VsOut vs_main(VsIn i) {
  VsOut o;
  o.pos = mul(float4(i.pos, 1.0f), view_proj);
  // The game's polygon offset for decals, approximated by a small depth pull.
  o.pos.z -= state.y * 2e-5f * o.pos.w;
  o.world = i.pos;
  o.uv = i.uv;
  o.uv2 = i.uv2;
  o.nrm = i.nrm;
  o.tangent = i.tangent.xyz;
  // E5C957221BEF0ECD: binormal = sign * cross(normal, tangent).
  o.binormal = i.tangent.w * cross(i.nrm, i.tangent.xyz);
  return o;
}

// The main world material, pixel shader F8D56B275E4DE57B (with vertex shader
// E5C957221BEF0ECD), transcribed from its ucode without the shadow map (the
// environment cube is read as face averages):
//   n = (2 normal.xy - 1) + (2 detail(uv * detail scale).xy - 1), 2 normal.z - 1,
//   in the tangent frame; macro = macro overlay(uv * macro scale);
//   baked = i^2 * chromaticity * 4.5 * 2.374 * (0.64 * (n.x sgn(T.L) +
//   n.y sgn(B.L)) + 0.4205 n.z) * macro;
//   sun specular = (18, 15, 10.5) * pow(R.V, 290 macro.g * 2 spec.g + 10) *
//   saturate(i^2 - 0.1) * 2 spec.r * macro.r, times 1 + 4 (1 - N.V)^8;
//   colour = baked * 2 albedo^2 + specular (albedo already squared here: x2).
float3 MainMaterial(VsOut i, float3 albedo, float intensity, float3 ch, int mode) {
  const float2 nm = normal_map.Sample(smp_wrap, i.uv).xy;
  const float nmz = normal_map.Sample(smp_wrap, i.uv).z;
  const float2 dm = detail_map.Sample(smp_wrap, i.uv * material.y).xy;
  const float3 macro = saturate(macro_map.Sample(smp_wrap, i.uv * material.x).rgb);
  const float3 spec = specular_map.Sample(smp_wrap, i.uv).rgb;
  const float3 tn = normalize(float3(2.0f * nm + 2.0f * dm - 2.0f, 2.0f * nmz - 1.0f));
  const float3 n = normalize(tn.x * i.tangent + tn.y * i.binormal + tn.z * i.nrm);
  const float3 v = normalize(eye.xyz - i.world);
  const float3 l = sun.xyz;
  const float i2 = intensity * intensity;
  // Bump term: the tangent normal biased toward the sun's side of the tangent
  // plane (sgn of T.L and B.L); ~1 for an unperturbed normal.
  const float tl = dot(i.tangent, l), bl = dot(i.binormal, l);
  const float bump = 2.374f * (0.64f * (tn.x * (tl > 0.0f ? 1.0f : -1.0f) +
                                         tn.y * (bl > 0.0f ? 1.0f : -1.0f)) +
                               0.4205f * tn.z);
  // Sun shadow: only on sun-facing pixels; darkens lit baked light toward
  // the game's shadow colour.
  const float sunvis = dot(n, l) > 0.0f ? SunVisibility(i.world) : 1.0f;
  const float shadow_amount = saturate(50.0f * (i2 - 0.04f)) * (1.0f - sunvis);
  const float3 baked = lerp(i2 * ch * 4.5f, float3(0.07f, 0.09f, 0.1f), shadow_amount) * bump;
  const float exponent = 290.0f * macro.g * 2.0f * spec.g + 10.0f;
  const float3 r = l - 2.0f * dot(n, l) * n;
  const float rv = saturate(dot(v, -r));
  const float m = min(i2, 0.04f);
  const float spec_mask = saturate(m - 0.1f + (i2 - m) * sunvis);
  const float3 sun_spec = float3(18.0f, 15.0f, 10.5f) * pow(max(rv, 1e-6f), exponent) * spec_mask;
  const float nv = saturate(dot(n, v));
  const float fres = 1.0f + 4.0f * pow(1.0f - nv, 8.0f);
  // Environment reflection: V - 2 (N.V) N with x, y negated, the cube read
  // as face averages (see EnvironmentAverage); amount i^2 plus a specular-
  // driven extra, masked by 2 spec.b * macro.b, times 1.5.
  float3 env = float3(0.0f, 0.0f, 0.0f);
  if (material.w > 0.5f) {
    const float3 rr = v - 2.0f * dot(n, v) * n;
    const float env_mask = 2.0f * spec.b * macro.b;
    const float boost = 0.3f * saturate(4.0f * (env_mask - 1.5f));
    const float lit_i2 = (i2 - m) * sunvis + m;
    const float amount = lit_i2 + boost * (1.0f - lit_i2);
    env = EnvironmentAverage(float3(-rr.x, -rr.y, rr.z)) * amount * env_mask * 1.5f;
  }
  const float3 specular = (env + sun_spec * 2.0f * spec.r * macro.r) * fres;
  if (mode == 5) return bump.xxx * 0.5f;
  if (mode == 6) return macro;
  if (mode == 7) return sun_spec * 2.0f * spec.r * macro.r * fres;
  if (mode == 8) return n * 0.5f + 0.5f;
  if (mode == 9) return env * fres;
  return baked * macro * 2.0f * albedo * albedo + specular;
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

  float3 color;
  if (material.z > 0.5f) {
    color = MainMaterial(i, albedo, intensity, ch, mode);
    if (mode >= 5) return float4(color, 1.0f);
  } else {
    const float i2 = intensity * intensity;
    const float shadow_amount = saturate(50.0f * (i2 - 0.04f)) * (1.0f - SunVisibility(i.world));
    color = albedo * albedo * lerp(i2 * 4.5f * ch, float3(0.07f, 0.09f, 0.1f), shadow_amount);
  }
  const float f = pow(saturate(length(i.world - eye.xyz) * fog.x + fog.y), fog.z);
  color = color * (1.0f - f) + fog_color.rgb * f;
  // Tone curve (PS c6.x = exposure, c253.x = 0.25, c255.w = 0.75, c255.z = 0.5).
  const float e = params.z;
  const float3 toe = saturate(1.0f - color * e);
  const float3 tone = (max(color * 0.25f * e + 0.75f, 1.0f) - toe * toe) * 0.5f;
  color = sqrt(abs(tone));
  return float4(color, diffuse_texel.a);
}
