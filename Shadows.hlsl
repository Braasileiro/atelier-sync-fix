#ifndef ALPHA
#define ALPHA 0
#endif

cbuffer Globals {
	float4 sBias;
};

#if ALPHA >= 2
Texture2D<float4> sStage0 : register(t0);
SamplerState smpsStage0 : register(s0);
#endif

struct PSInput
{
	float4 t0 : TEXCOORD0;
#if ALPHA >= 2
	float2 t1 : TEXCOORD1;
#endif
};

float4 main(float4 pos : SV_Position, PSInput input) : SV_TARGET
{
#if ALPHA == 1
	clip(1.0 - sBias.w);
#elif ALPHA == 2
	float4 color = sStage0.Sample(smpsStage0, input.t1);
	clip(color.w - sBias.w);
#endif
	float depth = input.t0.z / input.t0.w;
	float depthSlope = abs(ddx_fine(depth)) + abs(ddy_fine(depth));
	float bias = depthSlope * sBias.x + (sBias.y / 5.0);
	return depth + bias;
}
