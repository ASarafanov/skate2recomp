// Native renderer, first stage: untextured world geometry.
//
// Positions are world space (the static world needs no per-object matrix) and
// are transformed by the camera view-projection the game keeps at
// camera+0xA0, as row vectors (v * M, D3D convention). The pixel shader lights
// each face from its derivative normal, so the shapes read without textures.
//
// Bindings follow the native RHI's Vulkan plan: set 0 holds the buffer
// parameters in order, so the root constants (register b0) are set 0,
// binding 0.

[[vk::binding(0, 0)]] cbuffer C : register(b0) {
  row_major float4x4 view_proj;
  float4 color;
};

struct VsIn {
  float3 pos : POSITION;
};

struct VsOut {
  float4 pos : SV_Position;
  float3 world : TEXCOORD0;
};

VsOut vs_main(VsIn i) {
  VsOut o;
  o.pos = mul(float4(i.pos, 1.0f), view_proj);
  o.world = i.pos;
  return o;
}

float4 ps_main(VsOut i) : SV_Target {
  const float3 n = normalize(cross(ddx(i.world), ddy(i.world)));
  const float3 sun = normalize(float3(0.35f, 0.85f, 0.4f));
  const float light = 0.3f + 0.7f * abs(dot(n, sun));
  return float4(color.rgb * light, 1.0f);
}
