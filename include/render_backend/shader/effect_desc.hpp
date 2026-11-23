// Effect description: everything an effect file declares besides its code. That is the parameters by rate with their
// place in the records the renderer fills, textures and samplers (heap indices in those records), selections, sampler
// states, target sets, and techniques and passes with their entry functions and states. The front-end builds it from
// the preprocessed effect file. The shader cache stores it as effects/<name>.desc beside the compiled shaders, in the
// text form that text() writes and parse() reads; a deployed folder contains only these files and the bytecode.
#pragma once
#include <render_backend/shader/vertex_semantic.hpp>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>

namespace render_backend::shader
{
// Authored: shader functions are the entry points, compiled as written. RendererRecords: the front-end generates
// entry points that load the renderer's bindless records (requires ShaderPolicy::recordAbi).
enum class EntryModel : uint8_t
{
	Authored,
	RendererRecords,
};
enum class Stage : uint8_t
{
	Vertex,
	Pixel,
	Mesh,
	Amplification,
	Compute,
};
constexpr uint32_t C_STAGES = 5;

// How a pass's geometry reaches its pixel shader.
enum class Path : uint8_t
{
	Vertices, // vertex shader fed by the input assembler (the vertex function's own vertex format)
	Pool,	  // vertex shader that pulls its vertex from the scene pools by SV_VertexID / SV_InstanceID
	Meshlets, // generated amplification and mesh shaders over the scene pools
};
const char *pathName(Path path); // "", "pool", "ml"

enum class TextureDim : uint8_t
{
	None,
	Tex2D,
	Cube,
	Tex3D,
};

// How often the renderer writes a parameter: Frame into the effect's frame record (once per frame), Instance into
// the instance data after the renderer's common instance header (when an instance changes), Material into the
// material record (when a material changes).
enum class Rate : uint8_t
{
	Frame,
	Instance,
	Material,
};
constexpr uint32_t C_RATES = 3;
const char		  *rateName(Rate rate);

// ParamDesc::use bits
constexpr uint8_t C_USE_RECORD = 1;	 // in its rate's record: the code reads it (offset/size)
constexpr uint8_t C_USE_STATE = 2;	 // a sampler state or a blend/alpha-reference state names it
constexpr uint8_t C_USE_TEXTURE = 4; // a texture: its record field is a ResourceDescriptorHeap index
constexpr uint8_t C_USE_DERIVED = 8; // computed by the shader from the renderer's headers by its semantic (WORLD,
									 // WORLDVIEWPROJECTION, SUNDIRECTION0, ALPHA ...), never stored

// An annotation of a parameter, technique or pass.
struct Annotation
{
	eastl::string name;
	eastl::string type;	 // int, float, float2, string ...
	eastl::string value; // numbers joined by ',' or a quoted string
};

struct ParamDesc
{
	eastl::string name;			// as declared (g_MatColor, t_DiffuseTexture ...)
	eastl::string type;			// HLSL type as declared (float4x3, LightPoint, Texture2D, string ...)
	eastl::string semantic;		// WORLDVIEWPROJECTION, DIFFUSE ... or empty
	eastl::string defaultValue; // numbers joined by ',' (bool as 0/1), a quoted string, or empty: all zero
	uint32_t	  count = 0;	// array length; 0: not an array
	Rate		  rate = Rate::Material;
	uint8_t		  use = 0; // C_USE_*
	TextureDim	  dim = TextureDim::None;
	int32_t		  offset = -1;		 // byte offset in its rate's record (C_USE_RECORD), else -1
	uint32_t	  size = 0;			 // bytes in the record (tight packing, 4-byte units; a texture: 4)
	bool		  writable = false;	 // a resource the shaders write (RWTexture2D, RWStructuredBuffer ...)
	eastl::string alias;			 // the prelude parameter with the same semantic: this one has no field of
									 // its own and holds that parameter's value
	eastl::string			  where; // the declaration, for messages: "effects/water.hlsl:23"
	eastl::vector<Annotation> annotations;
};

// A selection: a compile-time value of the shaders and/or a key of the pipeline state. The renderer chooses one value
// per draw (usually from the material); each combination a pass uses gets its own compiled shader and pipeline.
struct SelectionDesc
{
	struct Value
	{
		eastl::string name; // a named int value (NONE, FRONT ...), else empty
		int32_t		  value = 0;
	};
	eastl::string		 name;
	bool				 isBool = true;
	int32_t				 defaultValue = 0;
	eastl::vector<Value> values; // the values that are built, in declared order; never empty
};

// The values of an effect's selections, in EffectDesc::selections order (an empty vector: all defaults).
using SelectionValues = eastl::vector<int32_t>;

// A state as written: an expression over selections, enum names, numbers and, for data and sampler states, material
// parameters. EffectDesc::resolve evaluates it for given selection values.
struct StateValue
{
	eastl::string state; // CullMode, DepthWrite, SrcBlend, MinFilter ...
	eastl::string value; // the expression's tokens joined by single blanks
};

// A SamplerState of the effect. A sampler declared in a file under <root>/common/ is a shared template and appears in
// an effect only if that effect's code samples with it.
struct SamplerDesc
{
	eastl::string			  name;	   // the name the code samples with
	eastl::string			  texture; // its Texture = state, may be empty
	TextureDim				  dim = TextureDim::Tex2D;
	int32_t					  offset = -1; // byte offset of its SamplerDescriptorHeap index in the material record
	eastl::vector<StateValue> states; // Filter and address modes, LOD controls, and material-parameter expressions.
};

// Blend factors shared by the renderer's pipeline state and the shader records. The prelude must use the same
// numbering.
enum class BlendFactor : uint8_t
{
	One = 0,
	Zero = 1,
	SrcColor = 2,
	InvSrcColor = 3,
	SrcAlpha = 4,
	InvSrcAlpha = 5,
	DestColor = 6,
	InvDestColor = 7,
	DestAlpha = 8,
	InvDestAlpha = 9,
	SrcAlphaSat = 10,
	Constant = 11,
	InvConstant = 12,
	BothSrcAlpha = 13,
	BothInvSrcAlpha = 14,
	Unspecified = 15,
};
static_assert(uint32_t(BlendFactor::Unspecified) < 16, "material blend factors occupy four bits");

constexpr BlendFactor blendFactorOf(uint32_t value)
{
	return value <= uint32_t(BlendFactor::Unspecified) ? BlendFactor(value) : BlendFactor::One;
}
constexpr uint32_t packMaterialBlend(BlendFactor src, BlendFactor dst, BlendFactor srcAlpha, BlendFactor dstAlpha)
{
	// Four 4-bit blend factors and the enable bit, as stored in the GPU material record.
	return uint32_t(src) | uint32_t(dst) << 4 | uint32_t(srcAlpha) << 8 | uint32_t(dstAlpha) << 12 | 1u << 16;
}
enum class SamplerFilter : uint8_t
{
	Point = 0,
	Linear = 1,
	Anisotropic = 2,
	BaseMip = 3,
};
constexpr int32_t C_EFFECT_BLEND_ADD = 1;

enum class SamplerAddress : uint8_t
{
	Wrap,
	Mirror,
	Clamp,
	Border,
	MirrorOnce,
	Unspecified,
};
struct SamplerSettings
{
	SamplerFilter  minFilter = SamplerFilter::Linear;
	SamplerFilter  magFilter = SamplerFilter::Linear;
	SamplerFilter  mipFilter = SamplerFilter::Linear;
	SamplerAddress addressU = SamplerAddress::Wrap;
	SamplerAddress addressV = SamplerAddress::Wrap;
	uint8_t		   anisotropy = 1;
};

// A stage of a pass: the effect function and its compile-time arguments (selection names or literals).
struct ShaderRef
{
	eastl::string				 function; // empty: the pass has no shader of this stage
	eastl::vector<eastl::string> args;
	eastl::vector<eastl::string> reads; // the selections its code reads by name (its own and the functions it calls)
	bool						 empty() const
	{
		return function.empty();
	}
};

struct PassDesc
{
	eastl::string			  name;
	eastl::vector<Annotation> annotations;
	ShaderRef				  shaders[C_STAGES];
	eastl::vector<StateValue> states; // the RenderState block's lines, then the pass's own (later lines win)
	bool pixelOwnOutputs = false;	  // the pixel function returns a struct of SV_Target outputs: BlendMode::Own
	bool meshlets = false;	   // Path::Meshlets: generated amplification and mesh entries from the vertex function
	bool poolVertices = false; // Path::Pool: a generated vertex entry pulling from the pools
	// Path::Vertices: the vertex function fed by the pass's own vertex buffer. A pass has one geometry path
	// (meshlets, else the pools, else its own vertices); "Vertices = true;" adds its own vertices to another path.
	bool vertices = false;
	// The vertex function's inputs (the input layout of Path::Vertices): semantic name, index, HLSL type.
	struct Input
	{
		eastl::string semantic;
		uint32_t	  index = 0;
		eastl::string type;
	};
	eastl::vector<Input> inputs;
	// The values a selection is restricted to in this pass ("Pin g_AlphaTest = false;" in the pass or its technique).
	// Pipeline enumeration and getShader allow only these. values is never empty.
	struct Pin
	{
		eastl::string		   selection;
		eastl::vector<int32_t> values;
	};
	eastl::vector<Pin> pins;

