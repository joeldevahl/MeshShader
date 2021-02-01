#include "Render.h"

#include <dxgi1_6.h>
#include <d3d12.h>

#include <directxmath.h> // for XMFLOAT4x4
using namespace DirectX;

#include "d3dx12.h"

#include <wrl.h> // for ComPtr
using namespace Microsoft::WRL;

#include <vector>

_declspec(align(256u)) struct SceneConstantBuffer
{
    XMFLOAT4X4 MVP;
    UINT Counts[4];
    UINT NumMeshes;
    UINT NumMaterials;
};

struct Render
{
    UINT width = 1280;
    UINT height = 720;
    UINT frameCount = 3;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device6> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain4> swapChain;
    UINT frameIndex;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> uniHeap;
    UINT rtvDescriptorSize;
    UINT dsvDescriptorSize;
    UINT uniDescriptorSize;

    std::vector<ComPtr<ID3D12Resource>> renderTargets;
    std::vector<ComPtr<ID3D12CommandAllocator>> commandAllocators;

    ComPtr<ID3D12Resource> depthStencil;

    SceneConstantBuffer constantBufferData;
    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12Resource> meshesBuffer;
    ComPtr<ID3D12Resource> materialsBuffer;
    char* cbvDataBegin;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;

    ComPtr<ID3D12GraphicsCommandList6> commandList;

    ComPtr<ID3D12Fence1> fence;
    std::vector<UINT> fenceValues;
    HANDLE fenceEvent;

    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;
};

static void ThrowIfFailed(HRESULT hr)
{
    _ASSERT(SUCCEEDED(hr));
}

static void GetHardwareAdapter(IDXGIFactory6* factory, IDXGIAdapter4** ppAdapter)
{
    *ppAdapter = nullptr;

    ComPtr<IDXGIAdapter4> adapter;

	for(UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)); ++adapterIndex)
	{
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			continue;

		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device), nullptr)))
			break;
	}

    *ppAdapter = adapter.Detach();
}

static HRESULT ReadDataFromFile(LPCWSTR filename, byte** data, UINT* size)
{
    CREATEFILE2_EXTENDED_PARAMETERS extendedParams = {};
    extendedParams.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS);
    extendedParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    extendedParams.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
    extendedParams.dwSecurityQosFlags = SECURITY_ANONYMOUS;
    extendedParams.lpSecurityAttributes = nullptr;
    extendedParams.hTemplateFile = nullptr;

    Wrappers::FileHandle file(CreateFile2(filename, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams));
    if (file.Get() == INVALID_HANDLE_VALUE)
    {
        throw std::exception();
    }

    FILE_STANDARD_INFO fileInfo = {};
    if (!GetFileInformationByHandleEx(file.Get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)))
    {
        throw std::exception();
    }

    if (fileInfo.EndOfFile.HighPart != 0)
    {
        throw std::exception();
    }

    *data = reinterpret_cast<byte*>(malloc(fileInfo.EndOfFile.LowPart));
    *size = fileInfo.EndOfFile.LowPart;

    if (!ReadFile(file.Get(), *data, fileInfo.EndOfFile.LowPart, nullptr, nullptr))
    {
        throw std::exception();
    }

    return S_OK;
}

static ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(Render* render, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = count;
	heapDesc.Type = type;
	heapDesc.Flags = flags;

    ComPtr<ID3D12DescriptorHeap> heap;
	ThrowIfFailed(render->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));

    return heap;
}

Render* CreateRender(UINT width, UINT height, UINT frameCount)
{
    Render* render = new Render;

    render->width = width;
    render->height = height;
    render->frameCount = frameCount;

    render->frameIndex = 0;

    render->rtvDescriptorSize = 0;
    render->dsvDescriptorSize = 0;

    render->renderTargets.resize(frameCount);
    render->commandAllocators.resize(frameCount);

    render->fenceValues.resize(frameCount, 0);

    return render;
}

void Destroy(Render* render)
{
	delete render;
}

