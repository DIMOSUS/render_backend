// Native DX12 queue and swap-chain operations shared by renderer hosts.
#pragma once
#include <nvrhi/d3d12.h>
#include <dxgi1_6.h>

namespace render_backend::gpu::dx12
{
HRESULT createDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, ID3D12Device **out);
HRESULT createDirectQueue(ID3D12Device *device, ID3D12CommandQueue **out);

// Flip-discard BGRA8 swap chain. The host chooses the client size and the window's presentation policy.
HRESULT createFlipSwapChain(HWND window, ID3D12CommandQueue *queue, uint32_t width, uint32_t height,
							uint32_t bufferCount, bool &allowTearing, IDXGISwapChain3 **out);
} // namespace render_backend::gpu::dx12