	const ShaderRef &shader(Stage stage) const
	{
		return shaders[static_cast<uint32_t>(stage)];
	}
};

struct TechniqueDesc
{
	eastl::string			  name;
	eastl::vector<Annotation> annotations;
	eastl::vector<PassDesc>	  passes;
};

// A register the front-end assigned to a resource declared without one (for example a ConstantBuffer<T>).
struct RegisterDesc
{
	eastl::string name;
	char		  kind = 'b'; // b, t, s, u
	uint32_t	  index = 0;
	uint32_t	  space = 0;
};

// A struct type stored in a record (for example the element type of an array parameter), with its field offsets
// (tight packing).
struct StructDesc
{
	struct Field
	{
		eastl::string name;
		eastl::string type; // the record's type (bool -> uint, half -> float)
		uint32_t	  count = 0;
		uint32_t	  offset = 0;
	};
	eastl::string		 name;
	uint32_t			 size = 0;
	eastl::vector<Field> fields;
};

// The layout hash of one effect, as compiled into the host (C_EFFECT_LAYOUTS of the generated layout header).
struct EffectLayout
{
	const char *effect;
	uint64_t	hash;
};

enum class CullMode : uint8_t
{
	None,
	Front,
	Back,
};
enum class CompareFunc : uint8_t
{
	Never,
	Less,
	Equal,
	LessEqual,
	Greater,
	NotEqual,
	GreaterEqual,
	Always,
};
enum class Topology : uint8_t
{
	Triangles,
	TriangleStrip,
	Lines,
	LineStrip,
	Points,
};

// A named set of render target formats, declared in effect source as
//   TargetSet TG_Scene { Color0 = RGBA16_FLOAT; Depth = D32_FLOAT; };
// Formats are DXGI names without the DXGI_FORMAT_ prefix. A pipeline is created for one target set.
//
// A pass selects its sets with "Targets = A, B;" and gets one pipeline per set. Default convention: a draw pass
// without a Targets state uses the set named TG_Scene if the effect (usually through the prelude) declares one;
// otherwise, and for compute passes, its pipeline has no fixed formats (an empty TargetSetDesc). A dual-source
// blended pass (BlendMode::DualSource) may name only sets with one colour target.
struct TargetSetDesc
{
	eastl::string				 name;
	eastl::vector<eastl::string> colors; // Color0, Color1 ... in order
	eastl::string				 depth;	 // empty: no depth buffer
};

enum class BlendMode : uint8_t
{
	DualSource, // the one blend state result = A + dst * B (the pixel entry writes A and B)
	None,		// no blending, one colour (Blend = NONE)
	Own,		// the pixel function returns its own outputs (a struct): the renderer chooses the blend
};

// The states of a pass for given selection values: the pipeline key, plus the blend and alpha reference that the
// pixel entry computes (each a number, or the name of a material parameter the renderer packs into the material
// header).
struct PassState
{
	CullMode	cullMode = CullMode::Back;
	bool		depthTest = true;
	bool		depthWrite = true;
	CompareFunc depthFunc = CompareFunc::LessEqual;
	uint8_t		colorWriteMask = 15;
	int32_t		depthBias = 0;
	float		slopeScaledDepthBias = 0.0f;
	BlendMode	blend = BlendMode::DualSource;
	bool		alphaTest = false;
	bool		meshlets = false;	  // Meshlets = true (PassDesc::meshlets where the vertex function allows it)
	bool		poolVertices = false; // PoolVertices = true (PassDesc::poolVertices likewise)
	bool		vertices = false;	  // Vertices = true: also drawn from its own vertices (PassDesc::vertices)
	Topology	topology = Topology::Triangles;
	bool		precache = true; // Precache = false: not among the pipelines built at start (debug, examples)
	// Screen = true: a full-screen pass drawn from its own vertices (the full-screen triangle). Its entries load no
	// instance header unless their code reads instance data; compute entries behave the same way.
	bool						 screen = false;
	eastl::vector<eastl::string> targets; // Targets = A, B;: target set names (TargetSetDesc); empty: the default
	// Data states, applied in the pixel entry: a number as text, a material parameter's name, or empty when the pass
	// does not set it. Unset blend states come from the material header's blend flags, unless BlendEnable is a
	// constant false.
	eastl::string blendEnable, srcBlend, destBlend, separateAlphaBlend, srcBlendAlpha, destBlendAlpha;
	eastl::string alphaRef = "0";
	// BackFaceCull: "0" (shaders cull nothing), "1" (shaders cull all back faces) or the name of a bool material
	// parameter (culled per material). When it is not "0" the pipeline culls nothing; the generated meshlet entry
	// culls the primitive (SV_CullPrimitive) and the vertex and pool paths' pixel entry discards back faces.
	eastl::string backFaceCull = "0";
};

// One pipeline an effect can need (ShaderCache::pipelines), keyed by a stable hash of everything it is made of:
// states, targets, topology, sample count and shader variants.
struct PipelineDesc
{
	eastl::string	technique;
	eastl::string	pass;
	Path			path = Path::Vertices;
	SelectionValues values; // the selections the pass reads at this pipeline's values; the others at their defaults
	PassState		state;
	TargetSetDesc	targets; // empty name: no fixed framebuffer formats
	uint32_t		samples = 1;
	eastl::string	shaders[C_STAGES]; // logical variant IDs; empty if the stage has no shader
	bool			precache = true;   // Precache = false: built on demand, not at start
	eastl::string	key;			   // the canonical text that hash is computed from
	uint64_t		hash = 0;
};

// A row of the remap table (ShaderCache::setRemapTable). It maps a material of an original effect and technique to
// the effect and technique that draw it, with selection values, material values the row fixes, and which caller
// parameter feeds which effect parameter.
//   <original> <technique> -> <effect> <technique> [selection=value ...] | [param=v[,v...] ...] | [caller=param ...]
// A line "passthrough <name> ..." lists names drawn as they are. '#' starts a comment.
struct RemapRow
{
	eastl::string original, originalTechnique; // as the row names them ('*': any)
	eastl::string effect, technique;		   // what draws it
	eastl::vector<eastl::pair<eastl::string, eastl::string>> selections; // name, value as written (true, 3, BACK)
	struct Value
	{
		eastl::string		 param;
		eastl::vector<float> values;
	};
	eastl::vector<Value>									 values; // section 2: fixed by the row
	eastl::vector<eastl::pair<eastl::string, eastl::string>> params; // section 3: caller parameter -> effect parameter
	bool		  passthrough = false;								 // not remapped: effect/technique as asked
	bool		  fallback = false;									 // no row named it: the default row
	eastl::string where;											 // file:line of the row
};
// Parses a remap table's text into rows and passthrough names; false with "file:line: error: ..." in error.
bool parseRemap(const char *file, const eastl::string &text, eastl::vector<RemapRow> &rows,
				eastl::vector<eastl::string> &passthrough, eastl::string &error);

// An element of the renderer's vertex layout: stream, byte offset, format (the renderer's own code, passed back in
// VertexBinding), semantic and semantic index.
struct VertexElement
{
	uint16_t	   stream = 0;
	uint16_t	   offset = 0;
	uint8_t		   format = 0;
	VertexSemantic semantic = VertexSemantic::Position;
	uint8_t		   semanticIndex = 0;
};
// One input of a pass's vertex function bound to the renderer's vertex layout (the input layout of Path::Vertices):
// the HLSL semantic name and index, the HLSL type, and the element that feeds it. bound is false when no element
// matches; D3D9 read (0, 0, 0, 1) for such an input, so the renderer should feed a constant there.
struct VertexBinding
{
	eastl::string semantic;
	uint32_t	  index = 0;
	eastl::string type;
	bool		  bound = false;
	uint16_t	  stream = 0;
	uint16_t	  offset = 0;
	uint8_t		  format = 0; // VertexElement::format of the matching element
};
// Matches a pass's vertex inputs (PassDesc::inputs) with the renderer's layout by semantic (the HLSL names in
// vertex_semantic.hpp) and index. false, with the input named in error, only for a semantic the backend does not know.
bool vertexLayout(const PassDesc &pass, const VertexElement *elements, uint32_t count,
				  eastl::vector<VertexBinding> &out, eastl::string &error);

class EffectDesc
{
  public:
	eastl::string				 name;
	eastl::string				 source; // the effect file, relative to the shader root ('/')
	eastl::vector<ParamDesc>	 params;
	eastl::vector<SelectionDesc> selections;
	eastl::vector<SamplerDesc>	 samplers;
	eastl::vector<TechniqueDesc> techniques;
	eastl::vector<RegisterDesc>	 registers;
	eastl::vector<StructDesc>	 structs;				   // the struct types in the records
	eastl::vector<TargetSetDesc> targetSets;			   // the render-target sets the passes name
	uint32_t					 recordSize[C_RATES] = {}; // bytes of each rate's record (0: the effect has none)
	bool						 materialHeader = false;   // material record starts with the renderer's header
	uint32_t					 materialHeaderSize = 0;
	EntryModel					 entryModel = EntryModel::Authored;
	uint64_t					 policyHash = 0;
	uint64_t layoutHash = 0;			// hash of the record layouts and registers (see ShaderCache::expectLayouts)
	eastl::vector<eastl::string> notes; // front-end warnings ("file:line: ..."), not errors

