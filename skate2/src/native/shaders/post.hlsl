// Native renderer: the game's last two post passes, transcribed from their
// ucode, with the constants of its own draws (NativeFrameConstants).
//
// Composite (pixel shader E063C9ADE96048FB): scene * c254.x (+ bloom, not yet)
// through a 32^3 colour grading table: coordinates (c1.c, c0.c, c2.c) *
// c255.xyx + c255.zwz, i.e. G, R (flipped), B with half-texel offsets. The
// table is an atlas of its slices side by side (NativeLutAtlas), filtered
// trilinearly here.
//
// Lens (pixel shader 9D5F41FC68105CB5), constants PS c0, c1, c254, c255:
//   centred coordinates q = uv - 0.5, aspect-corrected a = (u * 16/9 - 8/9, q.y);
//   barrel distortion: the scene is read at q * (c0.x * s^2 + c0.y) + 0.5,
//     s = dot(q, q) (0.72 s^2 + 0.9 in Freeskate);
//   vignette: a 32x32 texture read at (c1.x + c254.z - c0.z * |a|, c1.y),
//     plus c1.w, saturated, times the scene.
//
// Bindings (native RHI Vulkan plan): set 0 = b0 root constants, then the
// static sampler; the texture table is set 1.

[[vk::binding(0, 0)]] cbuffer C : register(b0) {
  float4 c0;
  float4 c1;
  float4 c254;
  float4 c255;
  float4 grade_row[3];   // composite c0..c2
  float4 grade_scale;    // composite c254 (x: scene scale)
  float4 grade_coord;    // composite c255
  float4 grade_flags;    // x: 1 = grading table bound; y: table size n
};

[[vk::binding(1, 0)]] SamplerState smp_clamp : register(s0);
[[vk::binding(0, 1)]] Texture2D<float4> scene : register(t0);
[[vk::binding(1, 1)]] Texture2D<float4> vignette : register(t1);
[[vk::binding(2, 1)]] Texture2D<float4> grade_lut : register(t2);

float3 Grade(float3 scene_color) {
  const float3 c = scene_color * grade_scale.x;
  if (grade_flags.x < 0.5f) return c;
  const float4 c4 = float4(c, 1.0f);
  const float3 t = float3(dot(grade_row[1], c4), dot(grade_row[0], c4), dot(grade_row[2], c4)) *
                       grade_coord.xyx + grade_coord.zwz;
  const float n = grade_flags.y;
  // Texel-space x and z (clamped to the slice), normalised y.
  const float x = clamp(t.x * n - 0.5f, 0.0f, n - 1.0f);
  const float z = clamp(t.z * n - 0.5f, 0.0f, n - 1.0f);
  const float z0 = floor(z);
  const float z1 = min(z0 + 1.0f, n - 1.0f);
  const float y = saturate(t.y);
  const float3 a = grade_lut.SampleLevel(smp_clamp, float2((x + 0.5f + z0 * n) / (n * n), y), 0).rgb;
  const float3 b = grade_lut.SampleLevel(smp_clamp, float2((x + 0.5f + z1 * n) / (n * n), y), 0).rgb;
  return lerp(a, b, z - z0);
}

struct VsOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

// One triangle covering the screen; uv (0, 0) is the top-left corner.
VsOut vs_main(uint id : SV_VertexID) {
  VsOut o;
  const float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv;
  o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
  return o;
}

float4 ps_main(VsOut i) : SV_Target {
  // r0.xyz = (u * c254.w + c254.y, v * c254.z + c254.x, u * c254.z + c254.x)
  const float ax = i.uv.x * c254.w + c254.y;
  const float qy = i.uv.y * c254.z + c254.x;
  const float qx = i.uv.x * c254.z + c254.x;
  const float r = sqrt(abs(ax * ax + qy * qy));
  const float s = qx * qx + qy * qy;
  const float k = c0.x * s * s + c0.y;
  float4 color = scene.Sample(smp_clamp, float2(qx, qy) * k + c255.x);
  color.rgb = Grade(color.rgb);
  const float2 vuv = float2(c1.x + c254.z - c0.z * r, c1.y);
  const float4 v = saturate(vignette.Sample(smp_clamp, vuv) + c1.w);
  return v * color;
}
