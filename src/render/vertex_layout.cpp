// Vertex format sizes, float conversion and layout keys. Half floats round half up. Normalised values are clamped and
// rounded the same way as the Direct3D 9 conversions.
#include <render_backend/gpu/vertex_layout.hpp>
#include <math.h>
#include <string.h>

using namespace render_backend::gpu;

namespace
{
float halfToFloat(uint16_t h)
{
	const uint32_t sign = uint32_t(h & 0x8000) << 16, exponent = h >> 10 & 31, mantissa = h & 0x3ff;
	uint32_t	   bits;
	if (exponent == 0)
	{
		if (mantissa == 0)
			bits = sign;
		else
		{
			// Subnormal: normalise.
			int		 e = -1;
			uint32_t m = mantissa;
			do
			{
				++e;
				m <<= 1;
			} while ((m & 0x400) == 0);
			bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3ff) << 13;
		}
	}
	else if (exponent == 31)
		bits = sign | 0x7f800000u | mantissa << 13;
	else
		bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

uint16_t floatToHalf(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, 4);
	const uint32_t sign = bits >> 16 & 0x8000;
	const int32_t  exponent = int32_t(bits >> 23 & 255) - 127 + 15;
	uint32_t	   mantissa = bits & 0x7fffff;
	if ((bits & 0x7fffffff) > 0x7f800000)
		return uint16_t(sign | 0x7e00); // NaN
	if (exponent >= 31)
		return uint16_t(sign | 0x7c00);
	if (exponent <= 0)
	{
		if (exponent < -10)
			return uint16_t(sign);
		mantissa |= 0x800000;
		const uint32_t shift = uint32_t(14 - exponent);
		uint32_t	   half = mantissa >> shift;
		if ((mantissa >> (shift - 1)) & 1)
			++half; // round half up
		return uint16_t(sign | half);
	}
	uint32_t half = sign | uint32_t(exponent) << 10 | mantissa >> 13;
	if (mantissa & 0x1000)
		++half; // Round half up. A carry into the exponent is correct.
	return uint16_t(half);
}

float clamp1(float v, float lo)
{
	return v < lo ? lo : v > 1.0f ? 1.0f : v;
}
} // namespace

//----------------------------------------------------------------------------------------------------------------------
uint32_t render_backend::gpu::vertexFormatSize(VertexFormat format)
{
	switch (format)
	{
	case VertexFormat::Float1:
	case VertexFormat::Bgra8Unorm:
	case VertexFormat::Uint8x4:
	case VertexFormat::Sint16x2:
	case VertexFormat::Unorm8x4:
	case VertexFormat::Snorm16x2:
	case VertexFormat::Unorm16x2:
	case VertexFormat::Uint10x3:
	case VertexFormat::Snorm10x3:
	case VertexFormat::Half2:
		return 4;
	case VertexFormat::Float2:
	case VertexFormat::Sint16x4:
	case VertexFormat::Snorm16x4:
	case VertexFormat::Unorm16x4:
	case VertexFormat::Half4:
		return 8;
	case VertexFormat::Float3:
		return 12;
	case VertexFormat::Float4:
		return 16;
	}
	return 0;
}

//----------------------------------------------------------------------------------------------------------------------
uint32_t render_backend::gpu::vertexStride(const VertexLayout &layout, uint32_t stream)
{
	uint32_t size = 0;
	for (const VertexAttribute &a : layout)
		if (a.stream == stream && a.offset + vertexFormatSize(a.format) > size)
			size = a.offset + vertexFormatSize(a.format);
	return size;
}

//----------------------------------------------------------------------------------------------------------------------
void render_backend::gpu::decodeVertexAttribute(VertexFormat format, const uint8_t *p, float out[4])
{
	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	int16_t	 s[4];
	uint16_t u[4];
	uint32_t v;
	switch (format)
	{
	case VertexFormat::Float4:
		memcpy(out, p, 16);
		break;
	case VertexFormat::Float3:
		memcpy(out, p, 12);
		break;
	case VertexFormat::Float2:
		memcpy(out, p, 8);
		break;
	case VertexFormat::Float1:
		memcpy(out, p, 4);
		break;
	case VertexFormat::Bgra8Unorm:
		out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f;
		break;
	case VertexFormat::Uint8x4:
		for (int i = 0; i < 4; ++i)
			out[i] = p[i];
		break;
	case VertexFormat::Unorm8x4:
		for (int i = 0; i < 4; ++i)
			out[i] = p[i] / 255.0f;
		break;
	case VertexFormat::Sint16x2:
	case VertexFormat::Snorm16x2:
		memcpy(s, p, 4);
		for (int i = 0; i < 2; ++i)
			out[i] = format == VertexFormat::Sint16x2 ? s[i] : clamp1(s[i] / 32767.0f, -1.0f);
		break;
	case VertexFormat::Sint16x4:
	case VertexFormat::Snorm16x4:
		memcpy(s, p, 8);
		for (int i = 0; i < 4; ++i)
			out[i] = format == VertexFormat::Sint16x4 ? s[i] : clamp1(s[i] / 32767.0f, -1.0f);
		break;
	case VertexFormat::Unorm16x2:
		memcpy(u, p, 4);
		out[0] = u[0] / 65535.0f, out[1] = u[1] / 65535.0f;
		break;
	case VertexFormat::Unorm16x4:
		memcpy(u, p, 8);
		for (int i = 0; i < 4; ++i)
			out[i] = u[i] / 65535.0f;
		break;
	case VertexFormat::Half2:
		memcpy(u, p, 4);
		out[0] = halfToFloat(u[0]), out[1] = halfToFloat(u[1]);
		break;
	case VertexFormat::Half4:
		memcpy(u, p, 8);
		for (int i = 0; i < 4; ++i)
			out[i] = halfToFloat(u[i]);
		break;
	case VertexFormat::Snorm10x3:
	case VertexFormat::Uint10x3:
		memcpy(&v, p, 4);
		for (int i = 0; i < 3; ++i)
		{
			const uint32_t c = v >> (10 * i) & 0x3ff;
			if (format == VertexFormat::Uint10x3)
				out[i] = float(c);
			else
				out[i] = clamp1(float(int32_t(c << 22) >> 22) / 511.0f, -1.0f);
		}
		break;
	}
}

