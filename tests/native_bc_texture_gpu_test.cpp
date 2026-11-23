#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <render_backend/gpu/texture_load.hpp>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace render_backend::gpu;

namespace
{
struct Case
{
	const char *name;
	DXGI_FORMAT dxgi;
	TextureFormat expected;
	nvrhi::Format native;
	bool cube;
	bool explicitView;
};

class Messages final : public nvrhi::IMessageCallback
{
  public:
	void message(nvrhi::MessageSeverity severity, const char *text) override
	{
		std::fprintf(stderr, "NVRHI %d: %s\n", static_cast<int>(severity), text);
		if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
			failed = true;
	}
	bool failed = false;
};

void write32(std::vector<uint8_t> &file, size_t at, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		file[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

std::vector<uint8_t> makeDds(const Case &test)
{
	constexpr size_t faceBytes = 64 + 16 + 16 + 16;
	const size_t faceCount = test.cube ? 6 : 1;
	std::vector<uint8_t> file(C_DDS_DX10_HEADER_SIZE + faceCount * faceBytes);
	write32(file, 0, 0x20534444); // DDS magic
	write32(file, 4, 124);
	write32(file, 12, 8);
	write32(file, 16, 8);
	write32(file, 28, 4);
	write32(file, 76, 32);
	write32(file, 80, 4);
	write32(file, 84, 0x30315844); // DX10 extension
	write32(file, 128, test.dxgi);
	write32(file, 132, 3); // Texture2D
	write32(file, 136, test.cube ? 4 : 0);
	write32(file, 140, 1);
	for (size_t i = 0; i < file.size() - C_DDS_DX10_HEADER_SIZE; ++i)
		file[C_DDS_DX10_HEADER_SIZE + i] = static_cast<uint8_t>((249 + i * 37 + i / faceBytes * 19) & 0xff);
	return file;
}

bool hardwareDevice(ComPtr<ID3D12Device> &device, DXGI_ADAPTER_DESC1 &info)
{
	ComPtr<IDXGIFactory4> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))))
		return false;
	for (UINT i = 0;; ++i)
	{
		ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapters1(i, adapter.GetAddressOf())))
			return false;
		if (FAILED(adapter->GetDesc1(&info)) || (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
			continue;
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.GetAddressOf()))))
			return true;
	}
}

bool fail(const Case &test, const char *reason)
{
	std::fprintf(stderr, "%s: %s\n", test.name, reason);
	return false;
}

bool submit(nvrhi::IDevice &device, ID3D12Device &native, nvrhi::ICommandList *commands, const Case &test,
			const char *stage)
{
	commands->close();
	const bool completed = device.executeCommandList(commands) != 0 && device.waitForIdle();
	const HRESULT reason = native.GetDeviceRemovedReason();
	if (!completed || FAILED(reason))
	{
		std::fprintf(stderr, "%s: %s submission failed, removed reason 0x%08lx\n", test.name, stage, reason);
		return false;
	}
	return true;
}

