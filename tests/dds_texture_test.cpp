#include <render_backend/gpu/dds_texture.hpp>
#include <render_backend/gpu/texture_format_nvrhi.hpp>
#include <cstdio>
#include <vector>

using namespace render_backend::gpu;

namespace
{
void write32(std::vector<uint8_t> &file, size_t at, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		file[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

std::vector<uint8_t> makeDds(uint32_t dxgiFormat, bool cube = false)
{
	// Two levels, 8x8 and 4x4, each with 16-byte BC blocks.
	constexpr size_t	 faceBytes = 64 + 16;
	const unsigned		 faces = cube ? 6 : 1;
	std::vector<uint8_t> file(C_DDS_DX10_HEADER_SIZE + faces * faceBytes, 0);
	write32(file, 0, 0x20534444); // "DDS "
	write32(file, 4, 124);		  // DDS_HEADER size
	write32(file, 12, 8);
	write32(file, 16, 8);
	write32(file, 28, 2);
	write32(file, 76, 32);		   // DDS_PIXELFORMAT size
	write32(file, 80, 4);		   // DDPF_FOURCC
	write32(file, 84, 0x30315844); // "DX10"
	write32(file, 128, dxgiFormat);
	write32(file, 132, 3); // Texture2D
	write32(file, 136, cube ? 4 : 0);
	write32(file, 140, 1);
	for (unsigned face = 0; face < faces; ++face)
	{
		const size_t offset = C_DDS_DX10_HEADER_SIZE + size_t(face) * faceBytes;
		file[offset] = static_cast<uint8_t>(2 * face + 1);
		file[offset + 64] = static_cast<uint8_t>(2 * face + 2);
	}
	return file;
}

bool check(bool condition, const char *name)
{
	if (!condition)
		std::fprintf(stderr, "DDS test failed: %s\n", name);
	return condition;
}

bool checkFormat(uint32_t dxgi, TextureFormat format, nvrhi::Format native, bool hasAlpha)
{
	const std::vector<uint8_t> file = makeDds(dxgi);
	DdsTextureView			   view;
	if (!check(readDdsTexture(file.data(), file.size(), view) == DdsReadError::None, "DX10 decode"))
		return false;
	const ImageDesc	  &desc = view.description();
	const GpuFormat	   layout = gpuFormatOf(desc.format);
	const DdsLevelView top = view.level(0, 0);
	const DdsLevelView mip = view.level(0, 1);
	return check(desc.format == format && desc.canvasHasAlpha == hasAlpha && layout.format == native &&
					 uint32_t(layout.dxgi) == dxgi && layout.block && layout.blockBytes == 16,
				 "native format") &&
		   check(desc.width == 8 && desc.height == 8 && desc.levels == 2 && view.faceCount() == 1, "2D description") &&
		   check(top.data == file.data() + C_DDS_DX10_HEADER_SIZE && top.size == 64 && top.rowPitch == 32 &&
					 top.width == 8 && top.height == 8 && top.data[0] == 1,
				 "borrowed top level") &&
		   check(mip.data == top.data + top.size && mip.size == 16 && mip.rowPitch == 16 && mip.width == 4 &&
					 mip.height == 4 && mip.data[0] == 2 && view.level(0, 2).data == nullptr &&
					 view.level(1, 0).data == nullptr,
				 "borrowed mip and range checks");
}
} // namespace

int main()
{
	bool ok = checkFormat(DXGI_FORMAT_BC6H_UF16, TextureFormat::BC6HUfloat, nvrhi::Format::BC6H_UFLOAT, false);
	ok &= checkFormat(DXGI_FORMAT_BC6H_SF16, TextureFormat::BC6HSfloat, nvrhi::Format::BC6H_SFLOAT, false);
	ok &= checkFormat(DXGI_FORMAT_BC7_UNORM, TextureFormat::BC7Unorm, nvrhi::Format::BC7_UNORM, true);
	ok &= checkFormat(DXGI_FORMAT_BC7_UNORM_SRGB, TextureFormat::BC7Srgb, nvrhi::Format::BC7_UNORM_SRGB, true);

	auto cube = makeDds(DXGI_FORMAT_BC7_UNORM, true);
	write32(cube, 144, 3); // DDS_ALPHA_MODE_OPAQUE
	DdsTextureView view;
	ok &= check(readDdsTexture(cube.data(), cube.size(), view) == DdsReadError::None && view.faceCount() == 6 &&
					view.description().cube && !view.description().canvasHasAlpha,
				"cube description");
	for (uint32_t face = 0; face < view.faceCount(); ++face)
		for (uint32_t mip = 0; mip < view.description().levels; ++mip)
		{
			const DdsLevelView level = view.level(face, mip);
			ok &= check(level.data != nullptr && level.data[0] == 2 * face + mip + 1, "face/mip order");
		}
	ok &= check(readDdsTexture(cube.data(), cube.size() - 1, view) == DdsReadError::Truncated && view.faceCount() == 0,
				"truncated last face clears prior view");
	ok &= check(readDdsTexture(cube.data(), C_DDS_DX10_HEADER_SIZE - 1, view) == DdsReadError::Truncated,
				"truncated DX10 header");
	write32(cube, 140, 2);
	ok &= check(readDdsTexture(cube.data(), cube.size(), view) == DdsReadError::UnsupportedResource, "cube array");
	write32(cube, 140, 1);
	write32(cube, 12, 4);
	ok &= check(readDdsTexture(cube.data(), cube.size(), view) == DdsReadError::InvalidDimensions, "nonsquare cube");

	auto bad = makeDds(DXGI_FORMAT_BC6H_TYPELESS);
	ok &= check(readDdsTexture(bad.data(), bad.size(), view) == DdsReadError::UnsupportedFormat, "typeless BC6H");
	write32(bad, 128, DXGI_FORMAT_BC7_UNORM);
	write32(bad, 28, 5);
	ok &= check(readDdsTexture(bad.data(), bad.size(), view) == DdsReadError::InvalidDimensions, "too many mips");
	write32(bad, 76, 0);
	ok &= check(readDdsTexture(bad.data(), bad.size(), view) == DdsReadError::InvalidHeader, "bad pixel header size");
	return ok ? 0 : 1;
}
