#include "FXAA.h"
#include "../ShaderManager.h"
#include "../MaterialManager.h"
#include "../GraphicManager.h"
#include "../Formatter.h"
#include "../ResourceManager.h"

Material FXAA::fxaaComputeMat;
ComPtr<ID3D12Resource> FXAA::tmpSrc;
DescriptorHeapData FXAA::fxaaHeapData;
FXAAConstant FXAA::fxaaConstantCPU;

void FXAA::Init()
{
	// init shader
	Shader* fxaa = ShaderManager::Instance().CompileShader(L"FXAACompute.hlsl");
	if (fxaa != nullptr)
	{
		fxaaComputeMat = MaterialManager::Instance().CreateComputeMat(fxaa);
	}

	// init heap data
	fxaaHeapData.AddSrv(nullptr, TextureInfo());
}

void FXAA::Release()
{
	fxaaComputeMat.Release();
	tmpSrc.Reset();
}

void FXAA::FXAACompute(ID3D12GraphicsCommandList* _cmdList, ID3D12Resource* _src, FXAAConstant _const, D3D12_GPU_DESCRIPTOR_HANDLE _outUav)
{
	// assume descriptor heap is binded
	if (!MaterialManager::Instance().SetComputePass(_cmdList, &fxaaComputeMat))
	{
		return;
	}

	// temp resource
	D3D12_RESOURCE_DESC desc = _src->GetDesc();
	desc.Format = Formatter::GetColorFormatFromTypeless(desc.Format);
	CreateTempResource(ResourceManager::Instance().RequestHeap(desc), desc);

	// copy input source to temp resource
	D3D12_RESOURCE_STATES states[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
	GraphicManager::Instance().CopyResourceWithBarrier(_cmdList, _src, tmpSrc.Get(), states, states);

	// upload constant
	fxaaConstantCPU = _const;
	UploadConstant(desc);

	// bind roots
	_cmdList->SetComputeRootDescriptorTable(0, _outUav);
	_cmdList->SetComputeRootConstantBufferView(1, GraphicManager::Instance().GetSystemConstantGPU());
	_cmdList->SetComputeRoot32BitConstants(2, sizeof(fxaaConstantCPU) / 4, &fxaaConstantCPU, 0);
	_cmdList->SetComputeRootDescriptorTable(3, GetFxaaSrv());
	_cmdList->SetComputeRootDescriptorTable(4, ResourceManager::Instance().GetSamplerHeap()->GetGPUDescriptorHandleForHeapStart());

	// dispatch
	int computeKernel = 8;
	_cmdList->Dispatch((UINT)(desc.Width + computeKernel) / computeKernel, (desc.Height + computeKernel) / computeKernel, 1);
}

void FXAA::UploadConstant(D3D12_RESOURCE_DESC _desc)
{
	// copy data
	fxaaConstantCPU.targetSize.x = (float)_desc.Width;
	fxaaConstantCPU.targetSize.y = (float)_desc.Height;
	fxaaConstantCPU.targetSize.z = 1.0f / (float)_desc.Width;
	fxaaConstantCPU.targetSize.w = 1.0f / (float)_desc.Height;
}

void FXAA::CreateTempResource(ID3D12Heap* _heap, D3D12_RESOURCE_DESC _desc)
{
	// create placed resource
	tmpSrc.Reset();
	LogIfFailedWithoutHR(GraphicManager::Instance().GetDevice()->CreatePlacedResource(_heap
		, 0
		, &_desc
		, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
		, nullptr
		, IID_PPV_ARGS(tmpSrc.GetAddressOf())));

	// create srv
	fxaaHeapData.UpdateSrv(tmpSrc.Get(), TextureInfo());
}

D3D12_GPU_DESCRIPTOR_HANDLE FXAA::GetFxaaSrv()
{
	return ResourceManager::Instance().GetTexHandle(fxaaHeapData.Srv());
}
