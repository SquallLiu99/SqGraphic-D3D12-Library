#pragma once
#include "Shader.h"
#include "Camera.h"
#include "Material.h"

class MaterialManager
{
public:
	MaterialManager(const MaterialManager&) = delete;
	MaterialManager(MaterialManager&&) = delete;
	MaterialManager& operator=(const MaterialManager&) = delete;
	MaterialManager& operator=(MaterialManager&&) = delete;

	static MaterialManager& Instance()
	{
		static MaterialManager instance;
		return instance;
	}

	MaterialManager() {}
	~MaterialManager() {}

	void Init();
	Material CreateMaterialFromShader(Shader *_shader, Camera _camera, D3D12_FILL_MODE _fillMode, D3D12_CULL_MODE _cullMode, int _srcBlend = 1, int _dstBlend = 1);
	Material CreateMaterialPost(Shader* _shader, Camera _camera, bool _zWrite);

	Material* AddMaterial(int _matInstanceId, int _renderQueue, int _cullMode, int _srcBlend, int _dstBlend);
	void Release();

private:
	static const int NUM_BLEND_MODE = 11;

	unordered_map<int, unique_ptr<Material>> materialTable;
	D3D12_BLEND blendTable[NUM_BLEND_MODE];
};