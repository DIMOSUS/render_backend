#include <render_backend/gpu/dds_texture.hpp>
#include <dxgiformat.h>

namespace render_backend::gpu
{
namespace
{
constexpr uint32_t fourCC(char a, char b, char c, char d)
{
	return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

uint32_t read32(const uint8_t *bytes)
{
	return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

constexpr size_t   C_HEADER_BYTES = 128;
constexpr uint32_t C_PIXELFORMAT_FOURCC = 0x4;
constexpr uint32_t C_LEGACY_CUBE = 0x200, C_LEGACY_CUBE_FACES = 0xfc00, C_LEGACY_VOLUME = 0x200000;
constexpr uint32_t C_TEXTURE_2D = 3, C_DX10_CUBE = 0x4;
constexpr uint32_t C_ALPHA_MODE_MASK = 7, C_BLOCK_EDGE = 4;
constexpr uint32_t C_TEXTURE_SIZE_LIMIT = 16384;

struct FormatLayout
{
	TextureFormat format = TextureFormat::Unknown;
	uint32_t	  bytesPerUnit = 0;
	uint32_t	  unitEdge = 1;
	bool		  alpha = false;
};

FormatLayout dxgiLayout(uint32_t dxgi)
{
	using F = TextureFormat;
	switch (dxgi)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return {F::RGBA8Unorm, 4, 1, true};
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return {F::RGBA8Srgb, 4, 1, true};
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return {F::BGRA8, 4, 1, true};
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return {F::BGRA8Srgb, 4, 1, true};
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return {F::RGBA16Unorm, 8, 1, true};
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return {F::RGB10A2Unorm, 4, 1, true};
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return {F::RGBA16Float, 8, 1, true};
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return {F::RGBA32Float, 16, 1, true};
	case DXGI_FORMAT_R8G8B8A8_SNORM:
		return {F::RGBA8Snorm, 4, 1, true};
	case DXGI_FORMAT_R16G16B16A16_SNORM:
		return {F::RGBA16Snorm, 8, 1, true};
	case DXGI_FORMAT_BC1_UNORM:
		return {F::BC1, 8, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		return {F::BC1Srgb, 8, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC2_UNORM:
		return {F::BC2, 16, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC2_UNORM_SRGB:
		return {F::BC2Srgb, 16, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC3_UNORM:
		return {F::BC3, 16, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		return {F::BC3Srgb, 16, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC4_UNORM:
		return {F::BC4Unorm, 8, C_BLOCK_EDGE};
	case DXGI_FORMAT_BC4_SNORM:
		return {F::BC4Snorm, 8, C_BLOCK_EDGE};
	case DXGI_FORMAT_BC5_UNORM:
		return {F::BC5Unorm, 16, C_BLOCK_EDGE};
	case DXGI_FORMAT_BC5_SNORM:
		return {F::BC5Snorm, 16, C_BLOCK_EDGE};
	case DXGI_FORMAT_BC6H_UF16:
		return {F::BC6HUfloat, 16, C_BLOCK_EDGE};
	case DXGI_FORMAT_BC6H_SF16:
		return {F::BC6HSfloat, 16, C_BLOCK_EDGE};
	case DXGI_FORMAT_BC7_UNORM:
		return {F::BC7Unorm, 16, C_BLOCK_EDGE, true};
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return {F::BC7Srgb, 16, C_BLOCK_EDGE, true};
	default:
		return {};
	}
}

FormatLayout legacyLayout(uint32_t code)
{
	switch (code)
	{
	case fourCC('D', 'X', 'T', '1'):
		return dxgiLayout(DXGI_FORMAT_BC1_UNORM);
	case fourCC('D', 'X', 'T', '2'):
	case fourCC('D', 'X', 'T', '3'):
		return dxgiLayout(DXGI_FORMAT_BC2_UNORM);
	case fourCC('D', 'X', 'T', '4'):
	case fourCC('D', 'X', 'T', '5'):
		return dxgiLayout(DXGI_FORMAT_BC3_UNORM);
	case fourCC('A', 'T', 'I', '1'):
	case fourCC('B', 'C', '4', 'U'):
		return dxgiLayout(DXGI_FORMAT_BC4_UNORM);
	case fourCC('B', 'C', '4', 'S'):
		return dxgiLayout(DXGI_FORMAT_BC4_SNORM);
	case fourCC('A', 'T', 'I', '2'):
	case fourCC('B', 'C', '5', 'U'):
		return dxgiLayout(DXGI_FORMAT_BC5_UNORM);
	case fourCC('B', 'C', '5', 'S'):
		return dxgiLayout(DXGI_FORMAT_BC5_SNORM);
	default:
		return {};
	}
}
} // namespace

DdsLevelView DdsTextureView::level(uint32_t face, uint32_t mip) const
{
	if (face >= faceCount() || mip >= m_desc.levels)
		return {};
	DdsLevelView view = m_levels[mip];
	view.data += size_t(face) * m_faceBytes;
	return view;
}

DdsReadError readDdsTexture(const void *data, size_t size, DdsTextureView &out)
{
	out = {};
	const auto *bytes = static_cast<const uint8_t *>(data);
	if (bytes == nullptr || size < 4 || read32(bytes) != fourCC('D', 'D', 'S', ' '))
		return DdsReadError::NotDds;
	if (size < C_HEADER_BYTES)
		return DdsReadError::Truncated;
	if (read32(bytes + 4) != 124 || read32(bytes + 76) != 32)
		return DdsReadError::InvalidHeader;
	if ((read32(bytes + 80) & C_PIXELFORMAT_FOURCC) == 0)
		return DdsReadError::UnsupportedFormat;
	const uint32_t code = read32(bytes + 84);
	const uint32_t caps = read32(bytes + 112);
	if (caps & C_LEGACY_VOLUME)
		return DdsReadError::UnsupportedResource;

	FormatLayout layout;
	bool		 cube;
	bool		 opaque = false;
	size_t		 offset = C_HEADER_BYTES;
	if (code == fourCC('D', 'X', '1', '0'))
	{
		if (size < C_DDS_DX10_HEADER_SIZE)
			return DdsReadError::Truncated;
		const uint8_t *extended = bytes + C_HEADER_BYTES;
		if (read32(extended + 4) != C_TEXTURE_2D || read32(extended + 12) != 1)
			return DdsReadError::UnsupportedResource;
		cube = (read32(extended + 8) & C_DX10_CUBE) != 0;
		const uint32_t alphaMode = read32(extended + 16) & C_ALPHA_MODE_MASK;
		if (alphaMode > 4)
			return DdsReadError::InvalidHeader;
		opaque = alphaMode == 3;
		layout = dxgiLayout(read32(extended));
		offset = C_DDS_DX10_HEADER_SIZE;
	}
	else
	{
		cube = (caps & C_LEGACY_CUBE) != 0;
		if ((caps & C_LEGACY_CUBE_FACES) != (cube ? C_LEGACY_CUBE_FACES : 0))
			return DdsReadError::UnsupportedResource;
		layout = legacyLayout(code);
	}
	if (layout.format == TextureFormat::Unknown)
		return DdsReadError::UnsupportedFormat;

	const uint32_t width = read32(bytes + 16), height = read32(bytes + 12), mipCount = read32(bytes + 28);
	const uint32_t levels = mipCount == 0 ? 1 : mipCount;
	if (width == 0 || height == 0 || width > C_TEXTURE_SIZE_LIMIT || height > C_TEXTURE_SIZE_LIMIT ||
		(cube && width != height))
		return DdsReadError::InvalidDimensions;
	uint32_t fullChain = 1;
	for (uint32_t extent = width > height ? width : height; extent > 1; extent >>= 1)
		++fullChain;
	if (levels > fullChain)
		return DdsReadError::InvalidDimensions;

	DdsTextureView view;
	view.m_desc = {layout.format, width, height, levels, cube, ImageKind::Sampled, layout.alpha && !opaque};
	view.m_pixels = bytes + offset;
	const size_t payloadBegin = offset;
	uint32_t	 w = width, h = height;
	for (uint32_t mip = 0; mip < levels; ++mip)
	{
		const uint32_t columns = (w + layout.unitEdge - 1) / layout.unitEdge;
		const uint32_t rows = (h + layout.unitEdge - 1) / layout.unitEdge;
		const uint32_t rowPitch = columns * layout.bytesPerUnit;
		if (size_t(rowPitch) > SIZE_MAX / rows)
			return DdsReadError::InvalidDimensions;
		const size_t levelBytes = size_t(rowPitch) * rows;
		if (levelBytes > size - offset)
			return DdsReadError::Truncated;
		view.m_levels[mip] = {bytes + offset, levelBytes, rowPitch, w, h};
		offset += levelBytes;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	view.m_faceBytes = offset - payloadBegin;
	if (cube && view.m_faceBytes > (size - offset) / 5)
		return DdsReadError::Truncated;
	out = view;
	return DdsReadError::None;
}
} // namespace render_backend::gpu
