// Shader cache for the D3D12 renderer. Effect files (<root>/effects/<name>.hlsl) are parsed by the front-end,
// compiled with DXC (dxcompiler.dll, loaded at run time) and cached next to the sources.
//
// Per effect the cache writes <name>.desc, the effect description (effect_desc.hpp). A variant is one stage of one
// pass for one set of selection values; its bytecode (.dxil) and manifest (.dep) have ABI-tagged paths
// (compiledPath). Each manifest records the hashes of the source and of every file it includes (transitively), the
// DXC version, the front-end version, the entry, the selection values, the profile, the compile options and the
// source policy. A file is rebuilt only when one of these changed.
//
// An offline compiler and a running application can share this code and therefore write identical files.
#pragma once
#include <render_backend/shader/effect_desc.hpp>
#include <render_backend/shader/record_abi.hpp>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <stdint.h>

struct IDxcCompiler3;
struct IDxcUtils;
struct HINSTANCE__;

namespace render_backend::shader
{
class AutoConstants;

// Source and entry-generation policy. The default compiles authored HLSL without a prelude.
struct ShaderPolicy
{
	EntryModel	entries = EntryModel::Authored;
	// Text prepended to every effect before preprocessing (the prelude). It usually #includes the shared
	// declarations. Copied by ShaderCache::open.
	const char *sourcePrefix = "";
	// Root-relative path of the file that holds the prelude's shared declarations ('/' separators), as #line
	// directives name it. Declarations from this file count as shared, not as the effect's own. Copied by open.
	const char *preludePath = "";
	const ShaderRecordAbi *recordAbi = nullptr; // Required by RendererRecords; must outlive the cache.
};

// Host-owned output names and record list for the generated C++ layout headers (ShaderCache::layoutHeaders).
struct LayoutHeaderPolicy
{
	const char		  *sharedHeader;	  // file name of the shared header (shared records, GPU layer effects)
	const char		  *consumerHeader;	  // file name of the host header (other effects, C_EFFECT_LAYOUTS)
	const char *const *sharedRecords;	  // prelude struct names written to the shared header
	size_t			   sharedRecordCount;
	const char		  *recordPrefix;	  // prefix stripped from HLSL struct names in C++ (may be null)
	uint32_t		   poolVertices;	  // value of C_POOL_VERTICES
};

// Shader model 6.6 profile of a stage: vs_6_6, ps_6_6, ms_6_6, as_6_6 or cs_6_6.
const char *profileOf(Stage stage);
// "vs", "ps", "ms", "as", "cs"
const char *stageName(Stage stage);

// DXC options for every variant. -Zpr selects row-major matrices, so a row-major C++ matrix is copied into a record
// unchanged.
extern const char *const C_COMPILE_OPTIONS;

// Wrapper around dxcompiler.dll, loaded at run time. There is no import library, so a process that only loads
// prebuilt .dxil files does not need the DLL.
class Compiler
{
  public:
	struct Result
	{
		bool						 ok = false;
		eastl::vector<uint8_t>		 bytecode; // compile: the DXIL container (signed by DXC; no dxil.dll needed)
		eastl::string				 text;	   // preprocess: the preprocessed text
		eastl::string				 messages; // errors and warnings
		eastl::vector<eastl::string> files;	   // preprocess: the source and every include, root-relative, '/'
		eastl::vector<uint64_t>		 hashes;   // hashOf() of each file's bytes as read
	};

	Compiler() = default;
	Compiler(const Compiler &) = delete;
	Compiler &operator=(const Compiler &) = delete;
	~Compiler();

