// Shows the frontbuffer on the swap chain through the game's gamma ramp.
//
// The Xbox 360 display controller maps each 8-bit channel of the frontbuffer
// through a 256-entry table of 10-bit values (DC_LUT_30_COLOR: blue in bits
// 0-9, green 10-19, red 20-29) that the game writes through DC_LUT
// registers. Broken Bond uses it to darken the shadows and midtones.
//
// Bindings match the renderer's pipeline layout: the 2D texture heap is set
// 0, samplers set 3, and the first push constant holds the address of this
// pass's constants: frontbuffer texture index, sampler index, two unused
// words, then the 256 table entries.

struct PushConstants {
    uint64_t presentConstants;
    uint64_t unused0;
    uint64_t unused1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_Push;

Texture2D<float4> g_Textures[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space3);

void VSMain(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    // One triangle covering the screen.
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    const uint texture = vk::RawBufferLoad<uint>(g_Push.presentConstants);
    const uint sampler = vk::RawBufferLoad<uint>(g_Push.presentConstants + 4);
    const float3 color = g_Textures[texture].SampleLevel(g_Samplers[sampler], uv, 0).rgb;
    const uint3 index = uint3(round(saturate(color) * 255.0));
    const uint red = vk::RawBufferLoad<uint>(g_Push.presentConstants + 16 + index.r * 4);
    const uint green = vk::RawBufferLoad<uint>(g_Push.presentConstants + 16 + index.g * 4);
    const uint blue = vk::RawBufferLoad<uint>(g_Push.presentConstants + 16 + index.b * 4);
    return float4(float3((red >> 20) & 1023, (green >> 10) & 1023, blue & 1023) / 1023.0, 1.0);
}
