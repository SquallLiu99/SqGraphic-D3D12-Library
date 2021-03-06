#ifndef SQLIGHT
#define SQLIGHT
#include "SqInput.hlsl"

struct SqGI
{
	float3 indirectDiffuse;
	float3 indirectSpecular;
};

struct LightResult
{
	float3 diffuse;
	float3 specular;
};

float3 LightDir(SqLight light, float3 worldPos)
{
	return lerp(normalize(worldPos - light.world.xyz), light.world.xyz, light.type == 1);
}

float LightAtten(int lightType, float distToLight, float range, bool isLinear = false)
{
	[branch]
	if (lightType == 1)
	{
		return 1.0f;
	}

	// square atten
	float atten = 1 - saturate(distToLight / range);

	[branch]
	if (!isLinear)
		atten *= atten;

	return atten;
}

float3 SchlickFresnel(float3 specColor, float ldotH)
{
	float cosIncidentAngle = ldotH;

	// pow5
	float f0 = 1.0f - cosIncidentAngle;
	float3 reflectPercent = specColor + (1.0f - specColor) * (f0 * f0 * f0 * f0 * f0);

	return reflectPercent;
}

float3 SchlickFresnelLerp(float3 F0, float3 F90, float ldotH)
{
	float cosIncidentAngle = ldotH;

	// pow5
	float f0 = 1.0f - cosIncidentAngle;
	float3 reflectPercent = lerp(F0, F90, (f0 * f0 * f0 * f0 * f0));

	return reflectPercent;
}

// formula from (Introduction to 3D Game Programming with DirectX 12 by Frank Luna)
float BlinnPhong(float m, float ndotH)
{
	m *= m;
	m *= m;
	m = max(m, FLOAT_EPSILON);
	m *= 256.0f;
	float n = (m + 64.0f) / 64.0f;

	return pow(ndotH, m) * n;
}

//   BRDF = Fresnel & Blinn Phong
//   I = BRDF * NdotL
LightResult AccumulateLight(SqLight light, float3 normal, float3 worldPos, float3 specColor, float smoothness, float shadowAtten)
{
	float3 viewDir = -normalize(worldPos - _CameraPos.xyz);

	LightResult result = (LightResult)0;

	float distToLight = length(worldPos - light.world.xyz);
	[branch]
	if (distToLight > light.range)
	{
		// early out if not in range
		return result;
	}

	float lightAtten = LightAtten(light.type, distToLight, light.range);

	// calc fresnel & blinn phong
	float3 lightColor = light.color.rgb * light.intensity * shadowAtten * lightAtten;
	float3 lightDir = -LightDir(light, worldPos);
	float3 halfDir = (viewDir + lightDir) / (length(viewDir + lightDir) + FLOAT_EPSILON);	// safe normalize

	float ndotL = saturate(dot(normal, lightDir));
	float ldotH = saturate(dot(lightDir, halfDir));
	float ndotH = saturate(dot(normal, halfDir));

	result.diffuse = lightColor * ndotL;
	result.specular = lightColor * SchlickFresnel(specColor, ldotH) * BlinnPhong(smoothness, ndotH) * ndotL * 0.25f;

	return result;
}

float3 AccumulateDirLight(float3 normal, float3 worldPos, float3 specColor, float smoothness, out float3 specular, float shadowAtten)
{
	float3 col = 0;
	specular = 0;

	for (uint i = 0; i < _NumDirLight; i++)
	{
		LightResult result = AccumulateLight(_SqDirLight[i], normal, worldPos, specColor, smoothness, shadowAtten);
		col += result.diffuse;
		specular += result.specular;
	}

	return col;
}

float3 AccumulatePointLight(int tileOffset, float3 normal, float3 worldPos, float3 specColor, float smoothness, out float3 specular, float shadowAtten)
{
	float3 col = 0;
	specular = 0;

	uint tileCount = _SqPointLightTile.Load(tileOffset);
	tileOffset += 4;

	// loop tile result only
	for (uint i = 0; i < tileCount; i++)
	{
		uint idx = _SqPointLightTile.Load(tileOffset);
		LightResult result = AccumulateLight(_SqPointLight[idx], normal, worldPos, specColor, smoothness, shadowAtten);
		col += result.diffuse;
		specular += result.specular;

		tileOffset += 4;
	}

	return col;
}

