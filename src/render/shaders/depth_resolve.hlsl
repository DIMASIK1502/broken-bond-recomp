// Copies a depth buffer into a float texture, for the game's depth resolves.
//
// On Xenos a resolve can copy the EDRAM depth into a k_24_8 texture that a
// later pass samples (the cutscene depth of field reads it to find what is
// out of focus). The host depth target is D32_FLOAT_S8_UINT and may be
// multisampled, which a copy or a resolve cannot turn into a sampled color
// texture, so this pass reads it in a shader and writes R32_FLOAT.
//
// The depth view sits in the renderer's 2D texture heap (set 0). A
// multisampled view uses the same descriptor type, so the heap is declared a
// second time as Texture2DMS. The first push constant holds the address of
// this pass's constants: depth texture index, 1 when it is multisampled, and
// the signed offset from a destination pixel to its source pixel.

struct PushConstants {
    uint64_t resolveConstants;
    uint64_t unused0;
    uint64_t unused1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_Push;

Texture2D<float> g_Depth[] : register(t0, space0);
Texture2DMS<float> g_DepthMS[] : register(t0, space0);

void VSMain(uint id : SV_VertexID, out float4 position : SV_Position)
{
    // One triangle covering the target.
    const float2 uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float PSMain(float4 position : SV_Position) : SV_Target
{
    const uint texture = vk::RawBufferLoad<uint>(g_Push.resolveConstants);
    const uint multisampled = vk::RawBufferLoad<uint>(g_Push.resolveConstants + 4);
    const int2 offset = int2(vk::RawBufferLoad<int>(g_Push.resolveConstants + 8),
                             vk::RawBufferLoad<int>(g_Push.resolveConstants + 12));
    const int2 pixel = int2(position.xy) + offset;
    if (multisampled != 0)
        return g_DepthMS[texture].Load(pixel, 0);
    return g_Depth[texture].Load(int3(pixel, 0));
}