void Initialize(Render* render, HWND hwnd)
{
    UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };

    ThrowIfFailed(D3D12EnableExperimentalFeatures(1, experimentalFeatures, nullptr, nullptr));

    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
        if (SUCCEEDED(hr))
        {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    {
        ComPtr<IDXGIFactory4> initialFactory;
        ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&initialFactory)));

        ThrowIfFailed(initialFactory->QueryInterface(IID_PPV_ARGS(&render->factory)));
    }

    bool useWarpDevice = false;
    if (useWarpDevice)
    {
        ComPtr<IDXGIAdapter4> warpAdapter;
        ThrowIfFailed(render->factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&render->device)));
    }
    else
    {
        ComPtr<IDXGIAdapter4> hardwareAdapter;
        GetHardwareAdapter(render->factory.Get(), &hardwareAdapter);
        ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&render->device)));
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(render->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&render->commandQueue)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = render->frameCount;
    swapChainDesc.Width = render->width;
    swapChainDesc.Height = render->height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(render->factory->CreateSwapChainForHwnd(
        render->commandQueue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    ));
    ThrowIfFailed(swapChain1.As(&render->swapChain));

    ThrowIfFailed(render->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    render->frameIndex = render->swapChain->GetCurrentBackBufferIndex();

	render->rtvHeap = CreateDescriptorHeap(render, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, render->frameCount);
	render->dsvHeap = CreateDescriptorHeap(render, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
	render->uniHeap = CreateDescriptorHeap(render, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000);
	render->rtvDescriptorSize = render->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	render->dsvDescriptorSize = render->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	render->uniDescriptorSize = render->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(render->rtvHeap->GetCPUDescriptorHandleForHeapStart());

        for (UINT n = 0; n < render->frameCount; n++)
        {
            ThrowIfFailed(render->swapChain->GetBuffer(n, IID_PPV_ARGS(&render->renderTargets[n])));
            render->device->CreateRenderTargetView(render->renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, render->rtvDescriptorSize);
        
            ThrowIfFailed(render->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render->commandAllocators[n])));
        }
    }

    {
        D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
        depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;

        D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
        depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
        depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
        depthOptimizedClearValue.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES heapType(D3D12_HEAP_TYPE_DEFAULT);

        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, render->width, render->height, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        ThrowIfFailed(render->device->CreateCommittedResource(
            &heapType,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthOptimizedClearValue,
            IID_PPV_ARGS(&render->depthStencil)
        ));

        render->device->CreateDepthStencilView(render->depthStencil.Get(), &depthStencilDesc, render->dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    {
        const UINT64 constantBufferSize = sizeof(SceneConstantBuffer) * render->frameCount;

        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
        ThrowIfFailed(render->device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&render->constantBuffer)));

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = render->constantBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = constantBufferSize;

        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(render->constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&render->cbvDataBegin)));
    }

    {
        CD3DX12_ROOT_PARAMETER1 rootParameters[1];
        rootParameters[0].InitAsConstantBufferView(0);

        auto desc = CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC(1, rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> rootBlob;
        ComPtr<ID3DBlob> errorBlob;
        ThrowIfFailed(D3D12SerializeVersionedRootSignature(&desc, &rootBlob, &errorBlob));

        ThrowIfFailed(render->device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&render->rootSignature)));
    }

    {
        struct
        {
            byte* data;
            uint32_t size;
        } amplificationShader, meshShader, pixelShader;

        ReadDataFromFile(L"x64/Debug/MeshletAS.cso", &amplificationShader.data, &amplificationShader.size);
        ReadDataFromFile(L"x64/Debug/MeshletMS.cso", &meshShader.data, &meshShader.size);
        ReadDataFromFile(L"x64/Debug/MeshletPS.cso", &pixelShader.data, &pixelShader.size);

        D3DX12_MESH_SHADER_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = render->rootSignature.Get();
        psoDesc.AS = { amplificationShader.data, amplificationShader.size };
        psoDesc.MS = { meshShader.data, meshShader.size };
        psoDesc.PS = { pixelShader.data, pixelShader.size };
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = render->renderTargets[0]->GetDesc().Format;
        psoDesc.DSVFormat = render->depthStencil->GetDesc().Format;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.SampleDesc = DefaultSampleDesc();

        auto psoStream = CD3DX12_PIPELINE_MESH_STATE_STREAM(psoDesc);

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc;
        streamDesc.pPipelineStateSubobjectStream = &psoStream;
        streamDesc.SizeInBytes = sizeof(psoStream);

        ThrowIfFailed(render->device->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&render->pipelineState)));
    }

    ThrowIfFailed(render->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, render->commandAllocators[render->frameIndex].Get(), render->pipelineState.Get(), IID_PPV_ARGS(&render->commandList)));
    ThrowIfFailed(render->commandList->Close());

    // TODO: upload mesh data here

    {
        ThrowIfFailed(render->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render->fence)));
        render->fenceValues[render->frameIndex]++;

        render->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (render->fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    render->viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)render->width, (float)render->height);
    render->scissorRect = CD3DX12_RECT(0, 0, render->width, render->height);
}

