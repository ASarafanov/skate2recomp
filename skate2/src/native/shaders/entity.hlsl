// Native renderer: entities drawn through RenderMesh (skater, pedestrians,
// props), first stage.
//
// The clip matrix and the bone palette are the game's own: the vertex
// shader constants of the entity's emulated draw, taken from the command
// processor's draw observer.
//   - c0..c3: clip rows (dot(c_i, p)); for skinned meshes world -> clip,
//     for rigid meshes model -> clip;
//   - c4: camera position; c5/c6: fog curve and colour (skinned shaders);
//   - from c7 (c8 for some shaders): the palette, three float4 rows per bone,
//     a 3x4 model -> world matrix (translation in w), indexed by the vertex's
//     bone number. The game's skinned shaders blend weights x, y, z (bytes
//     / 255) and give the fourth bone 1 - (x + y + z).
// Character positions are k_16_16_16_16 snorm * 2 with y + 0.8 (the game's
// skinned vertex shaders, as in Skate 3 Recomp's character dequantisation).
// Shading is a placeholder (squared diffuse, sun + ambient, the game's tone
// curve) until the character shaders are ported.
//
// Bindings (native RHI Vulkan plan): set 0 = b0 root constants, b1 palette,
// then the static sampler; the texture table is set 1.

[[vk::binding(0, 0)]] cbuffer C : register(b0) {
  float4 clip_row[4];
  float4 eye;        // xyz: camera position
  float4 fog;        // xyz: fog curve (scale, bias, power)
  float4 fog_color;  // xyz: linear fog colour
  float4 params;     // x: 1 = skinned; y: exposure; z: alpha test reference; w: 1 = textured
  float4 sun;        // xyz: direction to the sun
};

[[vk::binding(1, 0)]] cbuffer Palette : register(b1) {
  float4 bones[240];
};

[[vk::binding(2, 0)]] SamplerState smp_wrap : register(s0);
[[vk::binding(0, 1)]] Texture2D<float4> diffuse : register(t0);

struct VsIn {
  float3 pos : POSITION;
  float2 uv : TEXCOORD0;
  float3 nrm : NORMAL;
  float4 weights : BLENDWEIGHT;
  uint4 bone : BLENDINDICES;
};

struct VsOut {
  float4 pos : SV_Position;
  float3 world : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float3 nrm : TEXCOORD2;
  float fog : TEXCOORD3;
};

VsOut vs_main(VsIn i) {
  VsOut o;
  float4 p = float4(i.pos, 1.0f);
  float3 n = i.nrm;
  if (params.x > 0.5f) {
    float3 wp = float3(0.0f, 0.0f, 0.0f);
    float3 wn = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int k = 0; k < 4; ++k) {
      const uint row = min(i.bone[k], 79u) * 3u;
      const float w = i.weights[k];
      wp += w * float3(dot(p, bones[row]), dot(p, bones[row + 1]), dot(p, bones[row + 2]));
      wn += w * float3(dot(n, bones[row].xyz), dot(n, bones[row + 1].xyz),
                       dot(n, bones[row + 2].xyz));
    }
    p = float4(wp, 1.0f);
    n = wn;
  }
  o.pos = float4(dot(clip_row[0], p), dot(clip_row[1], p), dot(clip_row[2], p),
                 dot(clip_row[3], p));
  o.world = p.xyz;
  o.uv = i.uv;
  o.nrm = n;
  // Fog needs world positions: skinned meshes only.
  o.fog = params.x > 0.5f
              ? pow(saturate(length(p.xyz - eye.xyz) * fog.x + fog.y), fog.z)
              : 0.0f;
  return o;
}

float4 ps_main(VsOut i) : SV_Target {
  float4 texel = params.w > 0.5f ? diffuse.Sample(smp_wrap, i.uv) : float4(0.6f, 0.6f, 0.6f, 1.0f);
  if (params.z > 0.0f) clip(texel.a - params.z);
  const float3 n = normalize(i.nrm);
  const float sun_term = saturate(dot(n, sun.xyz));
  float3 color = texel.rgb * texel.rgb * (0.35f + 1.1f * sun_term);
  color = color * (1.0f - i.fog) + fog_color.rgb * i.fog;
  // The game's tone curve (exposure e), then sqrt back.
  const float e = params.y;
  const float3 toe = saturate(1.0f - color * e);
  const float3 tone = (max(color * 0.25f * e + 0.75f, 1.0f) - toe * toe) * 0.5f;
  return float4(sqrt(abs(tone)), texel.a);
}
