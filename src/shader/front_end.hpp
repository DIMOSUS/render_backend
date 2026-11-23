// Effect language front-end (private to src/shader). It reads an effect after the DXC preprocessor, removes the
// constructs that are not HLSL (parameter blocks, selections, sampler and render states, techniques), and builds the
// EffectDesc. It also generates the code the author does not write: the parameter records, register assignments,
// and, per (pass, stage, selection values), the entry point that links the stages and applies alpha test and blend.
// The shader cache is its only user, both at run time and in an offline compiler built on ShaderCache.
#pragma once
#include <render_backend/shader/effect_desc.hpp>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <stdint.h>

namespace render_backend::shader
{
struct ShaderPolicy;
struct LayoutHeaderPolicy;
struct ShaderRecordAbi;
} // namespace render_backend::shader

namespace render_backend::shader::front
{
// "render-fx N", recorded in every manifest. Bump it whenever the generated text changes: every compiled file then
// becomes stale.
extern const char *const C_FRONTEND_VERSION;

struct Member
{
	eastl::string modifiers; // centroid, nointerpolation, uniform, in ... joined by blanks
	eastl::string type;
	eastl::string name;
	uint32_t	  count = 0;
	eastl::string semantic;
};

struct Struct
{
	eastl::string		  name;
	eastl::vector<Member> members;
};

struct Function
{
	eastl::string				 name;
	eastl::string				 returnType;
	eastl::string				 returnSemantic;
	eastl::vector<Member>		 params;
	eastl::string				 attributes;		 // [numthreads(...)] ... as written before it
	eastl::vector<eastl::string> idents;			 // the identifiers of its body (what it reads, what it calls)
	uint32_t					 file = 0, line = 0; // where it is declared (messages)
	bool method = false; // a struct's member function: its body counts for what it reads, never an entry
};

// one compiled shader: the entry point's name, the variant's file id (<entry>[.<values>]), and the generated text
struct Entry
{
	eastl::string name;
	eastl::string variant;
	eastl::string text;
	eastl::string prefix; // before the body: the selections as static constants of this variant's values
};

class Effect
{
  public:
	// preprocessed: DXC's -P output of the effect (the prelude included first), with #line directives
	bool parse(const char *effectName, const char *sourceRel, const eastl::string &preprocessed,
			   const ShaderPolicy &policy, eastl::string &error);

	EffectDesc desc;
	// what DXC compiles before the entry text: the effect with its constructs blanked out (lines and columns stay),
	// registers written in, and the records and their loader where the last parameter block stood
	eastl::string body;

	// the entry of one stage of a pass for the selection values (empty: the defaults); meshlets: the pixel entry of
	// the meshlet path (the mesh and amplification stages of a pass without its own are generated, always meshlets)
	bool entry(const PassDesc &pass, Stage stage, const SelectionValues &values, Path path, Entry &out,
			   eastl::string &error) const;

	// the renderer's records as the prelude declares them (push constants, frame header, instance header, material header,
	// the light list, the pools' records): their tight layouts, the one definition the generated C++ header carries
	void standardRecords(const char *const *names, size_t count, eastl::vector<StructDesc> &out) const;

  private:
	const Struct   *findStruct(const eastl::string &name) const;
	uint32_t		standardStruct(const eastl::string &name, eastl::vector<StructDesc> &out) const;
	const Function *findFunction(const eastl::string &name) const;
	// every input of the vertex function is something a pool vertex has (the meshlet path can feed it)
	bool meshletInputs(const Function &f) const;

	eastl::vector<Struct>		 m_structs;
	eastl::vector<Function>		 m_functions;
	eastl::vector<eastl::string> m_files; // the file names of the #line directives
	bool						 m_records = false;
	const ShaderRecordAbi		*m_recordAbi = nullptr;
};

// the variant id and the entry's name of one stage of a pass for the values, from the description alone (the cache
// finds compiled files without parsing the source); false with the reason
bool variantOf(const EffectDesc &desc, const PassDesc &pass, Stage stage, const SelectionValues &values, Path path,
			   eastl::string &variant, eastl::string &entry, eastl::string &error, const ShaderRecordAbi *abi);

// Every combination of the values of the selections a stage of a pass depends on (its arguments, alpha test, blend),
// within the pass's pins, with all other selections at their defaults. ShaderCache::updateAll builds these.
void combinations(const EffectDesc &desc, const PassDesc &pass, Stage stage, eastl::vector<SelectionValues> &out);

// every pipeline of the effect: per pass, the values of only the selections the pass reads (its states and its
// shaders), within its pins, x its target sets x its paths; deduplicated by key
void pipelines(const EffectDesc &desc, eastl::vector<PipelineDesc> &out, const ShaderRecordAbi *abi);
// the geometry paths of a pass: one (meshlets, else the pools, else its own vertices), its own vertices besides the
// other when the pass says "Vertices = true;"; a pass with its own mesh shader: meshlets; a dispatch: Vertices
void geometryPaths(const PassDesc &pass, eastl::vector<Path> &out);
// the selections a pass reads (its states and its shaders' arguments and code), sorted indices: what makes its
// pipelines differ
void passSelections(const EffectDesc &desc, const PassDesc &pass, eastl::vector<int32_t> &used);

// a pass's pins as the effect would write them ("Pin g_AlphaTest = [false, true];"), and whether values are inside
eastl::string pinText(const EffectDesc &desc, const PassDesc &pass);
bool		  pinned(const EffectDesc &desc, const PassDesc &pass, const SelectionValues &values, eastl::string &which);

// The C++ layout headers: record layouts, registers and layout hashes. `layer` (headers.sharedHeader) holds the
// prelude's records, VertexInput and the effects named in layerEffects. `host` (headers.consumerHeader) holds every
// other effect and the C_EFFECT_LAYOUTS table of all layout hashes; it includes the layer header.
void layoutHeaders(const LayoutHeaderPolicy &headers, const ShaderRecordAbi *recordAbi,
				   const eastl::vector<EffectDesc> &effects, const eastl::vector<StructDesc> &standard,
				   const eastl::vector<eastl::string> &layerEffects, eastl::string &layer, eastl::string &host);

} // namespace render_backend::shader::front