//----------------------------------------------------------------------------------------------------------------------
void render_backend::gpu::encodeVertexAttribute(VertexFormat format, uint8_t *p, const float in[4])
{
	int16_t	 s[4];
	uint16_t u[4];
	uint32_t v = 0;
	auto	 unorm = [](float f, float scale) { return f <= 0.0f ? 0.0f : f >= 1.0f ? scale : f * scale + 0.5f; };
	auto	 snorm = [](float f) { return int16_t(lrintf(clamp1(f, -1.0f) * 32767.0f)); };
	auto	 sint16 = [](float f) { return int16_t(f < -32768.0f ? -32768.0f : f > 32767.0f ? 32767.0f : f); };
	switch (format)
	{
	case VertexFormat::Float4:
		memcpy(p, in, 16);
		break;
	case VertexFormat::Float3:
		memcpy(p, in, 12);
		break;
	case VertexFormat::Float2:
		memcpy(p, in, 8);
		break;
	case VertexFormat::Float1:
		memcpy(p, in, 4);
		break;
	case VertexFormat::Bgra8Unorm:
		p[2] = uint8_t(unorm(in[0], 255)), p[1] = uint8_t(unorm(in[1], 255)), p[0] = uint8_t(unorm(in[2], 255)),
		p[3] = uint8_t(unorm(in[3], 255));
		break;
	case VertexFormat::Uint8x4:
		for (int i = 0; i < 4; ++i)
			p[i] = uint8_t(in[i] < 0 ? 0 : in[i] > 255 ? 255 : in[i]);
		break;
	case VertexFormat::Unorm8x4:
		for (int i = 0; i < 4; ++i)
			p[i] = uint8_t(unorm(in[i], 255));
		break;
	case VertexFormat::Sint16x2:
	case VertexFormat::Snorm16x2:
		for (int i = 0; i < 2; ++i)
			s[i] = format == VertexFormat::Sint16x2 ? sint16(in[i]) : snorm(in[i]);
		memcpy(p, s, 4);
		break;
	case VertexFormat::Sint16x4:
	case VertexFormat::Snorm16x4:
		for (int i = 0; i < 4; ++i)
			s[i] = format == VertexFormat::Sint16x4 ? sint16(in[i]) : snorm(in[i]);
		memcpy(p, s, 8);
		break;
	case VertexFormat::Unorm16x2:
		u[0] = uint16_t(unorm(in[0], 65535)), u[1] = uint16_t(unorm(in[1], 65535));
		memcpy(p, u, 4);
		break;
	case VertexFormat::Unorm16x4:
		for (int i = 0; i < 4; ++i)
			u[i] = uint16_t(unorm(in[i], 65535));
		memcpy(p, u, 8);
		break;
	case VertexFormat::Half2:
		u[0] = floatToHalf(in[0]), u[1] = floatToHalf(in[1]);
		memcpy(p, u, 4);
		break;
	case VertexFormat::Half4:
		for (int i = 0; i < 4; ++i)
			u[i] = floatToHalf(in[i]);
		memcpy(p, u, 8);
		break;
	case VertexFormat::Snorm10x3:
		for (int i = 0; i < 3; ++i)
			v |= (uint32_t(lrintf(clamp1(in[i], -1.0f) * 511.0f)) & 0x3ff) << (10 * i);
		memcpy(p, &v, 4);
		break;
	case VertexFormat::Uint10x3:
		for (int i = 0; i < 3; ++i)
			v |= (uint32_t(in[i] < 0 ? 0 : in[i] > 1023 ? 1023 : in[i]) & 0x3ff) << (10 * i);
		memcpy(p, &v, 4);
		break;
	}
}

//----------------------------------------------------------------------------------------------------------------------
// FNV-1a over the stream 0 and stream 1 attributes and the stride.
uint64_t render_backend::gpu::vertexLayoutKey(const VertexLayout &layout, uint32_t stride)
{
	uint64_t h = 14695981039346656037ull;
	auto	 mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
	for (const VertexAttribute &a : layout)
		if (a.stream <= 1)
			mix(uint64_t(a.offset) | uint64_t(a.format) << 16 | uint64_t(a.semantic) << 24 |
				uint64_t(a.semanticIndex) << 32 | uint64_t(a.stream) << 40);
	mix(stride);
	return h;
}