float3 LightBRDF(float3 diffColor, float3 specColor, float smoothness, float3 normal, float3 worldPos, float2 screenPos, float shadowAtten, SqGI gi)
{
	// get forward+ tile
	uint tileX = (uint)(screenPos.x) / TILE_SIZE;
	uint tileY = (uint)(screenPos.y) / TILE_SIZE;

	uint tileIndex = tileX + tileY * _TileCountX;
	int tileOffset = GetPointLightOffset(tileIndex);

	// dir light
	float3 dirSpecular = 0;
	float3 dirDiffuse = AccumulateDirLight(normal, worldPos, specColor, smoothness, dirSpecular, shadowAtten);

	// point light
	float3 pointSpecular = 0;
	float3 pointDiffuse = AccumulatePointLight(tileOffset, normal, worldPos, specColor, smoothness, pointSpecular, shadowAtten);

	// GI specular
	float3 view = -normalize(worldPos - _CameraPos.xyz);
	float nv = abs(dot(normal, view));

	float specFade = smoothness * smoothness;

#if defined(_FRESNEL_EFFECT)
	float3 giSpec = specFade * SchlickFresnelLerp(specColor, gi.indirectSpecular, nv);
#else
	float3 giSpec = specFade * gi.indirectSpecular * SchlickFresnel(specColor, nv);
#endif

	return diffColor * (dirDiffuse + pointDiffuse + gi.indirectDiffuse) + dirSpecular + pointSpecular + giSpec;
}

float3 LightBRDFSimple(float3 diffColor, float3 specColor, float smoothness, float3 normal, float3 worldPos, float shadowAtten, SqGI gi, float useSpecular)
{
	// directional light only
	float3 dirSpecular = 0;
	float3 dirDiffuse = AccumulateDirLight(normal, worldPos, specColor, smoothness, dirSpecular, shadowAtten);

	float3 view = -normalize(worldPos - _CameraPos.xyz);
	float nv = abs(dot(normal, view));
	float specFade = smoothness * smoothness;
	float3 giSpec = specFade * gi.indirectSpecular * SchlickFresnel(specColor, nv);

	return diffColor * (dirDiffuse + gi.indirectDiffuse) + dirSpecular * useSpecular + giSpec;
}

SqGI CalcGISimple(float3 normal, float occlusion)
{
	SqGI gi = (SqGI)0;

	// hemi sphere sky light
	float up = normal.y * 0.5f + 0.5f;	// convert to [0,1]
	gi.indirectDiffuse = _AmbientGround.rgb + up * _AmbientSky.rgb;
	gi.indirectDiffuse *= occlusion;

	return gi;
}

SqGI CalcGI(float3 normal, float2 screenUV, float smoothness, float occlusion, bool isTransparent, float depthFade)
{
	SqGI gi = (SqGI)0;

	[branch]
	if (!isTransparent)
	{
		// sample ray tracing AO
		float4 rtAmbient = SQ_SAMPLE_TEXTURE(_AmbientRTIndex, _LinearClampSampler, screenUV);

		// hemi sphere sky light
		float up = normal.y * 0.5f + 0.5f;	// convert to [0,1]
		gi.indirectDiffuse = rtAmbient.rgb + up * _AmbientSky.rgb;
		gi.indirectDiffuse *= min(occlusion, rtAmbient.a);
	}
	else
		gi = CalcGISimple(normal, occlusion);

#if defined(RAY_SHADER)
	// ray shader only calc diffuse
	return gi;
#endif

	// get texture dimensions
	float reflLevels = GetMipLevels(_SqTexTable[_ReflectionRTIndex]);

	// smooth to mip map
	float roughness = 1 - smoothness * smoothness;
	float specMip = roughness * reflLevels;

	// sample indirect specular
	[branch]
	if (!isTransparent)
	{
		gi.indirectSpecular = SQ_SAMPLE_TEXTURE_LEVEL(_ReflectionRTIndex, _LinearClampSampler, screenUV, specMip).rgb;
	}
	else
	{
		gi.indirectSpecular = SQ_SAMPLE_TEXTURE_LEVEL(_TransReflectionRTIndex, _LinearClampSampler, screenUV, specMip).rgb;
		gi.indirectSpecular = lerp(0, gi.indirectSpecular, depthFade);
	}

	gi.indirectSpecular *= occlusion;

	return gi;
}

#endif