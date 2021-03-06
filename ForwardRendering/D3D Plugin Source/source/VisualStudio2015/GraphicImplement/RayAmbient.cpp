#include "RayAmbient.h"
#include "../stdafx.h"
#include "../ShaderManager.h"
#include "../MaterialManager.h"
#include "../GraphicManager.h"
#include "../RayTracingManager.h"
#include "GaussianBlur.h"

void RayAmbient::Init(ID3D12Resource* _ambientRT, ID3D12Resource* _noiseTex)
{
	ambientSrc = _ambientRT;

	// create uav/srv for RT
	ambientHeapData.AddUav(ambientSrc, TextureInfo(true, false, true, false, false));
	ambientHeapData.AddSrv(ambientSrc, TextureInfo(true, false, false, false, false));

	// srv for noise
	noiseHeapData.AddSrv(_noiseTex, TextureInfo());

	// create material
	Shader* rtAmbient = ShaderManager::Instance().CompileShader(L"RayTracingAmbient.hlsl");
	if (rtAmbient != nullptr)
	{
		rtAmbientMat = MaterialManager::Instance().CreateRayTracingMat(rtAmbient);
	}

	Shader* regionFade = ShaderManager::Instance().CompileShader(L"AmbientRegionFade.hlsl");
	if (regionFade != nullptr)
	{
		ambientRegionFadeMat = MaterialManager::Instance().CreateComputeMat(regionFade);
	}

	CreateResource();
}

void RayAmbient::Release()
{
	rtAmbientMat.Release();
	ambientRegionFadeMat.Release();
	uniformVectorGPU.reset();
	ambientHitDistance.reset();
}

