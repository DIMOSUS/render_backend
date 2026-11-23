// Parses a GPU-ready DDS texture in place, without copying or decoding its texels.
#pragma once
#include <stddef.h>
#include <render_backend/gpu/texture_format.hpp>

namespace render_backend::gpu
{
inline constexpr size_t C_DDS_DX10_HEADER_SIZE = 148;

enum class DdsReadError : uint8_t
{
	None,
	NotDds,
	Truncated,
	InvalidHeader,
	UnsupportedFormat,
	UnsupportedResource,
	InvalidDimensions,
};

struct DdsLevelView
{
	const uint8_t *data = nullptr;
	size_t		   size = 0;
	uint32_t	   rowPitch = 0;
	uint32_t	   width = 0, height = 0;
};

class DdsTextureView
{
  public:
	const ImageDesc &description() const
	{
		return m_desc;
	}
	uint32_t faceCount() const
	{
		return m_pixels == nullptr ? 0 : m_desc.cube ? 6 : 1;
	}
	// Cube faces are ordered +X, -X, +Y, -Y, +Z, -Z. An invalid face or level returns an empty view.
	DdsLevelView level(uint32_t face, uint32_t mip) const;

  private:
	static constexpr uint32_t C_MAX_LEVELS = 15; // Full chain of a 16384 texture.
	friend DdsReadError		  readDdsTexture(const void *data, size_t size, DdsTextureView &out);
	ImageDesc				  m_desc;
	DdsLevelView			  m_levels[C_MAX_LEVELS] = {};
	const uint8_t			 *m_pixels = nullptr;
	size_t					  m_faceBytes = 0;
};

// `data` must outlive the view. Supports 2D textures and single cubemaps including their mip chains.
// On failure `out` is empty. The decoder uses tightly packed rows and allows trailing file data.
DdsReadError readDdsTexture(const void *data, size_t size, DdsTextureView &out);
} // namespace render_backend::gpu
