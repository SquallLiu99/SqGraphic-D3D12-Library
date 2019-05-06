#include "GraphicManager.h"
#include "stdafx.h"
#include "CameraManager.h"
#include "ForwardRenderingPath.h"
#include "d3dx12.h"

bool GraphicManager::Initialize(ID3D12Device* _device, int _numOfThreads)
{
	initSucceed = false;
	numOfLogicalCores = 0;

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
    
	return initSucceed;
}

void GraphicManager::Release()
{
	if (mainGraphicFence)
	{
		const UINT64 fence = mainFenceValue;
		const UINT64 lastCompletedFence = mainGraphicFence->GetCompletedValue();

		// Signal and increment the fence value.
		mainGraphicQueue->Signal(mainGraphicFence.Get(), mainFenceValue);
		mainFenceValue++;

		// Wait until gpu is finished.
		if (lastCompletedFence < fence)
		{
			mainGraphicFence->SetEventOnCompletion(fence, mainFenceEvent);
			WaitForSingleObject(mainFenceEvent, INFINITE);
		}
		mainGraphicFence.Reset();
	}

	CloseHandle(beginRenderThread);
	CloseHandle(renderThreadHandles);

	for (int i = 0; i < FrameCount; i++)
	{
		if (mainGraphicAllocator[i])
		{
			mainGraphicAllocator[i].Reset();
		}

		if (mainGraphicList[i])
		{
			mainGraphicList[i].Reset();
		}
	}


	if (mainGraphicQueue)
	{
		mainGraphicQueue.Reset();
	}

	if (gpuTimeQuery)
	{
		gpuTimeQuery.Reset();
	}

	if (gpuTimeResult)
	{
		gpuTimeResult.Reset();
	}

	mainDevice = nullptr;
}

int GraphicManager::GetThreadCount()
{
	return numOfLogicalCores;
}

void GraphicManager::Update()
{
#if defined(GRAPHICTIME)
	TIMER_INIT
	TIMER_START
#endif
	
	const UINT64 lastCompletedFence = mainGraphicFence->GetCompletedValue();
	currFrameIndex = (currFrameIndex + 1) % FrameCount;

	// Make sure GPU isn't busy.
	// If it is, wait for it to complete.
	if (graphicFences[currFrameIndex] > lastCompletedFence)
	{
		HANDLE eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		mainGraphicFence->SetEventOnCompletion(graphicFences[currFrameIndex], eventHandle);
		WaitForSingleObject(eventHandle, INFINITE);
	}

#if defined(GRAPHICTIME)
	TIMER_STOP
	gameTime.updateTime = elapsedTime;
#endif
}

void GraphicManager::Render()
{
#if defined(GRAPHICTIME)
	TIMER_INIT
	TIMER_START
	gameTime.gpuTime = 0.0f;
#endif

	// wake up render thread
	SetEvent(beginRenderThread);

#if defined(GRAPHICTIME)
	TIMER_STOP
	gameTime.renderTime = elapsedTime;
#endif
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
	
	LogIfFailed(mainDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK)
		, D3D12_HEAP_FLAG_NONE
		, &CD3DX12_RESOURCE_DESC::Buffer(2 * sizeof(uint64_t))
		, D3D12_RESOURCE_STATE_COPY_DEST
		, nullptr
		, IID_PPV_ARGS(&gpuTimeResult)), hr);
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

	for (int i = 0; i < FrameCount; i++)
	{
		LogIfFailed(mainDevice->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(mainGraphicAllocator[i].GetAddressOf())), hr);

		LogIfFailed(mainDevice->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			mainGraphicAllocator[i].Get(),
			nullptr,
			IID_PPV_ARGS(mainGraphicList[i].GetAddressOf())), hr);

		// close first because we will call reset at the beginning of render work
		LogIfFailed(mainGraphicList[i]->Close(), hr);
	}

	return hr;
}

