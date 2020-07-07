#include "LightManager.h"
#include "GraphicManager.h"
#include "ShaderManager.h"

void LightManager::Init(int _numDirLight, int _numPointLight, int _numSpotLight)
{
	maxDirLight = _numDirLight;
	maxPointLight = _numPointLight;
	maxSpotLight = _numSpotLight;

	// create srv buffer
	ID3D12Device *device = GraphicManager::Instance().GetDevice();
	for (int i = 0; i < MAX_FRAME_COUNT; i++)
	{
		dirLightData[i] = make_unique<UploadBuffer<SqLightData>>(device, maxDirLight, false);
		//pointLightData[i] = make_unique<UploadBuffer<SqLightData>>(device, maxPointLight, false);
		//spotLightData[i] = make_unique<UploadBuffer<SqLightData>>(device, maxSpotLight, false);
		lightConstant[i] = make_unique<UploadBuffer<LightConstant>>(device, MAX_CASCADE_SHADOW, true);
	}

	D3D_SHADER_MACRO shadowMacro[] = { "_SHADOW_CUTOFF_ON","1",NULL,NULL };
	Shader* shadowPassOpaque = ShaderManager::Instance().CompileShader(L"ShadowPass.hlsl");
	Shader* shadowPassCutoff = ShaderManager::Instance().CompileShader(L"ShadowPass.hlsl", shadowMacro);

	// create shadow material
	for (int i = 0; i < CullMode::NumCullMode; i++)
	{
		if (shadowPassOpaque != nullptr)
		{
			shadowOpaqueMat[i] = MaterialManager::Instance().CreateMaterialDepthOnly(shadowPassOpaque, D3D12_FILL_MODE_SOLID, (D3D12_CULL_MODE)(i + 1));
		}

		if (shadowPassCutoff != nullptr)
		{
			shadowCutoutMat[i] = MaterialManager::Instance().CreateMaterialDepthOnly(shadowPassCutoff, D3D12_FILL_MODE_SOLID, (D3D12_CULL_MODE)(i + 1));
		}
	}
}

void LightManager::InitNativeShadows(int _nativeID, int _numCascade, void** _shadowMapRaw)
{
	AddDirShadow(_nativeID, _numCascade, _shadowMapRaw);
}

void LightManager::Release()
{
	for (int i = 0; i < (int)dirLights.size(); i++)
	{
		dirLights[i].Release();
	}

	for (int i = 0; i < (int)pointLights.size(); i++)
	{
		pointLights[i].Release();
	}

	for (int i = 0; i < (int)spotLights.size(); i++)
	{
		spotLights[i].Release();
	}

	dirLights.clear();
	pointLights.clear();
	spotLights.clear();

	for (int i = 0; i < MAX_FRAME_COUNT; i++)
	{
		dirLightData[i].reset();
		pointLightData[i].reset();
		spotLightData[i].reset();
		lightConstant[i].reset();
	}

	for (int i = 0; i < CullMode::NumCullMode; i++)
	{
		shadowOpaqueMat[i].Release();
		shadowCutoutMat[i].Release();
	}
}

int LightManager::AddNativeLight(int _instanceID, SqLightData _data)
{
	if (_data.type == LightType::Directional)
	{
		return AddDirLight(_instanceID, _data);
	}
	else if (_data.type == LightType::Point)
	{
		return AddPointLight(_instanceID, _data);
	}
	else
	{
		return AddSpotLight(_instanceID, _data);
	}
}

void LightManager::UpdateNativeLight(int _id, SqLightData _data)
{
	if (_data.type == LightType::Directional)
	{
		dirLights[_id].SetLightData(_data);
	}
	else if (_data.type == LightType::Point)
	{
		pointLights[_id].SetLightData(_data);
	}
	else
	{
		spotLights[_id].SetLightData(_data);
	}
}

void LightManager::SetViewPortScissorRect(int _nativeID, D3D12_VIEWPORT _viewPort, D3D12_RECT _scissorRect)
{
	// currently onyl dir light needs 
	dirLights[_nativeID].SetViewPortScissorRect(_viewPort, _scissorRect);
}

void LightManager::UploadPerLightBuffer(int _frameIdx)
{
	// per light upload
	for (int i = 0; i < (int)dirLights.size(); i++)
	{
		if (dirLights[i].IsDirty(_frameIdx))
		{
			dirLightData[_frameIdx]->CopyData(i, *dirLights[i].GetLightData());
			dirLights[i].SetDirty(false, _frameIdx);
		}
	}
}

void LightManager::UploadLightConstant(LightConstant lc, int _cascade, int _frameIdx)
{
	lightConstant[_frameIdx]->CopyData(_cascade, lc);
}

void LightManager::FillSystemConstant(SystemConstant& _sc)
{
	_sc.numDirLight = (int)dirLights.size();
	_sc.numPointLight = 0;
	_sc.numSpotLight = 0;
}

Light* LightManager::GetDirLights()
{
	return dirLights.data();
}

int LightManager::GetNumDirLights()
{
	return (int)dirLights.size();
}

Material* LightManager::GetShadowOpqaue(int _cullMode)
{
	return &shadowOpaqueMat[_cullMode];
}

Material* LightManager::GetShadowCutout(int _cullMode)
{
	return &shadowCutoutMat[_cullMode];
}

ID3D12Resource* LightManager::GetDirLightResource(int _frameIdx)
{
	return dirLightData[_frameIdx]->Resource();
}

D3D12_GPU_VIRTUAL_ADDRESS LightManager::GetLightConstantGPU(int _cascade, int _frameIdx)
{
	UINT lightConstantSize = CalcConstantBufferByteSize(sizeof(LightConstant));
	return lightConstant[_frameIdx]->Resource()->GetGPUVirtualAddress() + _cascade * lightConstantSize;
}

int LightManager::FindLight(vector<Light> _lights, int _instanceID)
{
	for (int i = 0; i < (int)_lights.size(); i++)
	{
		if (_lights[i].GetInstanceID() == _instanceID)
		{
			return i;
		}
	}

	return -1;
}

int LightManager::AddDirLight(int _instanceID, SqLightData _data)
{
	// max light reached
	if ((int)dirLights.size() == maxDirLight)
	{
		return -1;
	}

	int idx = FindLight(dirLights, _instanceID);

	if (idx == -1)
	{
		Light newDirLight;
		newDirLight.Init(_instanceID, _data);
		dirLights.push_back(newDirLight);
		idx = (int)dirLights.size() - 1;

		// copy to buffer
		for (int i = 0; i < MAX_FRAME_COUNT; i++)
		{
			dirLightData[i]->CopyData(idx, _data);
		}
	}

	return idx;
}

int LightManager::AddPointLight(int _instanceID, SqLightData _data)
{
	return -1;
}

int LightManager::AddSpotLight(int _instanceID, SqLightData _data)
{
	return -1;
}

void LightManager::AddDirShadow(int _nativeID, int _numCascade, void** _shadowMapRaw)
{
	if (_nativeID < 0 || (int)_nativeID >= dirLights.size())
	{
		return;
	}

	dirLights[_nativeID].InitNativeShadows(_numCascade, _shadowMapRaw);
}
