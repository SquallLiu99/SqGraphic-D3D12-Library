#include "GraphicManager.h"
#include "stdafx.h"
#include "ForwardRenderingPath.h"
#include "d3dx12.h"

bool GraphicManager::Initialize(ID3D12Device* _device, int _numOfThreads)
{
	initSucceed = false;
	numOfLogicalCores = 0;
	mainFence = 0;

	mainDevice = _device;
#if defined(GRAPHICTIME)
	LogIfFailedWithoutHR(mainDevice->SetStablePowerState(true));
#endif

	// require logical cores number
	numOfLogicalCores = (int)std::thread::hardware_concurrency();
	numOfLogicalCores = (_numOfThreads > numOfLogicalCores) ? numOfLogicalCores : _numOfThreads;

	// reset frame index
	currFrameIndex = 0;

	// get descriptor size
	rtvDescriptorSize = mainDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	dsvDescriptorSize = mainDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	cbvSrvUavDescriptorSize = mainDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	initSucceed = SUCCEEDED(CreateGpuTimeQuery())
		&& SUCCEEDED(CreateGraphicCommand())
		&& SUCCEEDED(CreateGraphicFences())
		&& CreateGraphicThreads();

	for (int i = 0; i < MAX_FRAME_COUNT; i++)
	{
		systemConstantGPU[i] = make_unique<UploadBuffer<SystemConstant>>(_device, 1, true);
	}
    
	InitRayTracingInterface();
	return initSucceed;
}

void GraphicManager::InitRayTracingInterface()
{
	LogIfFailedWithoutHR(mainDevice->QueryInterface(IID_PPV_ARGS(&rayTracingDevice)));
	LogIfFailedWithoutHR(mainGfxList->QueryInterface(IID_PPV_ARGS(&rayTracingCmd)));
}

void GraphicManager::Release()
{
	WaitForGPU();

	mainGraphicFence.Reset();
	CloseHandle(mainFenceEvent);
	CloseHandle(beginRenderThread);
	CloseHandle(renderThreadHandle);
	CloseHandle(renderThreadFinish);

	for (int i = 0; i < MAX_WORKER_THREAD_COUNT; i++)
	{
		CloseHandle(beginWorkerThread);
		CloseHandle(workerThreadHandle[i]);
		CloseHandle(workerThreadFinish[i]);
	}

	for (int i = 0; i < MAX_FRAME_COUNT; i++)
	{
		mainGfxAllocator[i].Reset();

		for (int j = 0; j < numOfLogicalCores - 1; j++)
		{
			workerGfxAllocator[j][i].Reset();
			workerGfxList[j].Reset();
		}

		systemConstantGPU[i].reset();
		graphicFences[i] = 0;
	}
	mainGfxList.Reset();
	mainFence = 0;

	mainGraphicQueue.Reset();
	gpuTimeQuery.Reset();

	rayTracingCmd.Reset();
	rayTracingDevice.Reset();
	mainDevice = nullptr;
}

int GraphicManager::GetThreadCount()
{
	return numOfLogicalCores;
}

void GraphicManager::Update()
{
	GRAPHIC_TIMER_START;
	
	// move to next frame and wait
	currFrameIndex = (currFrameIndex + 1) % MAX_FRAME_COUNT;

	if (graphicFences[currFrameIndex] > 0 && mainGraphicFence->GetCompletedValue() < graphicFences[currFrameIndex])
	{
		LogIfFailedWithoutHR(mainGraphicFence->SetEventOnCompletion(graphicFences[currFrameIndex], mainFenceEvent));
		WaitForSingleObjectEx(mainFenceEvent, INFINITE, FALSE);
	}

	GRAPHIC_TIMER_STOP(GameTimerManager::Instance().gameTime.updateTime)
}

void GraphicManager::Render()
{
	// wake up render thread
	ResetEvent(renderThreadFinish);
	SetEvent(beginRenderThread);
	WaitForRenderThread();
}

HRESULT GraphicManager::CreateGpuTimeQuery()
{
	HRESULT hr = S_OK;

#if defined(GRAPHICTIME)
	D3D12_QUERY_HEAP_DESC queryDesc;
	ZeroMemory(&queryDesc, sizeof(queryDesc));
	queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	queryDesc.Count = 2;

	LogIfFailed(mainDevice->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&gpuTimeQuery)), hr);
#endif

	return hr;
}

HRESULT GraphicManager::CreateGraphicCommand()
{
	// create graphics command queue
	HRESULT hr = S_OK;

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	LogIfFailed(mainDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mainGraphicQueue)), hr);

#if defined(GRAPHICTIME)
	LogIfFailed(mainGraphicQueue->GetTimestampFrequency(&gpuFreq), hr);
