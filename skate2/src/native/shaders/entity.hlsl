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
// Character shading follows the game's character pixel shaders (SH ambient,
// fill, sun; normal map, shadows and specular not yet); other entities get a
// placeholder (squared diffuse, sun + ambient). Both use the game's tone curve.
//
// Bindings (native RHI Vulkan plan): set 0 = b0 root constants, b1 palette,
// then the static sampler; the texture table is set 1.

[[vk::binding(0, 0)]] cbuffer C : register(b0) {
  float4 clip_row[4];
  float4 eye;        // xyz: camera position
  float4 fog;        // xyz: fog curve (scale, bias, power)
  float4 fog_color;  // xyz: linear fog colour
  float4 params;     // x: 1 = skinned; y: exposure; z: alpha test reference; w: 1 = textured
  float4 sun;        // xyz: direction to the sun; w: 1 = character lighting in Palette
};

[[vk::binding(1, 0)]] cbuffer Palette : register(b1) {
  float4 bones[216];
  // The character shader's lighting (pixel shader constants, see
  // WriteCharacterLighting): SH9 irradiance; light[0] sun direction + exposure,
  // [1] sun colour + ambient scale, [2] constant fill, [3] ambient floor.
  float4 sh[9];
  float4 light[4];
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
      const uint row = min(i.bone[k], 71u) * 3u;
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
  float3 color;
  if (sun.w > 0.5f) {
    // The game's character lighting (ucode of 3BE9E8635A346045) without the
    // normal map, shadow and specular terms: an order-2 SH irradiance in the
    // game's basis, a floor, a constant fill and the sun.
    const float3 shl = sh[0].rgb + n.x * sh[1].rgb + n.y * sh[2].rgb + n.z * sh[3].rgb +
                       n.x * n.z * sh[4].rgb + n.z * n.y * sh[5].rgb + n.y * n.x * sh[6].rgb +
                       (3.0f * n.z * n.z - 1.0f) * sh[7].rgb +
                       (n.x * n.x - n.y * n.y) * sh[8].rgb;
    const float3 lit = max(shl, light[3].rgb) * light[1].w + light[2].rgb +
                       light[1].rgb * saturate(dot(n, light[0].xyz));
    color = texel.rgb * texel.rgb * lit;
  } else {
    color = texel.rgb * texel.rgb * (0.35f + 1.1f * saturate(dot(n, sun.xyz)));
  }
  color = color * (1.0f - i.fog) + fog_color.rgb * i.fog;
  // The game's tone curve (exposure e), then sqrt back.
  const float e = params.y;
  const float3 toe = saturate(1.0f - color * e);
  const float3 tone = (max(color * 0.25f * e + 0.75f, 1.0f) - toe * toe) * 0.5f;
  return float4(sqrt(abs(tone)), texel.a);
}