	// dllPath: that file. nullptr: dxcompiler.dll next to the executable, else the newest Windows SDK copy
	// (bin/10.*/x64).
	bool load(const char *dllPath, eastl::string &error);
	bool loaded() const
	{
		return m_compiler != nullptr;
	}
	// For example "1.8.2502.11 (<commit hash>)". A different compiler version makes every manifest stale.
	const eastl::string &version() const
	{
		return m_version;
	}
	const eastl::string &path() const
	{
		return m_path;
	}
	// Runs the DXC preprocessor over root/sourceRel with the in-memory sourcePrefix (the prelude) prepended; a
	// #line directive then restores the effect's own file name and line numbers. Includes resolve relative to the
	// including file, then to the root. Every file read, the source included, is recorded in out.files with its hash.
	bool preprocess(const char *root, const char *sourceRel, Result &out, const char *sourcePrefix = "") const;
	// Compiles preprocessed text (no includes left). name: the file the messages name before the first #line.
	bool compile(const char *name, const eastl::string &text, const char *entry, Stage stage, Result &out) const;
	// DXC's disassembly of a DXIL container.
	bool disassemble(const eastl::vector<uint8_t> &bytecode, eastl::string &out) const;

  private:
	HINSTANCE__	  *m_module = nullptr;
	IDxcCompiler3 *m_compiler = nullptr;
	IDxcUtils	  *m_utils = nullptr;
	eastl::string  m_version;
	eastl::string  m_path;
};

// 64-bit FNV-1a hash, used for file contents in manifests.
uint64_t hashOf(const void *data, size_t size);

// Why a file was (or would be) built.
enum class BuildReason : uint8_t
{
	UpToDate,
	NoOutput,		 // no output file, or its hash is not the manifest's
	NoManifest,		 // no .dep, or one this version cannot read
	SourceChanged,	 // the effect file (BuildEntry::file)
	IncludeChanged,	 // an included file (BuildEntry::file), or one that is gone
	CompilerChanged, // another dxcompiler version or another front-end version
	OptionsChanged,	 // the entry, the values, the profile, C_COMPILE_OPTIONS, the policy or the record layout
};
const char *reasonName(BuildReason reason);

enum class BuildResult : uint8_t
{
	UpToDate,
	Compiled,
	Failed, // the old files (if any) stay; BuildEntry::messages says why
	Stale,	// check only: it would be built
};

// One row of a build report: an effect description (description == true) or one compiled variant.
struct BuildEntry
{
	eastl::string	effect;
	bool			description = false;
	bool			changed = false; // description: its text differs from the previous one
	eastl::string	technique;
	eastl::string	pass;
	Stage			stage = Stage::Vertex;
	SelectionValues values;
	Path			path = Path::Vertices; // the variant's path (Meshlets for mesh and amplification)
	eastl::string	entry;				   // the DXIL entry point
	eastl::string	variant;			   // logical variant ID ("desc" for a description)
	BuildReason		reason = BuildReason::UpToDate;
	BuildResult		result = BuildResult::UpToDate;
	eastl::string	file;	  // the changed file for SourceChanged / IncludeChanged
	eastl::string	messages; // front-end and compiler output (errors, warnings)
};

// List of deployed effects in <folder>/effects: one effect name per line, sorted. ShaderCache::deploy writes it; a
// Prebuilt cache serves only the effects it names.
constexpr const char *C_DEPLOYED_EFFECTS = "deployed_effects.txt";

// A shader folder: <root>/effects/*.hlsl, optional includes (for example <root>/common/*.hlsli) and
// effects/aliases.txt, with the built files next to the effects.
//
// Threading: every build step runs under one lock, so a watcher thread can rebuild while the renderer asks for a
// new variant. updateAll takes the lock per step, not per round.
class ShaderCache
{
  public:
	enum class Mode : uint8_t
	{
		Prebuilt, // a deployed folder: .desc and .dxil only, nothing compiled, no compiler loaded
		Develop,  // a source tree: stale descriptions and variants are rebuilt before they are handed out
	};
	enum class Variants : uint8_t
	{
		Declared, // updateAll builds every combination of the declared selection values (offline compilation)
		Existing, // updateAll rebuilds only variants compiled before (the watcher: what the application asked for)
	};

