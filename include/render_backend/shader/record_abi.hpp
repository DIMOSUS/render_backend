// Host bindings for EntryModel::RendererRecords: the names and HLSL snippets the front-end uses when it generates
// record loaders and entry points. All strings are HLSL text or identifiers owned by the host.
#pragma once
#include <render_backend/shader/effect_desc.hpp>

namespace render_backend::shader
{
struct DerivedMember
{
	const char *name;
	uint32_t	components;
};

// Validates a structured derived parameter and, when code is not null, appends the HLSL that fills it.
using WriteDerived = bool (*)(const ParamDesc &param, const DerivedMember *members, size_t count, eastl::string *code,
							  eastl::string &error);

// A semantic whose value the shader computes from the renderer's headers instead of reading it from a record.
struct DerivedBinding
{
	const char	*semantic;	 // upper case; declarations match it case-insensitively
	const char	*type;		 // required HLSL type; Texture2D denotes a descriptor-heap index with any element type
	const char	*expression; // HLSL expression assigned to the parameter
	bool		 instance;	 // derived from instance data (not available to the screen loader)
	WriteDerived write = nullptr; // Optional shape validation and emission for a structured binding.
};

// How a pool vertex supplies an input semantic on the pool and meshlet paths.
struct PoolInputBinding
{
	const char *semantic;		// normalized semantic, for example "TEXCOORD0"
	const char *expression;		// HLSL expression of the pool vertex's value
	uint32_t	components;		// components of expression
	const char *extraComponent; // value appended when the input has more components
};

// The renderer's header at the start of a material record.
struct MaterialHeaderAbi
{
	const char *member;	  // field name in the material record
	const char *type;	  // HLSL struct type
	const char *variable; // static variable the loader fills
	uint32_t	size;	  // bytes
};

// Per rate: generated record struct names are typePrefix + effect name; the loader reads the record into a local
// variable with loadRecord<Type>(loadArguments).
struct RateRecordAbi
{
	const char *typePrefix;
	const char *local;
	const char *loadArguments;
};

// The parser links the authored stage signatures. The host emits the wrappers that read its meshlet and pool ABI.
struct PulledVertexSource
{
	const char *entry;
	const char *linkType;
	const char *linkTag;
	const char *vertexReturnType;
	const char *vertexFunction;
	const char *arguments;
	const char *inputs;
	const char *assignLink;
	const char *backFaceCull; // material parameter that culls back faces, or null
};

// Entry point names and HLSL helpers for the generated entries.
struct RendererEntryAbi
{
	const char *vertexPrefix;
	const char *poolPrefix;
	const char *pixelPrefix;
	const char *meshPrefix;
	const char *amplificationEntry;
	const char *computePrefix;
	const char *linkTypePrefix;
	const char *instanceMember;
	const char *instanceSemantic;
	const char *positionMember;
	const char *instanceExpression;
	const char *loadLinkedRecords;
	const char *blendOutput;
	const char *blendFunction;
	const char *materialBlendFactors;
	const char *materialAlphaRef;
	void (*writeAmplification)(const char *entry, const eastl::string &backFaceCull, eastl::string &code) = nullptr;
	void (*writePoolVertex)(const PulledVertexSource &source, eastl::string &code) = nullptr;
	void (*writeMeshlet)(const PulledVertexSource &source, eastl::string &code) = nullptr;
};

// Immutable for the lifetime of the cache. The host supplies a new version tag when changing this ABI; the tag is
// part of the policy hash, so a new tag makes every cached file stale. A host can deliberately keep an empty tag when
// taking over a byte-compatible existing ABI.
struct ShaderRecordAbi
{
	const char			   *versionTag;
	const DerivedBinding   *derived;
	size_t					derivedCount;
	const PoolInputBinding *poolInputs;
	size_t					poolInputCount;
	MaterialHeaderAbi		materialHeader;
	RateRecordAbi			rates[C_RATES];
	const char			   *loadRecord;		   // HLSL template function that loads a record
	const char			   *recordLoaderStart; // opens the full record loader function (closed by the front-end)
	const char			   *screenLoaderStart; // opens the screen loader (no instance data)
	const char			   *loadVertexEntry;   // statement an entry uses to run the full loader
	const char			   *loadScreenEntry;   // statement an entry uses to run the screen loader
	const char *const	   *instanceIdentifiers; // identifiers whose use means the code reads instance data
	size_t					instanceIdentifierCount;
	const RendererEntryAbi *entries;
};
} // namespace render_backend::shader