bool exercise(nvrhi::IDevice &device, ID3D12Device &nativeDevice, const Case &test, bool uploadOnly)
{
	const auto file = makeDds(test);
	DdsTextureView expected;
	if (readDdsTexture(file.data(), file.size(), expected) != DdsReadError::None)
		return fail(test, "test fixture could not be parsed");
	nvrhi::CommandListHandle commands = device.createCommandList();
	if (!commands)
		return fail(test, "cannot create a graphics command list");
	commands->open();
	const TextureLoadResult loaded = test.explicitView
		? uploadDdsTexture(device, *commands, expected, test.name)
		: loadTexture(device, *commands, file.data(), file.size(), TextureColorSpace::Linear, test.name);
	if (!loaded.texture || loaded.error != TextureLoadError::None)
	{
		commands->close();
		std::fprintf(stderr, "%s: load error %u, DDS error %u\n", test.name,
					 static_cast<unsigned>(loaded.error), static_cast<unsigned>(loaded.ddsError));
		return false;
	}
	const auto &actual = loaded.texture->getDesc();
	if (loaded.description.format != test.expected || loaded.description.cube != test.cube ||
		actual.width != 8 || actual.height != 8 || actual.mipLevels != 4 || actual.arraySize != expected.faceCount() ||
		actual.format != test.native ||
		actual.dimension != (test.cube ? nvrhi::TextureDimension::TextureCube : nvrhi::TextureDimension::Texture2D))
	{
		commands->close();
		return fail(test, "native texture metadata mismatch");
	}
	const auto native = loaded.texture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
	if (!static_cast<ID3D12Resource *>(native.pointer))
	{
		commands->close();
		return fail(test, "no native D3D12 resource");
	}
	if (!submit(device, nativeDevice, commands, test, "upload"))
		return false;
	if (uploadOnly)
	{
		std::printf("%s: %u faces x %u mip levels, upload completed\n", test.name, actual.arraySize, actual.mipLevels);
		return true;
	}
	// NVRHI staging reads use an explicit source box, which violates the D3D12 block-alignment
	// requirement at the 2x2 and 1x1 mips. Copy whole subresources into native footprints instead.
	auto *texture = static_cast<ID3D12Resource *>(native.pointer);
	const D3D12_RESOURCE_DESC nativeDesc = texture->GetDesc();
	const uint32_t subresources = actual.arraySize * actual.mipLevels;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
	UINT64 total = 0;
	nativeDevice.GetCopyableFootprints(&nativeDesc, 0, subresources, 0, footprints.data(), nullptr, nullptr, &total);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	heap.CreationNodeMask = heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC buffer{};
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = total;
	buffer.Height = 1;
	buffer.DepthOrArraySize = buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> readback;
	if (FAILED(nativeDevice.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
													 nullptr, IID_PPV_ARGS(readback.GetAddressOf()))))
		return fail(test, "cannot create a native readback buffer");
	commands = device.createCommandList();
	if (!commands)
		return fail(test, "cannot create the readback graphics command list");
	commands->open();
	const auto commandObject = commands->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
	auto *copy = static_cast<ID3D12GraphicsCommandList *>(commandObject.pointer);
	if (copy == nullptr)
	{
		commands->close();
		return fail(test, "cannot retrieve the D3D12 copy command list");
	}
	commands->setTextureState(loaded.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
	commands->commitBarriers();
	for (uint32_t i = 0; i < subresources; ++i)
	{
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = readback.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = footprints[i];
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = texture;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = i;
		copy->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	if (!submit(device, nativeDevice, commands, test, "readback"))
		return false;
	uint8_t *gpuBytes = nullptr;
	const D3D12_RANGE readRange{0, static_cast<SIZE_T>(total)};
	if (FAILED(readback->Map(0, &readRange, reinterpret_cast<void **>(&gpuBytes))))
		return fail(test, "cannot map readback data");
	bool matches = true;
	for (uint32_t face = 0; matches && face < actual.arraySize; ++face)
		for (uint32_t mip = 0; matches && mip < actual.mipLevels; ++mip)
		{
			const auto level = expected.level(face, mip);
			const auto &footprint = footprints[face * actual.mipLevels + mip];
			const auto pitch = footprint.Footprint.RowPitch;
			matches = pitch >= level.rowPitch;
			for (size_t row = 0; matches && row < level.size / level.rowPitch; ++row)
				matches = std::memcmp(gpuBytes + footprint.Offset + row * pitch, level.data + row * level.rowPitch,
									  level.rowPitch) == 0;
		}
	const D3D12_RANGE writtenRange{0, 0};
	readback->Unmap(0, &writtenRange);
	if (!matches)
		return fail(test, "GPU readback differs from the original compressed blocks");
	std::printf("%s: %u faces x %u mip levels, native metadata and compressed GPU readback match\n",
				test.name, actual.arraySize, actual.mipLevels);
	return true;
}

bool reportDebugErrors(ID3D12Device &device)
{
	ComPtr<ID3D12InfoQueue> queue;
	if (FAILED(device.QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()))))
		return false;
	bool errors = false;
	for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
	{
		SIZE_T size = 0;
		if (FAILED(queue->GetMessage(i, nullptr, &size)) || size == 0)
			continue;
		std::vector<uint8_t> storage(size);
		auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
		if (SUCCEEDED(queue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
		{
			std::fprintf(stderr, "D3D12 %d (%d): %s\n", message->Severity, message->ID, message->pDescription);
			errors |= message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
		}
	}
	return errors;
}
} // namespace

int main(int argc, char **argv)
{
	bool uploadOnly = false;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--upload-only") == 0)
			uploadOnly = true;
		else if (std::strcmp(argv[i], "--debug") == 0)
		{
			ComPtr<ID3D12Debug> debug;
			if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()))))
			{
				// --debug asks for a validated run; skip rather than pass without validation.
				std::fputs("D3D12 debug layer unavailable (install the Graphics Tools optional feature)\n", stderr);
				return 77;
			}
			debug->EnableDebugLayer();
		}
		else
			return 2;
	}
	constexpr Case cases[] = {
		{"BC6H UF16", DXGI_FORMAT_BC6H_UF16, TextureFormat::BC6HUfloat, nvrhi::Format::BC6H_UFLOAT, false, false},
		{"BC6H SF16", DXGI_FORMAT_BC6H_SF16, TextureFormat::BC6HSfloat, nvrhi::Format::BC6H_SFLOAT, false, true},
		{"BC7 UNORM", DXGI_FORMAT_BC7_UNORM, TextureFormat::BC7Unorm, nvrhi::Format::BC7_UNORM, false, true},
		{"BC7 sRGB cube", DXGI_FORMAT_BC7_UNORM_SRGB, TextureFormat::BC7Srgb, nvrhi::Format::BC7_UNORM_SRGB, true, false},
	};
	ComPtr<ID3D12Device> native;
	DXGI_ADAPTER_DESC1 info{};
	if (!hardwareDevice(native, info))
	{
		std::fputs("No hardware D3D12 feature-level 12.0 adapter\n", stderr);
		return 77;
	}
	std::printf("D3D12 hardware adapter: %ls\n", info.Description);
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ComPtr<ID3D12CommandQueue> queue;
	if (FAILED(native->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.GetAddressOf()))))
		return 1;
	Messages messages;
	nvrhi::d3d12::DeviceDesc desc;
	desc.pDevice = native.Get();
	desc.pGraphicsCommandQueue = queue.Get();
	desc.errorCB = &messages;
	auto device = nvrhi::d3d12::createDevice(desc);
	if (!device)
		return 1;
	bool ok = true;
	for (const auto &test : cases)
	{
		ok &= exercise(*device, *native.Get(), test, uploadOnly);
		if (FAILED(native->GetDeviceRemovedReason()))
			break;
	}
	const bool nativeErrors = reportDebugErrors(*native.Get());
	device->runGarbageCollection();
	return ok && !messages.failed && !nativeErrors ? 0 : 1;
}
