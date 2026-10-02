// Capture shaders (recorder plan §5.1 step 3): scale + letterbox the game's back buffer into the
// output size and convert it to NV12 in one pass. The target is a single R8_UNORM texture,
// W x (3H/2): its first H rows are the luma plane, the next H/2 rows the interleaved chroma plane
// (Cb, Cr, Cb, Cr ... W bytes per row) - exactly the NV12 memory layout, so the read-back is one copy.
//
// BT.601 full range, as in the codec plan §4.3:
//   Y  = 0.299 R + 0.587 G + 0.114 B
//   Cb = 128 - 0.168736 R - 0.331264 G + 0.5 B
//   Cr = 128 + 0.5 R - 0.418688 G - 0.081312 B
// Chroma is the average of the 2x2 block of output pixels it covers. Compiled offline by fxc into
// headers (see hook/CMakeLists.txt); shader model 4.0 so a feature-level 10_0 device can run it.

cbuffer Params : register(b0) {
    float4 rect;   // x, y = top-left of the picture in output (luma) pixels; z, w = its size (letterbox)
    float4 flags;  // x = 1: source is linear floating point (HDR), clamp and sRGB-encode; y = output height H
};

Texture2D<float4> source : register(t0);
SamplerState linear_clamp : register(s0);

struct VsOut {
    float4 position : SV_Position;
};

VsOut vs_main(uint id : SV_VertexID) {
    // One triangle covering the target: (-1,1), (3,1), (-1,-3).
    VsOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 srgb_encode(float3 c) {
    c = saturate(c);
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return (c <= 0.0031308) ? low : high;
}

// Colour of the picture at a position given in output luma pixels (centre of the pixel = +0.5);
// black outside the letterboxed picture.
float3 picture(float2 luma_position) {
    float2 uv = (luma_position - rect.xy) / rect.zw;
    if (any(uv < 0.0) || any(uv > 1.0)) return float3(0, 0, 0);
    float3 c = source.SampleLevel(linear_clamp, uv, 0).rgb;
    return (flags.x > 0.5) ? srgb_encode(c) : c;
}

float ps_nv12(VsOut i) : SV_Target {
    float2 pixel = floor(i.position.xy);  // column, row of the output byte
    if (pixel.y < flags.y) {
        // Luma row.
        float3 c = picture(pixel + 0.5);
        return dot(c, float3(0.299, 0.587, 0.114));
    }
    // Chroma row: byte 2k is Cb and byte 2k+1 is Cr of chroma pixel k, which covers the 2x2 luma
    // pixels at (2k, 2r) .. (2k+1, 2r+1), r = row - H.
    float2 chroma = float2(floor(pixel.x * 0.5), pixel.y - flags.y);
    float2 base = chroma * 2.0;
    float3 c = picture(base + float2(0.5, 0.5)) + picture(base + float2(1.5, 0.5)) +
               picture(base + float2(0.5, 1.5)) + picture(base + float2(1.5, 1.5));
    c *= 0.25;
    bool is_cr = frac(pixel.x * 0.5) > 0.25;
    float cb = 128.0 / 255.0 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b;
    float cr = 128.0 / 255.0 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b;
    return is_cr ? cr : cb;
}
