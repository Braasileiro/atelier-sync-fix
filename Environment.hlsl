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

//#define SHADOW_IMPLEMENTATION_GAME
#define SHADOW_IMPLEMENTATION_GATHER

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
	float aref = vATest;
	// The game forgets to set a nonzero aref when fading objects in and out
	// This results in bushes increasing in size when they fade out, since suddenly more of the bush is shown
	// Hardcoding an aref at all times affects real transparent objects (like water) as well
	// Assume that objects getting faded in and out have a < 1 alpha multiplier, while normal transparent objects get their alpha only from the texture
	// (And hopefully the game never tries to fade water in or out...)
	if (atColor.a < 1)
		aref = 0.785;
	float4 color = (1.0).xxxx;
	if (nStageNum.x >= 0.5) {
		color = sStage0.Sample(smpsStage0, input.litCoord.zw);
	}
#if ALPHA == 1
	clip(color.a - aref);
#elif ALPHA == 2
	float pxAlpha = abs(ddx_fine(color.a)) + abs(ddy_fine(color.a));
	float a2c = saturate((color.a - aref) / pxAlpha + 0.5);
	if (a2c == 0)
		discard;
#endif
#endif
	float2 shadowBase = (input.shadow.xy / input.shadow.w);
	shadowBase += scSM2.xy; // -1.5px offset
	float shadowRef = saturate(input.shadow.z / input.shadow.w);
	float shadow = 0;
#ifdef SHADOW_IMPLEMENTATION_GAME
	// The shadow sampling algorithm used by the original game
	// Seems to be a full SampleCompare emulation, executed for a 4x4 grid
	// 64 (!) texture loads, and manual calculation of each coordinate
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
#elif !defined(SHADOW_IMPLEMENTATION_GATHER)
	// We can make things a little cheaper by realizing they don't actually sample 64 different pixels
	// Since each of the 16 bilinear operations samples four pixels in a square, and each operation is at a 1 pixel offset from the next, we only actually need to sample 25 pixels
	// In addition, we only need to calculate the fractional position once, and can use offset sampling for the rest
	// (Yes I realize there's a dedicated SampleCompare instruction, but that would require changing the sampler and I'm lazy.  Also, if you scroll down, we can actually do better than the 16 samples that would take)
	{
		float2 scaled = shadowBase * scSM.zw;
		float2 offset = frac(scaled - 0.5);
		float2 coord = shadowBase - offset * scSM.xy;
		float4 weights = float4(1 - offset, offset); // Weight for left, top, right, bottom edges
		float row[5];
		[unroll] for (int y = 0; y < 5; y++) {
			float px[5];
			[unroll] for (int x = 0; x < 5; x++) {
				px[x] = sShadow.SampleLevel(smpsShadow, coord, 0, int2(x, y)).x < shadowRef ? 0.0 : 1.0;
			}
			float px01 = px[0] * weights.x + px[1];
			float px34 = px[4] * weights.z + px[3];
			row[y] = px01 + px[2] + px34;
		}
		float row01 = row[0] * weights.y + row[1];
		float row34 = row[4] * weights.w + row[3];
		shadow = (row01 + row[2] + row34) * (1.0 / 16.0);
	}
#else // SHADOW_IMPLEMENTATION_GATHER
	// To add to the above, we can use gather instructions to get everything in 9 samples!
	// Annoyingly, the way the results are returned makes the nice loop above not so nice looking.
	// Use this chart for reference with variable names
	// A B C D E
	// F G H I J                                                    W Z
	// K L M N O   (Reminder: Gather gathers in this goofy pattern: X Y)
	// P Q R S T
	// U V W X Y
	{
		float2 scaled = shadowBase * scSM.zw;
		float2 offset = frac(scaled - 0.5);
		float2 coord = shadowBase - (offset - 0.5) * scSM.xy; // Gather wants a pixel corner, not a pixel center, so offset the offset
		float4 weights = float4(1 - offset, offset); // Weight for left, top, right, bottom edges
		float4 fgba = sShadow.Gather(smpsShadow, coord, int2(0, 0))    < shadowRef ? 0.0 : 1.0;
		float4 hidc = sShadow.Gather(smpsShadow, coord, int2(2, 0))    < shadowRef ? 0.0 : 1.0;
		float2 je   = sShadow.Gather(smpsShadow, coord, int2(3, 0)).yz < shadowRef ? 0.0 : 1.0;
		float4 pqlk = sShadow.Gather(smpsShadow, coord, int2(0, 2))    < shadowRef ? 0.0 : 1.0;
		float4 rsnm = sShadow.Gather(smpsShadow, coord, int2(2, 2))    < shadowRef ? 0.0 : 1.0;
		float2 to   = sShadow.Gather(smpsShadow, coord, int2(3, 2)).yz < shadowRef ? 0.0 : 1.0;
		float2 uv   = sShadow.Gather(smpsShadow, coord, int2(0, 3)).xy < shadowRef ? 0.0 : 1.0;
		float2 wx   = sShadow.Gather(smpsShadow, coord, int2(2, 3)).xy < shadowRef ? 0.0 : 1.0;
		float  y    = sShadow.Gather(smpsShadow, coord, int2(3, 3)).y  < shadowRef ? 0.0 : 1.0;
		float4 left  = float4(fgba.wx, pqlk.wx) * weights.x + float4(fgba.zy, pqlk.zy);
		float leftB  = uv.x * weights.x + uv.y;
		float4 mid   = float4(hidc.wx, rsnm.wx);
		float midB   = wx.x;
		float4 right = float4(je.yx, to.yx) * weights.z + float4(hidc.zy, rsnm.zy);
		float rightB = y * weights.z + wx.y;
		float4 rows = left + mid + right;
		float rowsB = leftB + midB + rightB;
		float row01 = rows.x * weights.y + rows.y;
		float row34 = rowsB * weights.w + rows.w;
		shadow = (row01 + rows.z + row34) * (1.0 / 16.0);
	}
#endif

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