	// The .desc text form: parse reads what text writes; false with "line N: ..." in error. Effect names cannot
	// contain blanks.
	bool		  parse(const char *text, size_t size, eastl::string &error);
	bool		  load(const char *path, eastl::string &error);
	eastl::string text() const;

	const TechniqueDesc *findTechnique(const char *techniqueName) const;
	const ParamDesc		*findParam(const char *paramName) const;
	int32_t				 findSelection(const char *selectionName) const; // its index, -1 if none

	// Every selection's default.
	void defaults(SelectionValues &values) const;
	// Carries values over from an older description of this effect (before a hot reload) by selection name. A
	// selection both declare keeps its value if this description still lists that value; the rest get defaults. Use it
	// when a Description change arrives (selections may be added, dropped or reordered).
	void carryOver(const EffectDesc &old, const SelectionValues &oldValues, SelectionValues &values) const;
	// The states of a pass for the values (empty: the defaults); false with the reason in error.
	bool resolve(const PassDesc &pass, const SelectionValues &values, PassState &out, eastl::string &error) const;
	// Evaluates one state expression to a number (enum names give their numbers) or, for data and sampler states, to
	// a material parameter's name; false with the reason in error.
	bool evaluate(const char *state, const eastl::string &expression, const SelectionValues &values,
				  eastl::string &text, eastl::string &error) const;
};

// The native value of a state name (CullMode NONE, SrcBlend SRC_ALPHA, AddressU CLAMP, etc.), or INT32_MIN if unknown.
int32_t stateEnum(const char *state, const char *name);
// Resolve the sampler's authored states and optional runtime overrides, all in the native state encoding.
using SamplerParameterLookup = bool (*)(const void *context, const ParamDesc &param, int32_t &value);
SamplerSettings resolveSamplerSettings(const EffectDesc &desc, const SamplerDesc &sampler,
									   const void *context = nullptr, SamplerParameterLookup lookup = nullptr);
// The kind of a state name: 1 pipeline state, 2 data state (blend, alpha reference), 3 sampler state (including
// Texture), 4 Targets, 0 unknown.
int32_t stateKind(const char *state);

} // namespace render_backend::shader
