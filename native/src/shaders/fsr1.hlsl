// AMD FidelityFX FSR 1.0 pixel-shader integration for Direct3D 11.
// Upstream algorithm source and MIT license are in third_party/amd_fsr1.
#define A_GPU 1
#define A_HLSL 1
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1

#include "../third_party/amd_fsr1/ffx_a.h"
#include "../third_party/amd_fsr1/ffx_fsr1.h"

Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer FsrConstants : register(b0)
{
    uint4 Con0;
    uint4 Con1;
    uint4 Con2;
    uint4 Con3;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

AF4 FsrEasuRF(AF2 p) { return Source.GatherRed(LinearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return Source.GatherGreen(LinearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return Source.GatherBlue(LinearClamp, p); }

AF4 FsrRcasLoadF(ASU2 p)
{
    uint width;
    uint height;
    Source.GetDimensions(width, height);
    p = min(p, ASU2(width - 1, height - 1));
    return Source.Load(int3(p, 0));
}

void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b)
{
    // Captured SDR frames are already display-referred RGB; no transform is needed.
}

float4 LinearPS(VertexOutput input) : SV_Target
{
    return Source.Sample(LinearClamp, input.uv);
}

float4 FsrEasuPS(VertexOutput input) : SV_Target
{
    AF3 color;
    FsrEasuF(color, AU2(input.position.xy), Con0, Con1, Con2, Con3);
    return float4(color, 1.0);
}

float4 FsrRcasPS(VertexOutput input) : SV_Target
{
    AF1 r;
    AF1 g;
    AF1 b;
    AF1 a;
    FsrRcasF(r, g, b, a, AU2(input.position.xy), Con0);
    return float4(r, g, b, a);
}