#endif

	for (int i = 0; i < MAX_FRAME_COUNT; i++)
	{
		CreateGfxAlloc(mainGfxAllocator[i]);

		// create worker GFX list
		for (int j = 0; j < numOfLogicalCores - 1; j++)
		{
			CreateGfxAlloc(workerGfxAllocator[j][i]);

			if (i == 0)
			{
				CreateGfxList(workerGfxAllocator[j][i], workerGfxList[j]);
			}
		}
	}

	CreateGfxList(mainGfxAllocator[0], mainGfxList);

	return hr;
}

HRESULT GraphicManager::CreateGfxAlloc(ComPtr<ID3D12CommandAllocator>& _allocator)
{
	HRESULT hr = S_OK;

	// create allocator
	LogIfFailed(mainDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(_allocator.GetAddressOf())), hr);

	return hr;
}

HRESULT GraphicManager::CreateGfxList(ComPtr<ID3D12CommandAllocator>& _allocator, ComPtr<ID3D12GraphicsCommandList>& _list)
{
	HRESULT hr = S_OK;

	LogIfFailed(mainDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		_allocator.Get(),
		nullptr,
		IID_PPV_ARGS(_list.GetAddressOf())), hr);

	// close first because we will call reset at the beginning of render work
	LogIfFailed(_list->Close(), hr);

	return hr;
}

HRESULT GraphicManager::CreateGraphicFences()
{
	HRESULT hr = S_OK;

	graphicFences[currFrameIndex] = 0;
	LogIfFailed(mainDevice->CreateFence(graphicFences[currFrameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mainGraphicFence)), hr);
	graphicFences[currFrameIndex]++;

	mainFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (mainFenceEvent == nullptr)
	{
		hr = S_FALSE;
	}

	return hr;
}

bool GraphicManager::CreateGraphicThreads()
{
	bool result = true;

	struct threadwrapper
	{
		static unsigned int WINAPI render(LPVOID lpParameter)
		{
			ThreadParameter* parameter = reinterpret_cast<ThreadParameter*>(lpParameter);
			GraphicManager::Instance().RenderThread();
			return 0;
		}

		static unsigned int WINAPI worker(LPVOID lpParameter)
		{
			ThreadParameter* parameter = reinterpret_cast<ThreadParameter*>(lpParameter);
			ForwardRenderingPath::Instance().WorkerThread(parameter->threadIndex);
			return 0;
		}
	};

	// render thread 
	beginRenderThread = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	renderThreadFinish = CreateEvent(nullptr, TRUE, FALSE, nullptr);

	renderThreadHandle = reinterpret_cast<HANDLE>(_beginthreadex(
		nullptr,
		0,
		threadwrapper::render,
		nullptr,
		0,
		nullptr));

	result = (beginRenderThread != nullptr) && (renderThreadFinish != nullptr) && (renderThreadHandle != nullptr);

	// worker thread (number of cores - 1)
	for (int i = 0; i < numOfLogicalCores - 1; i++)
	{
		threadParams[i].threadIndex = i;
		beginWorkerThread[i] = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		workerThreadFinish[i] = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		workerThreadHandle[i] = reinterpret_cast<HANDLE>(_beginthreadex(
			nullptr,
			0,
			threadwrapper::worker,
			reinterpret_cast<LPVOID>(&threadParams[i]),
			0,
			nullptr));

		result &= (workerThreadHandle[i] != nullptr) && (workerThreadFinish[i] != nullptr) && (beginWorkerThread[i] != nullptr);
	}

	if (!result)
	{
		LogMessage(L"[SqGraphic Error] SqGraphic: Create Render Thread Failed.");
	}

	return result;
}

void GraphicManager::DrawCamera()
{
#if defined(GRAPHICTIME)
	GameTimerManager::Instance().gameTime.cullingTime = 0.0;
	GameTimerManager::Instance().gameTime.sortingTime = 0.0;
	GameTimerManager::Instance().gameTime.renderTime = 0.0;
	for (int i = 0; i < numOfLogicalCores - 1; i++)
	{
		GameTimerManager::Instance().gameTime.renderThreadTime[i] = 0.0;
		GameTimerManager::Instance().gameTime.batchCount[i] = 0;
	}
#endif

	// render path
	Camera* cam = CameraManager::Instance().GetCamera();
	if (cam->GetCameraData()->renderingPath == RenderingPathType::Forward)
	{
		ForwardRenderingPath::Instance().CullingWork(cam);
		ForwardRenderingPath::Instance().SortingWork(cam);
		ForwardRenderingPath::Instance().RenderLoop(cam, currFrameIndex);
	}
}

