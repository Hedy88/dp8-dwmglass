// glass_blur.hlsl - glass blur and tint shader

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

cbuffer GlassConstants : register(b0) {
    float4 g_TintColor;
    float g_BlurRadius;
    float g_Opacity;
    float2 g_Resolution;
    float2 g_Padding;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float2 texel_size = 1.0f / g_Resolution;

    float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // gaussian blur - 9-tap
    float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };

    color += g_Texture.Sample(g_Sampler, input.tex) * weights[0];

    for (int i = 1; i < 5; i++) {
        float2 offset = texel_size * g_BlurRadius * i;
        color += g_Texture.Sample(g_Sampler, input.tex + float2(offset.x, 0.0f)) * weights[i];
        color += g_Texture.Sample(g_Sampler, input.tex - float2(offset.x, 0.0f)) * weights[i];
        color += g_Texture.Sample(g_Sampler, input.tex + float2(0.0f, offset.y)) * weights[i];
        color += g_Texture.Sample(g_Sampler, input.tex - float2(0.0f, offset.y)) * weights[i];
    }

    // apply tint
    color.rgb = lerp(color.rgb, g_TintColor.rgb, g_TintColor.a);

    // apply overall opacity
    color.a *= g_Opacity;

    return color;
}

// vertex shader - passthrough
struct VS_INPUT {
    float4 pos : POSITION;
    float2 tex : TEXCOORD;
};

float4 VSMain(VS_INPUT input) : SV_POSITION {
    return input.pos;
}
