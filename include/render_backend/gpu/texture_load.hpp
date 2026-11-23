// GPU upload of DDS, JPEG, PNG, TGA and BMP from memory.
#pragma once
#include <nvrhi/nvrhi.h>
#include <render_backend/gpu/dds_texture.hpp>

namespace render_backend::gpu
{
enum class TextureColorSpace : uint8_t
{
	Linear,
	Srgb,
};

enum class TextureLoadError : uint8_t
{
	None,
	InvalidData,
	InvalidDds,
	UnsupportedDimensions,
	UnsupportedByDevice,
	AllocationFailed,
};

struct TextureLoadResult
{
	nvrhi::TextureHandle texture;
	ImageDesc			 description;
	TextureLoadError	 error = TextureLoadError::InvalidData;
	DdsReadError		 ddsError = DdsReadError::None;
};

// Records the upload in an open graphics command list with automatic barriers. NVRHI copies the CPU
// data into its upload storage during the call. The caller submits the list before sampling the texture.
TextureLoadResult uploadDdsTexture(nvrhi::IDevice &device, nvrhi::ICommandList &commands, const DdsTextureView &dds,
								   const char *debugName = nullptr);

// DDS uses its encoded color space and mip chain. Other files decode to one RGBA8 level using `colorSpace`.
TextureLoadResult loadTexture(nvrhi::IDevice &device, nvrhi::ICommandList &commands, const void *data, size_t size,
							  TextureColorSpace colorSpace = TextureColorSpace::Linear,
							  const char	   *debugName = nullptr);
} // namespace render_backend::gpu
