// Vertex layout description: each attribute's stream, semantic and semantic index, format and byte offset, plus
// helpers for format sizes and conversion to and from floats. The enumerator values are internal and may change; a
// caller that stores layouts in files must map them to its own values. Semantics are shared with the shader module,
// whose vertex input bindings use the same values.
#pragma once
#include <render_backend/shader/vertex_semantic.hpp>
#include <stdint.h>

namespace render_backend::gpu
{
using VertexSemantic = render_backend::shader::VertexSemantic;

enum class VertexFormat : uint8_t
{
	Float1, // Float1..Float4 are consecutive: component count is format - Float1 + 1.
	Float2,
	Float3,
	Float4,
	Half2,
	Half4,
	Unorm8x4,
	Bgra8Unorm, // Bytes in B, G, R, A order, unorm; typically a vertex colour.
	Uint8x4,
	Sint16x2,
	Sint16x4,
	Snorm16x2,
	Snorm16x4,
	Unorm16x2,
	Unorm16x4,
	Uint10x3,  // x, y, z as 10-bit unsigned integers at bits 0, 10 and 20.
	Snorm10x3, // Same layout, as signed normalised values.
};

struct VertexAttribute
{
	uint8_t		   stream = 0; // 0: per vertex. 1: per instance.
	VertexSemantic semantic = VertexSemantic::Position;
	uint8_t		   semanticIndex = 0;
	VertexFormat   format = VertexFormat::Float3;
	uint16_t	   offset = 0; // Byte offset within the stream's element.
};

constexpr uint32_t C_MAX_VERTEX_ATTRIBUTES = 16;
struct VertexLayout
{
	uint32_t		count = 0;
	VertexAttribute attributes[C_MAX_VERTEX_ATTRIBUTES] = {};

	const VertexAttribute *begin() const
	{
		return attributes;
	}
	const VertexAttribute *end() const
	{
		return attributes + count;
	}
	// Stream 0 attribute with this semantic and index, or null.
	const VertexAttribute *find(VertexSemantic semantic, uint32_t index = 0) const
	{
		for (const VertexAttribute &a : *this)
			if (a.stream == 0 && a.semantic == semantic && a.semanticIndex == index)
				return &a;
		return nullptr;
	}
};

// Size of one attribute of `format`, in bytes.
uint32_t vertexFormatSize(VertexFormat format);
// Stride of `stream`: the end offset of its furthest attribute. No padding is added.
uint32_t vertexStride(const VertexLayout &layout, uint32_t stream = 0);
// Decodes an attribute to four floats; missing components are 0, and w is 1. Encoding clamps and rounds to the
// format's range.
void decodeVertexAttribute(VertexFormat format, const uint8_t *bytes, float out[4]);
void encodeVertexAttribute(VertexFormat format, uint8_t *bytes, const float in[4]);
// Hash of the stream 0 and 1 attributes and `stride`, suitable as a pipeline cache key.
uint64_t vertexLayoutKey(const VertexLayout &layout, uint32_t stride);
} // namespace render_backend::gpu