	ShaderCache();
	ShaderCache(const ShaderCache &) = delete;
	ShaderCache &operator=(const ShaderCache &) = delete;
	~ShaderCache();

	// Prebuilt requires <root>/effects/C_DEPLOYED_EFFECTS. Develop loads DXC (dxcPath as in Compiler::load).
	bool				 open(const char *rootDir, Mode mode, eastl::string &error, const char *dxcPath = nullptr,
							  const ShaderPolicy &policy = {});
	const eastl::string &root() const
	{
		return m_root;
	}
	Mode mode() const
	{
		return m_mode;
	}
	const Compiler &compiler() const
	{
		return m_compiler;
	}
	// The layout hashes the host was built against (C_EFFECT_LAYOUTS of the generated layout header). Every
	// description handed out (loaded, up to date or rebuilt) of a listed effect must have the same layout hash. A
	// rebuilt description that differs is refused and the old description and shaders stay; the error says to
	// regenerate the layout headers and rebuild the host. Not called: no check. The array must outlive the cache.
	void expectLayouts(const EffectLayout *layouts, size_t count);
	// The auto-constants the renderer registered (auto_constants.hpp). Develop only: a description whose parameters
	// name one with another type or rate is refused, with the declaration's file and line; after a failed rebuild the
	// old description stays. nullptr: no check. Not owned; it must outlive the cache.
	void setAutoConstants(AutoConstants *autos)
	{
		m_autos = autos;
	}

