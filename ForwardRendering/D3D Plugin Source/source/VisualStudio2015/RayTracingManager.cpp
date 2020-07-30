#include "RayTracingManager.h"
#include "GraphicManager.h"
#include "MeshManager.h"
#include "RendererManager.h"

void RayTracingManager::Release()
{
	scratchTop.reset();
	topLevelAS.reset();
	rayTracingInstance.reset();
}

void RayTracingManager::InitRayTracingInstance()
{
	GraphicManager::Instance().ResetCreationList();
	auto dxrCmd = GraphicManager::Instance().GetDxrList();

	// build bottom AS
	MeshManager::Instance().CreateBottomAccelerationStructure(dxrCmd);

	// build top AS
	CreateTopAccelerationStructure(dxrCmd);

	GraphicManager::Instance().ExecuteCreationList();
	GraphicManager::Instance().WaitForGPU();
}

void RayTracingManager::CreateTopAccelerationStructure(ID3D12GraphicsCommandList5* _dxrList)
{
	auto renderers = RendererManager::Instance().GetRenderers();

	// prepare top level AS build
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = {};
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& topLevelInputs = topLevelBuildDesc.Inputs;
	topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	topLevelInputs.NumDescs = (UINT)renderers.size();

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
	GraphicManager::Instance().GetDxrDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);

	if (topLevelPrebuildInfo.ResultDataMaxSizeInBytes == 0)
	{
		LogMessage(L"[SqGraphic Error]: Create Top Acc Struct Failed.");
		return;
	}

	// create scratch & AS
	scratchTop = make_unique<DefaultBuffer>(GraphicManager::Instance().GetDevice(), topLevelPrebuildInfo.ScratchDataSizeInBytes, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	topLevelAS = make_unique<DefaultBuffer>(GraphicManager::Instance().GetDevice(), topLevelPrebuildInfo.ResultDataMaxSizeInBytes, true, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

	// prepare ray tracing instance desc
	vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs;
	instanceDescs.resize(renderers.size());

	int i = 0;
	for (auto& r : renderers)
	{
		instanceDescs[i] = {};
		instanceDescs[i].InstanceMask = 1;
		instanceDescs[i].AccelerationStructure = r->GetMesh()->GetBottomAS()->GetGPUVirtualAddress();
		XMStoreFloat3x4(reinterpret_cast<XMFLOAT3X4*>(instanceDescs[i].Transform), XMLoadFloat4x4(&r->GetWorld()));

		i++;
	}
}