void Draw(Render* render)
{
    ThrowIfFailed(render->commandAllocators[render->frameIndex]->Reset());

    ThrowIfFailed(render->commandList->Reset(render->commandAllocators[render->frameIndex].Get(), render->pipelineState.Get()));

    XMMATRIX proj = XMMatrixPerspectiveFovRH(XM_PI / 3.0f, (float)render->width / (float)render->height, 1.0f, 1000.0f);
    XMStoreFloat4x4(&render->constantBufferData.MVP, XMMatrixTranspose(proj));
    render->constantBufferData.Counts[0] = 230;
    render->constantBufferData.Counts[1] = 130;
    render->constantBufferData.Counts[2] = 0;
    render->constantBufferData.Counts[3] = 0;
    render->constantBufferData.NumMeshes = 256;
    render->constantBufferData.NumMaterials = 1024;
    memcpy(render->cbvDataBegin + sizeof(SceneConstantBuffer) * render->frameIndex, &render->constantBufferData, sizeof(render->constantBufferData));

    render->commandList->SetGraphicsRootSignature(render->rootSignature.Get());
    render->commandList->RSSetViewports(1, &render->viewport);
    render->commandList->RSSetScissorRects(1, &render->scissorRect);

    CD3DX12_RESOURCE_BARRIER rtBarrier = CD3DX12_RESOURCE_BARRIER::Transition(render->renderTargets[render->frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    render->commandList->ResourceBarrier(1, &rtBarrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(render->rtvHeap->GetCPUDescriptorHandleForHeapStart(), render->frameIndex, render->rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(render->dsvHeap->GetCPUDescriptorHandleForHeapStart());
    render->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    render->commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    render->commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    render->commandList->SetGraphicsRootConstantBufferView(0, render->constantBuffer->GetGPUVirtualAddress() + sizeof(SceneConstantBuffer) * render->frameIndex);

	//render->commandList->SetGraphicsRoot32BitConstant(1, sizeof(UINT32), 0);
	//render->commandList->SetGraphicsRootShaderResourceView(2, vertexBuffer->GetGPUVirtualAddress());
	//render->commandList->SetGraphicsRootShaderResourceView(3, meshletBuffer->GetGPUVirtualAddress());
	//render->commandList->SetGraphicsRootShaderResourceView(4, uniqueVertexIndices->GetGPUVirtualAddress());
	//render->commandList->SetGraphicsRootShaderResourceView(5, primitiveIndices->GetGPUVirtualAddress());

	//render->commandList->SetGraphicsRoot32BitConstant(1, 0, 1); // 0 offset
	render->commandList->DispatchMesh(render->constantBufferData.Counts[0], render->constantBufferData.Counts[1], 1);

    auto presentBarrier = CD3DX12_RESOURCE_BARRIER::Transition(render->renderTargets[render->frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    render->commandList->ResourceBarrier(1, &presentBarrier);

    ThrowIfFailed(render->commandList->Close());

    ID3D12CommandList* ppCommandLists[] = { render->commandList.Get() };
    render->commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    ThrowIfFailed(render->swapChain->Present(1, 0));

    const UINT64 currentFenceValue = render->fenceValues[render->frameIndex];
    ThrowIfFailed(render->commandQueue->Signal(render->fence.Get(), currentFenceValue));

    render->frameIndex = render->swapChain->GetCurrentBackBufferIndex();

    if (render->fence->GetCompletedValue() < render->fenceValues[render->frameIndex])
    {
        // TODO: do this wait on frame begin instead of frame end
        ThrowIfFailed(render->fence->SetEventOnCompletion(render->fenceValues[render->frameIndex], render->fenceEvent));
        WaitForSingleObjectEx(render->fenceEvent, INFINITE, FALSE);
    }

    render->fenceValues[render->frameIndex] = currentFenceValue + 1;
}
