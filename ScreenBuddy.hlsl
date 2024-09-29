//

cbuffer Constants : register(b0)
{
	float4 Size;
};

void VS(in uint Id: SV_VertexID, out float4 Position : SV_Position, out float2 TexCoord : TEXCOORD)
{
	float2 Corner = float2(uint2(Id & 1, Id >> 1) != 0);
	float2 Vertex = Corner * Size.xy + Size.zw;

	Position.xy = (Vertex * 2 - 1) * float2(1, -1);
	Position.z = 0;
	Position.w = 1;
	TexCoord = Corner;
}

//

Texture2D<float3> Texture : register(t0);
SamplerState LinearSampler : register(s0);

float3 PS(in float4 Position : SV_Position, in float2 TexCoord : TEXCOORD) : SV_TARGET
{
	return Texture.Sample(LinearSampler, TexCoord);
}