HRESULT GraphicManager::CreateGraphicFences()
{
	HRESULT hr = S_OK;

	mainFenceValue = 0;
	for (int i = 0; i < FrameCount; i++)
	{
		graphicFences[i] = 0;
	}

	LogIfFailed(mainDevice->CreateFence(mainFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mainGraphicFence)), hr);
	mainFenceValue++;

	mainFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (mainFenceEvent == nullptr)
	{
		hr = S_FALSE;
	}

	// Signal and increment the fence value.
	UINT64 fenceToWaitFor = mainFenceValue;
	LogIfFailed(mainGraphicQueue->Signal(mainGraphicFence.Get(), mainFenceValue), hr);
	mainFenceValue++;

	// Wait until the fence is completed.
	LogIfFailed(mainGraphicFence->SetEventOnCompletion(fenceToWaitFor, mainFenceEvent), hr);
	WaitForSingleObject(mainFenceEvent, INFINITE);

	return hr;
}

bool GraphicManager::CreateGraphicThreads()
{
	bool result = true;

	// setup thread parameter
	struct ThreadParameter
	{
		int threadIndex;
	};

	struct threadwrapper
	{
		static unsigned int WINAPI thunk(LPVOID lpParameter)
		{
			ThreadParameter* parameter = reinterpret_cast<ThreadParameter*>(lpParameter);
			GraphicManager::Instance().RenderThread();
			return 0;
		}
	};

	beginRenderThread = CreateEvent(
		NULL,
		FALSE,
		FALSE,
		NULL);

	renderThreadHandles = reinterpret_cast<HANDLE>(_beginthreadex(
		nullptr,
		0,
		threadwrapper::thunk,
		nullptr,
		0,
		nullptr));

	result = (beginRenderThread != nullptr) && (renderThreadHandles != nullptr);

	if (!result)
	{
		LogMessage(L"[SqGraphic Error] SqGraphic: Create Render Thread Failed.");
	}

	return result;
}

void GraphicManager::RenderThread()
{
	while (true)
	{
		// wait for main thread notify to draw
		WaitForSingleObject(beginRenderThread, INFINITE);

		// process render thread
#if defined(GRAPHICTIME)
		TIMER_INIT
		TIMER_START
		gameTime.renderThreadTime = 0.0f;
#endif

		vector<Camera> cams = CameraManager::Instance().GetCameras();
		for (size_t i = 0; i < cams.size(); i++)
		{
			// forward render path
			if (cams[i].GetCameraData().renderingPath == RenderingPathType::Forward)
			{
				gameTime.gpuTime += ForwardRenderingPath::Instance().RenderLoop(cams[i]);
			}
		}

		// Signal and increment the fence value.
		graphicFences[currFrameIndex] = mainFenceValue;
		mainGraphicQueue->Signal(mainGraphicFence.Get(), mainFenceValue);
		mainFenceValue++;

#if defined(GRAPHICTIME)
		TIMER_STOP
		gameTime.renderThreadTime = elapsedTime;
#endif
	}
}

ID3D12Device * GraphicManager::GetDevice()
{
	return mainDevice;
}

ID3D12QueryHeap * GraphicManager::GetGpuTimeQuery()
{
	return gpuTimeQuery.Get();
}

ID3D12Resource * GraphicManager::GetGpuTimeResult()
{
	return gpuTimeResult.Get();
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

FrameResource GraphicManager::GetFrameResource()
{
	FrameResource fr;

	fr.beginRenderThread = beginRenderThread;
	fr.renderThreadHandles = renderThreadHandles;
	fr.mainGraphicList = mainGraphicList[currFrameIndex].Get();
	fr.mainGraphicAllocator = mainGraphicAllocator[currFrameIndex].Get();
	fr.numOfLogicalCores = numOfLogicalCores;
	fr.mainGraphicQueue = mainGraphicQueue.Get();

	return fr;
}

GameTime GraphicManager::GetGameTime()
{
	return gameTime;
}

UINT64 GraphicManager::GetGpuFreq()
{
	return gpuFreq;
}
