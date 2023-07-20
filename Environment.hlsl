#ifndef BLEND
// 0 => Single Source
// 1-3 => Dual Source (xy, yz, xz)
// 4-6 => Dual Source + RTT (xy, yz, xz)
#define BLEND 0
#endif
#ifndef ALPHA
// 0 => Ground (No Alpha)
// 1 => Objects (Normal Alpha)
// 2 => Objects (Alpha to Coverage)
#define ALPHA 0
#endif

cbuffer Globals {
#if ALPHA > 0
	float nStageNum;
	float vATest;
#endif
	float4 scSM;
	float4 scSM2;
#if ALPHA == 0
	float4 texsize0;
	float4 uvOffset0;
#if BLEND > 0
	float4 bmsize;
#endif
#else
	float4 atColor = 1;
	float HdrRangeInv = 1;
#endif
};

#if ALPHA == 0
Texture2D<float4> sShadow : register(t0);
SamplerState smpsShadow : register(s0);
Texture2D<float4> sLit : register(t1);
SamplerState smpsLit : register(s1);
Texture2D<float4> sLand0 : register(t2);
SamplerState smpsLand0 : register(s2);
#if BLEND > 0
Texture2D<float4> sLand1 : register(t3);
SamplerState smpsLand1 : register(s3);
Texture2D<float4> sBlendMap : register(t4);
SamplerState smpsBlendMap : register(s4);
#endif
#if BLEND > 3
Texture2D<float4> sRtt : register(t5);
SamplerState smpsRtt : register(s5);
#endif
#else
Texture2D<float4> sStage0 : register(t0);
SamplerState smpsStage0 : register(s0);
Texture2D<float4> sShadow : register(t1);
SamplerState smpsShadow : register(s1);
Texture2D<float4> sLit : register(t2);
SamplerState smpsLit : register(s2);
#endif

struct PSInput
{
#if ALPHA == 0
	float4 texCoord : TEXCOORD0;
	float4 litBase : TEXCOORD1;
	float4 shadow : TEXCOORD2;
	float3 baseColor : TEXCOORD3;
	float3 texColorWeight : TEXCOORD4;
#if BLEND > 3
	float3 rttCoord : TEXCOORD5;
	float3 litColor : TEXCOORD6;
	float2 litCoord : TEXCOORD7;
#else
	float3 litColor : TEXCOORD5;
	float2 litCoord : TEXCOORD6;
#endif
#else
	float4 litBase : TEXCOORD0;
	float4 shadow : TEXCOORD1;
	float4 litCoord : TEXCOORD2;
	float3 baseColor : TEXCOORD3;
	float3 texColorWeight : TEXCOORD4;
	float3 litColor : TEXCOORD5;
#endif
};

float4 main(float4 pos : SV_Position, PSInput input) : SV_TARGET{
#if ALPHA != 0
	float3 lit = sLit.Sample(smpsLit, input.litCoord.xy).xyz * input.litColor;
	float4 color = (1.0).xxxx;
	if (nStageNum.x >= 0.5) {
		color = sStage0.Sample(smpsStage0, input.litCoord.zw);
	}
#if ALPHA == 1
	clip(color.a - vATest);
#elif ALPHA == 2
	float pxAlpha = abs(ddx_fine(color.a)) + abs(ddy_fine(color.a));
	float a2c = saturate((color.a - vATest) / pxAlpha + 0.5);
	if (a2c == 0)
		discard;
#endif
#endif
	float2 shadowBase = (input.shadow.xy / input.shadow.w);
	shadowBase += scSM2.xy; // -1.5px offset
	float shadowRef = saturate(input.shadow.z / input.shadow.w);
	float shadow = 0;
	[unroll] for (uint x = 0; x < 4; x++) {
		float acc = 0;
		[unroll] for (uint y = 0; y < 4; y++) {
			float2 coord = shadowBase + scSM.xy * float2(uint2(x, y));
			float2 scaled = coord * scSM.zw; // Scale to size of shadow texture
			float2 offset = frac(scaled - 0.5); // Get offset from center of pixel
			coord -= offset * scSM.xy; // Center coordinate on pixel
			float4 coords = float4(coord, coord + scSM.xy);
			float lt = sShadow.SampleLevel(smpsShadow, coords.xy, 0).x;
			float rt = sShadow.SampleLevel(smpsShadow, coords.zy, 0).x;
			float lb = sShadow.SampleLevel(smpsShadow, coords.xw, 0).x;
			float rb = sShadow.SampleLevel(smpsShadow, coords.zw, 0).x;
			float4 cmp = float4(lt, lb, rt, rb) < shadowRef ? 0.0 : 1.0;
			float2 tmp = lerp(cmp.xy, cmp.zw, offset.xx);
			acc += lerp(tmp.x, tmp.y, offset.y);
		}
		shadow += acc;
	}
	shadow *= (1.0 / 16.0);

#if ALPHA == 0

	float3 lit = sLit.Sample(smpsLit, input.litCoord).xyz * input.litColor;
	lit = lit * shadow + input.litBase.xyz;
#if BLEND > 0
	float2 blendCoord = input.texCoord.xz * bmsize.xy + bmsize.zw;
#if BLEND == 1 || BLEND == 4
	float2 blend = sBlendMap.Sample(smpsBlendMap, blendCoord).xy;
#elif BLEND == 2 || BLEND == 5
	float2 blend = sBlendMap.Sample(smpsBlendMap, blendCoord).yz;
#elif BLEND == 3 || BLEND == 6
	float2 blend = sBlendMap.Sample(smpsBlendMap, blendCoord).zx;
#endif
	float mod = blend.x + blend.y + 0.001;
#if BLEND > 3
	float2 rttCoord = input.rttCoord.xy / input.rttCoord.zz;
	float4 rtt = sRtt.Sample(smpsRtt, rttCoord);
	mod += rtt.a * 2;
#endif
	blend /= mod;
	float4 texcoord = input.texCoord.xzzx * texsize0;
	float3 tex0 = sLand0.Sample(smpsLand0, texcoord.xy).xyz;
	float3 tex1 = sLand1.Sample(smpsLand1, texcoord.wz).xyz;
	float3 tex = tex0 * blend.xxx + tex1 * blend.yyy;
#if BLEND > 3
	tex += rtt.xyz / mod;
#endif
#else // BLEND == 0
	float3 tex = sLand0.Sample(smpsLand0, input.texCoord.xz * texsize0.xy);
#endif
	float3 color = lerp(input.baseColor, lit * tex, input.texColorWeight);
	return float4(color * 0.5, input.litBase.a);

#else // ALPHA != 0

	shadow *= max((atColor.x - 0.7) * 3, 0);
	lit = lit * shadow + input.litBase.xyz;
	color.a *= input.litBase.a;
	color.rgb = lerp(input.baseColor, lit * color.rgb, input.texColorWeight);
	color = color * saturate(atColor) + saturate(atColor - 1);
#if ALPHA == 1
	return float4(color.rgb * HdrRangeInv, color.a);
#else
	return float4(color.rgb * HdrRangeInv, a2c);
#endif

#endif
}
