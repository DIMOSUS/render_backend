// Maps TextureFormat to NVRHI and DXGI formats, with tightly packed row and image sizes for uploads.
#pragma once
#include <render_backend/gpu/texture_format.hpp>
#include <dxgiformat.h>
#include <nvrhi/nvrhi.h>

namespace render_backend::gpu
{
struct GpuFormat
{
	nvrhi::Format format = nvrhi::Format::UNKNOWN;
	DXGI_FORMAT	  dxgi = DXGI_FORMAT_UNKNOWN;
	bool		  block = false;  // 4x4 block-compressed.
	uint32_t	  blockBytes = 0; // Bytes per texel, or per 4x4 block when `block` is set.

	size_t rowPitch(uint32_t width) const
	{
		return (block ? (size_t(width) + 3) / 4 : width) * blockBytes;
	}
	size_t byteSize(uint32_t width, uint32_t height) const
	{
		return rowPitch(width) * (block ? (size_t(height) + 3) / 4 : height);
	}
};
inline GpuFormat gpuFormatOf(TextureFormat format)
{
	switch (format)
	{
	case TextureFormat::BGRA8:
		return {nvrhi::Format::BGRA8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, false, 4};
	case TextureFormat::RGBA16Unorm:
		return {nvrhi::Format::RGBA16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, false, 8};
	case TextureFormat::RGB10A2Unorm:
		return {nvrhi::Format::R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, false, 4};
	case TextureFormat::RGBA16Float:
		return {nvrhi::Format::RGBA16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, false, 8};
	case TextureFormat::RGBA32Float:
		return {nvrhi::Format::RGBA32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, false, 16};
	case TextureFormat::RGBA8Snorm:
		return {nvrhi::Format::RGBA8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM, false, 4};
	case TextureFormat::RGBA16Snorm:
		return {nvrhi::Format::RGBA16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM, false, 8};
	case TextureFormat::BC1:
		return {nvrhi::Format::BC1_UNORM, DXGI_FORMAT_BC1_UNORM, true, 8};
	case TextureFormat::BC2:
		return {nvrhi::Format::BC2_UNORM, DXGI_FORMAT_BC2_UNORM, true, 16};
	case TextureFormat::BC3:
		return {nvrhi::Format::BC3_UNORM, DXGI_FORMAT_BC3_UNORM, true, 16};
	case TextureFormat::RGBA8Unorm:
		return {nvrhi::Format::RGBA8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, 4};
	case TextureFormat::RGBA8Srgb:
		return {nvrhi::Format::SRGBA8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, 4};
	case TextureFormat::BGRA8Srgb:
		return {nvrhi::Format::SBGRA8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false, 4};
	case TextureFormat::BC1Srgb:
		return {nvrhi::Format::BC1_UNORM_SRGB, DXGI_FORMAT_BC1_UNORM_SRGB, true, 8};
	case TextureFormat::BC2Srgb:
		return {nvrhi::Format::BC2_UNORM_SRGB, DXGI_FORMAT_BC2_UNORM_SRGB, true, 16};
	case TextureFormat::BC3Srgb:
		return {nvrhi::Format::BC3_UNORM_SRGB, DXGI_FORMAT_BC3_UNORM_SRGB, true, 16};
	case TextureFormat::BC4Unorm:
		return {nvrhi::Format::BC4_UNORM, DXGI_FORMAT_BC4_UNORM, true, 8};
	case TextureFormat::BC4Snorm:
		return {nvrhi::Format::BC4_SNORM, DXGI_FORMAT_BC4_SNORM, true, 8};
	case TextureFormat::BC5Unorm:
		return {nvrhi::Format::BC5_UNORM, DXGI_FORMAT_BC5_UNORM, true, 16};
	case TextureFormat::BC5Snorm:
		return {nvrhi::Format::BC5_SNORM, DXGI_FORMAT_BC5_SNORM, true, 16};
	case TextureFormat::BC6HUfloat:
		return {nvrhi::Format::BC6H_UFLOAT, DXGI_FORMAT_BC6H_UF16, true, 16};
	case TextureFormat::BC6HSfloat:
		return {nvrhi::Format::BC6H_SFLOAT, DXGI_FORMAT_BC6H_SF16, true, 16};
	case TextureFormat::BC7Unorm:
		return {nvrhi::Format::BC7_UNORM, DXGI_FORMAT_BC7_UNORM, true, 16};
	case TextureFormat::BC7Srgb:
		return {nvrhi::Format::BC7_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM_SRGB, true, 16};
	case TextureFormat::Unknown:
		return {};
	}
	return {};
}
} // namespace render_backend::gpu