	// The effects and aliases, sorted. Develop: effects/*.hlsl. Prebuilt: the names in C_DEPLOYED_EFFECTS only, so
	// files an older deployment left in the folder are ignored.
	void listEffects(eastl::vector<eastl::string> &names) const;
	// The effect an alias names (effects/aliases.txt), or the name itself.
	eastl::string effectOf(const char *name) const;
	// The description of an effect or alias (desc.name is the effect's name). Prebuilt: fails for an effect not in
	// C_DEPLOYED_EFFECTS. Develop: rebuilt first when stale. On failure error says why; in Develop desc then holds the
	// last intact description, if any.
	bool loadEffect(const char *name, EffectDesc &desc, eastl::string &error);
	// True for a loadable effect, or a name that a validated remap table row names as an original.
	bool hasEffect(const char *name);
	// The bytecode of one stage of a pass for the selection values (empty: the defaults). Prebuilt: only deployed
	// effects are served. Develop: stale variants are built first, lazily (only the combinations asked for); when
	// that fails, the old bytecode (if any) is returned and report.result is Failed with the error. false: no
	// bytecode.
	// path: Vertices (input assembler), Pool (a vertex entry that pulls vertices from the pools; its pixel entry
	// reads the records by instance) or Meshlets (generated amplification and mesh entries; the pixel entry reads by
	// instance as well). PassDesc::vertices, poolVertices and meshlets say which paths a pass has
	// (front::geometryPaths). Vertices for a pass that is not drawn from its own vertices selects its one geometry path.
	bool getShader(const char *effect, const char *technique, const char *pass, Stage stage,
				   const SelectionValues &values, eastl::vector<uint8_t> &bytecode, BuildEntry &report,
				   Path path = Path::Vertices);
	// The first pass whose shader for that stage is the named function, with default values (for effects that are
	// used by function name).
	bool getShader(const char *effect, const char *function, Stage stage, eastl::vector<uint8_t> &bytecode,
				   BuildEntry &report);
	// Builds every effect (aliases once): its description, then its variants. checkOnly compiles nothing and reports
	// Stale instead. One row per description and per variant, in effect and pass order; the rows start with what
	// getShader and loadEffect compiled since the last round.
	void updateAll(bool checkOnly, eastl::vector<BuildEntry> &report, Variants variants = Variants::Declared);
	// The generated code of an effect, for inspecting stage linkage: per pass "pass <technique> <pass> vs=<entry>" and
	// the vertex (or mesh) entry with default values.
	bool emit(const char *effect, eastl::string &out, eastl::string &error);
	// Generates the C++ layout headers. `layer` holds the records every effect shares and the effects named in
	// layerEffects; `host` holds the other effects and the C_EFFECT_LAYOUTS table for expectLayouts.
	bool layoutHeaders(const LayoutHeaderPolicy &headers, const eastl::vector<eastl::string> &layerEffects,
					   eastl::string &layer, eastl::string &host, eastl::string &error);
	// Every pipeline an effect can need: its passes x the values of the selections each pass reads (within its
	// pins) x its target sets x its geometry paths, each with a stable key hash. The renderer can create them at
	// start and key a disk pipeline cache with the hashes. A draw pass without a Targets state uses the effect's
	// TG_Scene target set if it declares one (see TargetSetDesc).
	bool pipelines(const char *effect, eastl::vector<PipelineDesc> &out, eastl::string &error);
	// Finds the pipeline of a draw in pipelines()' list. The values are reduced to the selections the pass reads and
	// must lie within its pins. The path must match exactly (unlike getShader's default). false with a message for a
	// pinned-out value (names the Pin to widen), a path the pass is not drawn by, or a pass drawn into several target
	// sets (the caller then picks by PipelineDesc::targets).
	static bool findPipeline(const EffectDesc &desc, const eastl::vector<PipelineDesc> &list, const char *technique,
							 const char *pass, Path path, const SelectionValues &values, const PipelineDesc *&out,
							 eastl::string &error);
	// The bytecode of every stage of a pipeline: exactly the variants pd.shaders[] names (bytecode[stage] stays empty
	// for a stage without one). false with the reason when one is missing or a different variant would be served.
	bool pipelineShaders(const char *effect, const PipelineDesc &pd, eastl::vector<uint8_t> (&bytecode)[C_STAGES],
						 eastl::string &error);
	// Sets the remap table file, root-relative with '/' (for example "effects/<name>.remap"; deploy copies
	// effects/*.remap), and optionally a file of extra effect names that rows may use as originals besides the tree's
	// effects. With no names file (null, "", or missing as in a deployed folder) the originals are not checked. Set it
	// before the first remap(); without a table remap() fails and the watcher reports no remap changes.
	void		  setRemapTable(const char *table, const char *extraEffects);
	eastl::string remapTable() const; // a copy, taken under the lock
	// Looks up the remap table: which effect, technique and material values draw a material that the caller names by
	// an original effect and technique.
	// - A passthrough name comes back as it is (out.passthrough).
	// - A known effect's unknown technique uses its '*' row, then its DEFAULT row.
	// - An effect of the tree that no row names is drawn as it is; this is logged once.
	// - Any other name gets the default row ('*') and is logged once (out.fallback).
	// The table is read on first use and again after an edit (remapChanged). It is validated against the effects
	// (names, techniques, parameters and their value counts, no duplicate rows); a table that fails keeps the rows
	// read before. false: no usable table.
	bool remap(const char *effect, const char *technique, RemapRow &out, eastl::string &error);
	// Where remap() logs unknown names. Without one it uses OutputDebugString.
	void setLog(void (*log)(const char *line, void *user), void *user)
	{
		m_log = log;
		m_logUser = user;
	}
	// For the watcher: true when the table changed since the last call, including when it now fails to read or
	// validate (error says why).
	bool remapChanged(eastl::string &error);
	// Copies everything a Prebuilt cache reads (*.desc, ABI-tagged *.dxil, aliases.txt, *.remap) into <dir>/effects
	// and writes <dir>/effects/C_DEPLOYED_EFFECTS listing the effects of the source tree. false with the file that
	// could not be copied or written.
	bool deploy(const char *dir, uint32_t &files, eastl::string &error) const;
	// A hash of the names and bytes of every compiled shader in the folder (Prebuilt: deployed effects only), for a
	// disk pipeline cache header.
	uint64_t shaderSetHash() const;
	// How many variants getShader compiled (Develop only; a Prebuilt cache never compiles, and a variant that was not
	// deployed is an error).
	uint32_t compiledCount() const
	{
		return m_compiledCount;
	}

