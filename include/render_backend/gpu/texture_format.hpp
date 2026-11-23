// API-neutral texture formats and image descriptions.
#pragma once
#include <stdint.h>

namespace render_backend::gpu
{
// Enumerator values are not grouped by family; do not rely on their order.
enum class TextureFormat : uint8_t
{
	Unknown,
	BGRA8,
	RGBA16Unorm,
	RGB10A2Unorm,
	RGBA16Float,
	RGBA32Float,
	RGBA8Snorm,
	RGBA16Snorm,
	BC1,
	BC2,
	BC3,
	RGBA8Unorm,
	RGBA8Srgb,
	BGRA8Srgb,
	BC1Srgb,
	BC2Srgb,
	BC3Srgb,
	BC4Unorm,
	BC4Snorm,
	BC5Unorm,
	BC5Snorm,
	BC6HUfloat,
	BC6HSfloat,
	BC7Unorm,
	BC7Srgb,
};

enum class ImageKind : uint8_t
{
	Sampled,
	OnRequest,
	Target,
};

struct ImageDesc
{
	TextureFormat format = TextureFormat::Unknown;
	uint32_t	  width = 0, height = 0;
	uint32_t	  levels = 1;
	bool		  cube = false;
	ImageKind	  kind = ImageKind::Sampled;
	bool		  canvasHasAlpha = false; // The source image carries a meaningful alpha channel.
};
} // namespace render_backend::gpu