void GraphicManager::WaitForGPU()
{
	if (mainGraphicFence)
	{
		// advance value to make command reach here
		mainFence++;

		// Signal and increment the fence value.
		LogIfFailedWithoutHR(mainGraphicQueue->Signal(mainGraphicFence.Get(), mainFence));

		if (mainGraphicFence->GetCompletedValue() < mainFence)
		{
			LogIfFailedWithoutHR(mainGraphicFence->SetEventOnCompletion(mainFence, mainFenceEvent));

			// Wait until gpu is finished.
			WaitForSingleObject(mainFenceEvent, INFINITE);
		}
	}
}

void GraphicManager::ResetCreationList()
{
	// use pre gfx 0 as creation list
	LogIfFailedWithoutHR(mainGfxAllocator[0]->Reset());
	LogIfFailedWithoutHR(mainGfxList->Reset(mainGfxAllocator[0].Get(), nullptr));
}

void GraphicManager::ExecuteCreationList()
{
	LogIfFailedWithoutHR(mainGfxList->Close());
	ID3D12CommandList* cmd[] = { mainGfxList.Get() };
	ExecuteCommandList(1, cmd);
}

void GraphicManager::ExecuteCommandList(int _listCount, ID3D12CommandList** _cmdList)
{
	mainGraphicQueue->ExecuteCommandLists(_listCount, _cmdList);
}

void GraphicManager::RenderThread()
{
	while (true)
	{
		// wait for main thread notify to draw
		WaitForSingleObject(beginRenderThread, INFINITE);

		DrawCamera();

		// mark fence value
		graphicFences[currFrameIndex] = ++mainFence;
		mainGraphicQueue->Signal(mainGraphicFence.Get(), mainFence);

		SetEvent(renderThreadFinish);
	}
}

void GraphicManager::WaitForRenderThread()
{
	WaitForSingleObject(renderThreadFinish, INFINITE);
}

ID3D12Device * GraphicManager::GetDevice()
{
	return mainDevice;
}

ID3D12Device5* GraphicManager::GetDxrDevice()
{
	return rayTracingDevice.Get();
}

ID3D12GraphicsCommandList5* GraphicManager::GetDxrList()
{
	return rayTracingCmd.Get();
}

ID3D12QueryHeap * GraphicManager::GetGpuTimeQuery()
{
	return gpuTimeQuery.Get();
}

UINT GraphicManager::GetRtvDesciptorSize()
{
	return rtvDescriptorSize;
}

UINT GraphicManager::GetDsvDesciptorSize()
{
	return dsvDescriptorSize;
}

UINT GraphicManager::GetCbvSrvUavDesciptorSize()
{
	return cbvSrvUavDescriptorSize;
}

FrameResource* GraphicManager::GetFrameResource()
{
	frameResource.mainGfxAllocator = mainGfxAllocator[currFrameIndex].Get();
	frameResource.mainGfxList = mainGfxList.Get();

	for (int i = 0; i < numOfLogicalCores - 1; i++)
	{
		frameResource.workerGfxAlloc[i] = workerGfxAllocator[i][currFrameIndex].Get();
		frameResource.workerGfxList[i] = workerGfxList[i].Get();
	}
	frameResource.currFrameIndex = currFrameIndex;

	return &frameResource;
}

UINT64 GraphicManager::GetGpuFreq()
{
	return gpuFreq;
}

void GraphicManager::WaitBeginWorkerThread(int _index)
{
	WaitForSingleObject(beginWorkerThread[_index], INFINITE);
}

void GraphicManager::SetBeginWorkerThreadEvent()
{
	for (int i = 0; i < numOfLogicalCores - 1; i++)
	{
		SetEvent(beginWorkerThread[i]);
	}
}

void GraphicManager::ResetWorkerThreadFinish()
{
	for (int i = 0; i < numOfLogicalCores - 1; i++)
	{
		ResetEvent(workerThreadFinish[i]);
	}
}

void GraphicManager::WaitForWorkerThread()
{
	WaitForMultipleObjects(numOfLogicalCores - 1, workerThreadFinish, TRUE, INFINITE);
}

void GraphicManager::SetWorkerThreadFinishEvent(int _index)
{
	SetEvent(workerThreadFinish[_index]);
}

void GraphicManager::UploadSystemConstant(SystemConstant _sc, int _frameIdx)
{
	systemConstantCPU = _sc;
	systemConstantGPU[_frameIdx]->CopyData(0, systemConstantCPU);
}

SystemConstant GraphicManager::GetSystemConstantCPU()
{
	return systemConstantCPU;
}

D3D12_GPU_VIRTUAL_ADDRESS GraphicManager::GetSystemConstantGPU(int _frameIdx)
{
	return systemConstantGPU[_frameIdx]->Resource()->GetGPUVirtualAddress();
}