  private:
	struct Round;
	bool updateDescription(const eastl::string &name, bool checkOnly, Round &round, BuildEntry &report,
						   EffectDesc &desc);
	bool updateVariant(const EffectDesc &desc, const TechniqueDesc &technique, const PassDesc &pass, Stage stage,
					   const SelectionValues &values, Path path, bool checkOnly, Round &round, BuildEntry &report);
	void readAliases(eastl::vector<eastl::pair<eastl::string, eastl::string>> &aliases) const;
	bool deployedEffect(const eastl::string &effect) const; // Prebuilt: whether C_DEPLOYED_EFFECTS names it
	bool loadDescription(EffectDesc &desc, const eastl::string &path, eastl::string &error) const;
	// Checks a description against the host's layout hashes (expectLayouts) and, in Develop, the registered
	// auto-constants. Appends errors and notes to messages; false when the description must not be served.
	bool acceptDescription(const EffectDesc &desc, eastl::string &messages) const;

	eastl::string			  m_root;
	eastl::vector<eastl::string> m_deployed; // Prebuilt: the names in C_DEPLOYED_EFFECTS, sorted
	eastl::string			  m_sourcePrefix, m_preludePath;
	ShaderPolicy			  m_policy;
	uint64_t				  m_policyHash = 0;
	Mode					  m_mode = Mode::Prebuilt;
	Compiler				  m_compiler;
	mutable void			 *m_lock = nullptr;	 // SRWLOCK, kept out of the header (windows.h)
	eastl::vector<BuildEntry> m_compiledOutside; // built by getShader/loadEffect, reported by the next updateAll
	const EffectLayout		 *m_layouts = nullptr;
	size_t					  m_layoutCount = 0;
	AutoConstants			 *m_autos = nullptr;
	uint32_t				  m_compiledCount = 0;
	// effects/aliases.txt, read on first use and again at the start of each updateAll. It has its own lock because
	// effectOf is also called inside the cache's locked steps.
	mutable eastl::vector<eastl::pair<eastl::string, eastl::string>> m_aliases;
	mutable bool													 m_aliasesValid = false;
	mutable void													*m_aliasLock = nullptr; // SRWLOCK
	// Remap state: the table and extra names files (setRemapTable), the rows and passthrough names last read and
	// validated, the hash of the file they came from, the hash the watcher last saw, and the unknown names already
	// logged. All guarded by m_lock.
	eastl::string				 m_remapTable, m_remapNames;
	eastl::vector<RemapRow>		 m_remap;
	eastl::vector<eastl::string> m_passthrough;
	eastl::vector<eastl::string> m_remapLogged;
	bool						 m_remapLoaded = false;
	bool						 m_remapSeenValid = false;
	uint64_t					 m_remapHash = 0, m_remapSeen = 0;
	eastl::string				 m_remapError;
	void (*m_log)(const char *line, void *user) = nullptr;
	void *m_logUser = nullptr;
	// Reads the table if it changed. retryRejected also re-validates a table that failed before, since a watcher
	// round may have repaired the effects it names.
	bool loadRemap(eastl::string &error, bool retryRejected = false);
	bool validateRemap(const eastl::vector<RemapRow> &rows, const eastl::vector<eastl::string> &passthrough,
					   const eastl::string &table, const eastl::string &extraNamesFile, eastl::string &error);
};

// <root>\effects\<effect>.<variant>.<ABI><extension> for ".dxil" and ".dep". An effect description uses variant
// "desc" without an ABI suffix.
eastl::string compiledPath(const eastl::string &root, const char *effect, const char *variant, const char *extension);

} // namespace render_backend::shader
