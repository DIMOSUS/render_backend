// Vertex semantics used to bind a vertex function's inputs (vertexLayout in effect_desc.hpp). Each value corresponds
// to the HLSL semantic name in its comment. The GPU layer's vertex layouts name their attributes with these values.
#pragma once
#include <stdint.h>

namespace render_backend::shader
{
enum class VertexSemantic : uint8_t
{
	Position,	  // POSITION
	BlendWeight,  // BLENDWEIGHT
	BlendIndices, // BLENDINDICES
	Normal,		  // NORMAL
	PointSize,	  // PSIZE
	TexCoord,	  // TEXCOORD
	Tangent,	  // TANGENT
	Binormal,	  // BINORMAL
	Color,		  // COLOR
	TessFactor,	  // TESSFACTOR
	PositionT,	  // POSITIONT
	Fog,		  // FOG
	Depth,		  // DEPTH
	Sample,		  // SAMPLE
};
} // namespace render_backend::shader
