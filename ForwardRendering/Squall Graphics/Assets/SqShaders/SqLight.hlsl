#ifndef SQLIGHT
#define SQLIGHT
#include "SqInput.hlsl"

struct SqGI
{
	float3 indirectDiffuse;
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

float BlinnPhong(float m, float ndotH)
{
	return pow(ndotH, m);
}

//   BRDF = Fresnel & Blinn Phong
//   I = BRDF * NdotL
float3 AccumulateLight(SqLight light, float3 normal, float3 worldPos, float3 specColor, float smoothness, out float3 specular, float shadowAtten)
{
	float roughness = 1 - smoothness;
	float3 viewDir = -normalize(worldPos - _CameraPos.xyz);

	float3 col = 0;
	specular = 0;

	float distToLight = length(worldPos - light.world.xyz);
	[branch]
	if (distToLight > light.range)
	{
		// early out if not in range
		return 0;
	}

	float lightAtten = LightAtten(light.type, distToLight, light.range);

	// calc fresnel & blinn phong
	float3 lightColor = light.color.rgb * light.intensity * shadowAtten * lightAtten;
	float3 lightDir = -LightDir(light, worldPos);
	float3 halfDir = (viewDir + lightDir) / (length(viewDir + lightDir) + 0.00001f);	// safe normalize

	float ndotL = saturate(dot(normal, lightDir));
	float ldotH = saturate(dot(lightDir, halfDir));
	float ndotH = saturate(dot(normal, halfDir));

	col = lightColor * ndotL;
	specular = lightColor * SchlickFresnel(specColor, ldotH) * BlinnPhong(roughness, ndotH) * ndotL * 0.25f;

	return col;
}

float3 AccumulateDirLight(float3 normal, float3 worldPos, float3 specColor, float smoothness, out float3 specular, float shadowAtten)
{
	float3 col = 0;
	specular = 0;

	[loop]
	for (uint i = 0; i < _NumDirLight; i++)
	{
		col += AccumulateLight(_SqDirLight[i], normal, worldPos, specColor, smoothness, specular, shadowAtten);
	}

	return col;
}

float3 AccumulatePointLight(int tileIndex, int tileOffset, float3 normal, float3 worldPos, float3 specColor, float smoothness, out float3 specular, float shadowAtten)
{
	float3 col = 0;
	specular = 0;

	uint tileCount = _SqPointLightTile.Load(tileOffset);
	tileOffset += 4;

	// loop tile result only
	[loop]
	for (uint i = 0; i < tileCount; i++)
	{
		uint idx = _SqPointLightTile.Load(tileOffset);
		col += AccumulateLight(_SqPointLight[idx], normal, worldPos, specColor, smoothness, specular, shadowAtten);
		tileOffset += 4;
	}

	return col;
}

float3 LightBRDF(float3 diffColor, float3 specColor, float smoothness, float3 normal, float3 worldPos, float2 screenPos, float shadowAtten, SqGI gi)
{
	// get forward+ tile
	uint tileX = (uint)screenPos.x / TILE_SIZE;
	uint tileY = (uint)screenPos.y / TILE_SIZE;

	uint tileIndex = tileX + tileY * _TileCountX;
	int tileOffset = tileIndex * (_NumPointLight * 4 + 4);

	float3 dirSpecular = 0;
	float3 dirDiffuse = AccumulateDirLight(normal, worldPos, specColor, smoothness, dirSpecular, shadowAtten);

	float3 pointSpecular = 0;
	float3 pointDiffuse = AccumulatePointLight(tileIndex, tileOffset, normal, worldPos, specColor, smoothness, pointSpecular, shadowAtten);

	return diffColor * (dirDiffuse + pointDiffuse + gi.indirectDiffuse) + dirSpecular + pointSpecular;
}

SqGI CalcGI(float3 normal, float occlusion)
{
	SqGI gi = (SqGI)0;

	// hemi sphere sky light
	float up = normal.y * 0.5f + 0.5f;	// convert to [0,1]
	gi.indirectDiffuse = _AmbientGround.rgb + up * _AmbientSky.rgb;
	gi.indirectDiffuse *= occlusion;

	return gi;
}

#endif