void RayAmbient::Trace(Camera* _targetCam, D3D12_GPU_VIRTUAL_ADDRESS _dirLightGPU)
{
	auto frameIndex = GraphicManager::Instance().GetFrameResource()->currFrameIndex;
	auto _cmdList = GraphicManager::Instance().GetFrameResource()->mainGfxList;

	LogIfFailedWithoutHR(_cmdList->Reset(GraphicManager::Instance().GetFrameResource()->mainGfxAllocator, nullptr));
	GPU_TIMER_START(_cmdList, GraphicManager::Instance().GetGpuTimeQuery());

	auto dxrCmd = GraphicManager::Instance().GetDxrList();
	if (!MaterialManager::Instance().SetRayTracingPass(dxrCmd, &rtAmbientMat))
	{
		return;
	}

	// copy hit group
	MaterialManager::Instance().CopyHitGroupIdentifier(GetMaterial(), HitGroupType::Ambient);

	// bind heap
	ID3D12DescriptorHeap* descriptorHeaps[] = { ResourceManager::Instance().GetTexHeap(), ResourceManager::Instance().GetSamplerHeap() };
	_cmdList->SetDescriptorHeaps(2, descriptorHeaps);

	// barriers before tracing
	D3D12_RESOURCE_BARRIER barriers[3];
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(_targetCam->GetRtvSrc(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(_targetCam->GetCameraDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	barriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(ambientSrc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	_cmdList->ResourceBarrier(3, barriers);

	// set roots
	_cmdList->SetComputeRootDescriptorTable(0, GetAmbientUav());
	_cmdList->SetComputeRootDescriptorTable(1, GetHitDistanceUav());
	_cmdList->SetComputeRootConstantBufferView(2, GraphicManager::Instance().GetSystemConstantGPU());
	_cmdList->SetComputeRootConstantBufferView(3, ambientConstantGPU->Resource()->GetGPUVirtualAddress());
	_cmdList->SetComputeRootShaderResourceView(4, RayTracingManager::Instance().GetTopLevelAS()->GetGPUVirtualAddress());
	_cmdList->SetComputeRootShaderResourceView(5, _dirLightGPU);
	_cmdList->SetComputeRootDescriptorTable(6, ResourceManager::Instance().GetTexHeap()->GetGPUDescriptorHandleForHeapStart());
	_cmdList->SetComputeRootDescriptorTable(7, ResourceManager::Instance().GetTexHeap()->GetGPUDescriptorHandleForHeapStart());
	_cmdList->SetComputeRootDescriptorTable(8, ResourceManager::Instance().GetTexHeap()->GetGPUDescriptorHandleForHeapStart());
	_cmdList->SetComputeRootDescriptorTable(9, ResourceManager::Instance().GetSamplerHeap()->GetGPUDescriptorHandleForHeapStart());
	_cmdList->SetComputeRootShaderResourceView(10, RayTracingManager::Instance().GetSubMeshInfoGPU(frameIndex));
	_cmdList->SetComputeRootShaderResourceView(11, uniformVectorGPU->Resource()->GetGPUVirtualAddress());

	// prepare dispatch desc
	D3D12_DISPATCH_RAYS_DESC dispatchDesc = rtAmbientMat.GetDispatchRayDesc((UINT)ambientSrc->GetDesc().Width, ambientSrc->GetDesc().Height);

	// setup hit group table
	auto hitGroup = MaterialManager::Instance().GetHitGroupGPU(HitGroupType::Ambient);
	dispatchDesc.HitGroupTable.StartAddress = hitGroup->Resource()->GetGPUVirtualAddress();
	dispatchDesc.HitGroupTable.SizeInBytes = hitGroup->Resource()->GetDesc().Width;
	dispatchDesc.HitGroupTable.StrideInBytes = hitGroup->Stride();

	// dispatch rays
	dxrCmd->DispatchRays(&dispatchDesc);

	// region fade
	AmbientRegionFade(_cmdList);

	// blur result
	GaussianBlur::BlurCompute(_cmdList, BlurConstant(ambientConst.blurRadius, ambientConst.blurDepthThres, ambientConst.blurNormalThres), ambientSrc, GetAmbientSrvHandle(), GetAmbientUav());

	// barriers after tracing
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(_targetCam->GetRtvSrc(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(_targetCam->GetCameraDepth(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barriers[2] = CD3DX12_RESOURCE_BARRIER::Transition(ambientSrc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	_cmdList->ResourceBarrier(3, barriers);

	GPU_TIMER_STOP(_cmdList, GraphicManager::Instance().GetGpuTimeQuery(), GameTimerManager::Instance().gpuTimeResult[GpuTimeType::RayTracingAmbient]);
	GraphicManager::Instance().ExecuteCommandList(_cmdList);
}

void RayAmbient::UpdataAmbientData(AmbientConstant _ac)
{
	ambientConst = _ac;
	ambientConst.ambientNoiseIndex = GetAmbientNoiseSrv();

	int vecCount = (int)ceil(sqrt(ambientConst.sampleCount));
	float gap = 1.0f / (float)vecCount;

	for (int i = 0; i < vecCount; i++)
	{
		for (int j = 0; j < vecCount; j++)
		{
			int idx = j + i * vecCount;
			uniformVectorCPU[idx].v.x = gap * j * 2.0f - 1.0f;
			uniformVectorCPU[idx].v.y = gap * i * 2.0f - 1.0f;
			uniformVectorCPU[idx].v.z = 1.0f;	// just forward on z
		}
	}

	uniformVectorGPU->CopyDataAll(uniformVectorCPU);

	ambientConstantGPU->CopyData(0, ambientConst);
}

int RayAmbient::GetAmbientSrv()
{
	return ambientHeapData.Srv();
}

int RayAmbient::GetAmbientNoiseSrv()
{
	return noiseHeapData.Srv();
}

Material* RayAmbient::GetMaterial()
{
	return &rtAmbientMat;
}

void RayAmbient::CreateResource()
{
	uniformVectorGPU = make_unique<UploadBuffer<UniformVector>>(GraphicManager::Instance().GetDevice(), maxSampleCount, false);
	ambientConstantGPU = make_unique<UploadBuffer<AmbientConstant>>(GraphicManager::Instance().GetDevice(), 1, true);

	auto desc = ambientSrc->GetDesc();
	desc.Format = DXGI_FORMAT_R16G16_TYPELESS;
	ambientHitDistance = make_unique<DefaultBuffer>(GraphicManager::Instance().GetDevice(), desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	// create uav/srv
	hitDistanceData.AddUav(ambientHitDistance->Resource(), TextureInfo(true, false, true, false, false));
	hitDistanceData.AddSrv(ambientHitDistance->Resource(), TextureInfo(true, false, false, false, false));
}

void RayAmbient::AmbientRegionFade(ID3D12GraphicsCommandList *_cmdList)
{
	if (!MaterialManager::Instance().SetComputePass(_cmdList, &ambientRegionFadeMat))
	{
		return;
	}

	// transition
	D3D12_RESOURCE_BARRIER barriers[1];
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(ambientHitDistance->Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	_cmdList->ResourceBarrier(1, barriers);

	// bind roots
	auto frameIdx = GraphicManager::Instance().GetFrameResource()->currFrameIndex;

	_cmdList->SetComputeRootDescriptorTable(0, GetAmbientUav());
	_cmdList->SetComputeRootConstantBufferView(1, GraphicManager::Instance().GetSystemConstantGPU());
	_cmdList->SetComputeRootConstantBufferView(2, ambientConstantGPU->Resource()->GetGPUVirtualAddress());
	_cmdList->SetComputeRootDescriptorTable(3, GetHitDistanceSrv());
	_cmdList->SetComputeRootDescriptorTable(4, ResourceManager::Instance().GetTexHeap()->GetGPUDescriptorHandleForHeapStart());
	_cmdList->SetComputeRootDescriptorTable(5, ResourceManager::Instance().GetSamplerHeap()->GetGPUDescriptorHandleForHeapStart());

	int computeKernel = 8;
	auto desc = ambientSrc->GetDesc();
	_cmdList->Dispatch(((UINT)desc.Width + computeKernel) / computeKernel, (desc.Height + computeKernel) / computeKernel, 1);

	// transition
	barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(ambientHitDistance->Resource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	_cmdList->ResourceBarrier(1, barriers);
}

D3D12_GPU_DESCRIPTOR_HANDLE RayAmbient::GetAmbientUav()
{
	return ResourceManager::Instance().GetTexHandle(ambientHeapData.Uav());
}

D3D12_GPU_DESCRIPTOR_HANDLE RayAmbient::GetAmbientSrvHandle()
{
	return ResourceManager::Instance().GetTexHandle(ambientHeapData.Srv());
}

D3D12_GPU_DESCRIPTOR_HANDLE RayAmbient::GetHitDistanceUav()
{
	return ResourceManager::Instance().GetTexHandle(hitDistanceData.Uav());
}

D3D12_GPU_DESCRIPTOR_HANDLE RayAmbient::GetHitDistanceSrv()
{
	return ResourceManager::Instance().GetTexHandle(hitDistanceData.Srv());
}
