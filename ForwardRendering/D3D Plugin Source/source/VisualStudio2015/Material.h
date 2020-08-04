#pragma once
#include <d3d12.h>
#include <wrl.h>
using namespace Microsoft::WRL;
#include "stdafx.h"
#include "UploadBuffer.h"
#include "FrameResource.h"
#include "Shader.h"

enum RenderQueue
{
	Opaque = 2000,
	CutoffStart = 2226,
	Cutoff = 2450,
	OpaqueLast = 2500,
	Transparent = 3000,
	TransparentLast = 3500,
};

enum CullMode
{
	Off, Front, Back, NumCullMode
};

enum BlendMode
{
	Zero = 0,
	One = 1,
	DstColor = 2,
	SrcColor = 3,
	OneMinusDstColor = 4,
	SrcAlpha = 5,
	OneMinusSrcColor = 6,
	DstAlpha = 7,
	OneMinusDstAlpha = 8,
	SrcAlphaSaturate = 9,
	OneMinusSrcAlpha = 10
};

class Material
{
public:
	bool CreatePsoFromDesc(D3D12_GRAPHICS_PIPELINE_STATE_DESC _desc);
	void CreateDxcPso(ComPtr<ID3D12StateObject> _pso, Shader *_shader);
	void AddMaterialConstant(UINT _byteSize, void* _data);
	void Release();
	void SetRenderQueue(int _queue);
	void SetCullMode(int _mode);
	void SetBlendMode(int _srcBlend, int _dstBlend);

	ID3D12PipelineState* GetPSO();
	ID3D12StateObject* GetDxcPSO();
	ID3D12RootSignature* GetRootSignature();
	int GetRenderQueue();
	CullMode GetCullMode();
	D3D12_GPU_VIRTUAL_ADDRESS GetMaterialConstantGPU(int _index);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC GetPsoDesc();
	D3D12_DISPATCH_RAYS_DESC GetDispatchRayDesc(UINT _width, UINT _height);
	bool IsRayTracingMat();

private:
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	CD3DX12_STATE_OBJECT_DESC dxcPsoDesc;

	ComPtr<ID3D12PipelineState> pso;
	ComPtr<ID3D12StateObject> dxcPso;
	shared_ptr<UploadBufferAny> materialConstant[MAX_FRAME_COUNT];
	shared_ptr<UploadBufferAny> rayGenShaderTable;
	shared_ptr<UploadBufferAny> missShaderTable;
	shared_ptr<UploadBufferAny> hitGroupTable;

	int renderQueue = 2000;
	CullMode cullMode = CullMode::Off;
	int srcBlend;
	int dstBlend;

	bool validMaterial = false;
	bool isDirty = true;
};