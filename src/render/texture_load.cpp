#include <render_backend/gpu/texture_load.hpp>
#include <render_backend/gpu/texture_format_nvrhi.hpp>
#include <limits.h>
#include <memory>

#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace render_backend::gpu
{
namespace
{
TextureLoadResult createSampledTexture(nvrhi::IDevice &device, const ImageDesc &image, const char *debugName)
{
	TextureLoadResult result;
	result.description = image;
	const GpuFormat native = gpuFormatOf(image.format);
	if (native.block && ((image.width | image.height) & 3) != 0)
	{
		result.error = TextureLoadError::UnsupportedDimensions;
		return result;
	}
	const auto required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderSample;
	if (native.format == nvrhi::Format::UNKNOWN || (device.queryFormatSupport(native.format) & required) != required)
	{
		result.error = TextureLoadError::UnsupportedByDevice;
		return result;
	}
	nvrhi::TextureDesc desc;
	desc.setWidth(image.width).setHeight(image.height).setMipLevels(image.levels).setFormat(native.format);
	desc.setDimension(image.cube ? nvrhi::TextureDimension::TextureCube : nvrhi::TextureDimension::Texture2D);
	desc.setArraySize(image.cube ? 6 : 1).enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
	if (debugName != nullptr)
		desc.setDebugName(debugName);
	result.texture = device.createTexture(desc);
	result.error = result.texture ? TextureLoadError::None : TextureLoadError::AllocationFailed;
	return result;
}
} // namespace

TextureLoadResult uploadDdsTexture(nvrhi::IDevice &device, nvrhi::ICommandList &commands, const DdsTextureView &dds,
								   const char *debugName)
{
	if (dds.faceCount() == 0)
		return {};
	TextureLoadResult result = createSampledTexture(device, dds.description(), debugName);
	if (!result.texture)
		return result;
	for (uint32_t face = 0; face < dds.faceCount(); ++face)
		for (uint32_t mip = 0; mip < result.description.levels; ++mip)
		{
			const DdsLevelView level = dds.level(face, mip);
			commands.writeTexture(result.texture, face, mip, level.data, level.rowPitch);
		}
	return result;
}

TextureLoadResult loadTexture(nvrhi::IDevice &device, nvrhi::ICommandList &commands, const void *data, size_t size,
							  TextureColorSpace colorSpace, const char *debugName)
{
	DdsTextureView	   dds;
	const DdsReadError parsed = readDdsTexture(data, size, dds);
	if (parsed == DdsReadError::None)
		return uploadDdsTexture(device, commands, dds, debugName);
	if (parsed != DdsReadError::NotDds)
	{
		TextureLoadResult result;
		result.error = TextureLoadError::InvalidDds;
		result.ddsError = parsed;
		return result;
	}
	if (data == nullptr || size == 0 || size > INT_MAX)
		return {};
	int														   width = 0, height = 0, channels = 0;
	const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
		stbi_load_from_memory(static_cast<const stbi_uc *>(data), static_cast<int>(size), &width, &height, &channels,
							  STBI_rgb_alpha),
		&stbi_image_free);
	if (!pixels)
		return {};
	const ImageDesc image{colorSpace == TextureColorSpace::Srgb ? TextureFormat::RGBA8Srgb : TextureFormat::RGBA8Unorm,
						  static_cast<uint32_t>(width),
						  static_cast<uint32_t>(height),
						  1,
						  false,
						  ImageKind::Sampled,
						  channels == 2 || channels == 4};
	TextureLoadResult result = createSampledTexture(device, image, debugName);
	if (result.texture)
		commands.writeTexture(result.texture, 0, 0, pixels.get(), size_t(width) * 4);
	return result;
}
} // namespace render_backend::gpu
