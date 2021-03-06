#define ResolveDepthRS "CBV(b0)," \
"RootConstants( num32BitConstants = 1, b1 )," \
"DescriptorTable(SRV(t0, numDescriptors=1))" 

#include "SqInput.hlsl"
#pragma sq_vertex ResolveDepthVS
#pragma sq_pixel ResolveDepthPS
#pragma sq_rootsig ResolveDepthRS

struct v2f
{
	float4 vertex : SV_POSITION;
};

static const float2 gTexCoords[6] =
{
    float2(0.0f, 1.0f),
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f)
};

cbuffer MsaaData : register(b1)
{
    int _MsaaCount;
}

Texture2DMS<float> _MsaaDepth : register(t0);

v2f ResolveDepthVS(uint vid : SV_VertexID)
{
	v2f o = (v2f)0;

    // convert uv to ndc space
    float2 uv = gTexCoords[vid];
    o.vertex = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0, 1);

	return o;
}

[RootSignature(ResolveDepthRS)]
void ResolveDepthPS(v2f i, out float oDepth : SV_Depth)
{
    float result = 0.0f;

    // choose max depth of all subsamples (reversed-z), which means closet depth
    // average like normal resolve isn't good idea for depth
    for (uint a = 0; a < _MsaaCount; a++)
    {
        float depth = _MsaaDepth.Load(i.vertex.xy, a).r;
        result = max(depth, result);
    }

    oDepth = result;
}