#include <render_backend/gpu/dx12_platform.hpp>

HRESULT render_backend::gpu::dx12::createDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, ID3D12Device **out)
{
	return D3D12CreateDevice(adapter, level, IID_PPV_ARGS(out));
}

HRESULT render_backend::gpu::dx12::createDirectQueue(ID3D12Device *device, ID3D12CommandQueue **out)
{
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	return device->CreateCommandQueue(&desc, IID_PPV_ARGS(out));
}

HRESULT render_backend::gpu::dx12::createFlipSwapChain(HWND window, ID3D12CommandQueue *queue, uint32_t width, uint32_t height,
										   uint32_t bufferCount, bool &allowTearing, IDXGISwapChain3 **out)
{
	IDXGIFactory5 *factory = nullptr;
	HRESULT		   hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
	if (FAILED(hr))
		return hr;
	BOOL tearing = FALSE;
	factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
	allowTearing = tearing != FALSE;
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = bufferCount;
	desc.Scaling = DXGI_SCALING_NONE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	IDXGISwapChain1 *swap = nullptr;
	hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swap);
	if (SUCCEEDED(hr))
	{
		factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		hr = swap->QueryInterface(IID_PPV_ARGS(out));
		swap->Release();
	}
	factory->Release();
	return hr;
}
