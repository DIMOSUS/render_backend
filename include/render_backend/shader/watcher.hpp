// Shader hot reload for a Develop ShaderCache. A worker thread watches the source tree (ReadDirectoryChangesW).
// After a change to an effect or include it rebuilds what is stale through the cache: the descriptions, and the
// previously compiled variants whose manifests name a changed file. The renderer calls poll() once per frame, at a
// point where it may replace shaders and drop the pipelines built from them. A failed compile reports the error and
// the old shader stays in use.
#pragma once
#include <render_backend/shader/shader_cache.hpp>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <stdint.h>

namespace render_backend::shader
{
struct ShaderChange
{
	enum class Kind : uint8_t
	{
		// New bytecode of a variant (effect, technique, pass, stage, values). A variant shared by several passes is
		// reported once, under the first pass that uses it, so key the pipelines to rebuild by (effect, variant):
		// every PipelineDesc whose shaders[] names it.
		Shader,
		// The effect description changed (a parameter, selection, state ...). Reload it with ShaderCache::loadEffect,
		// carry the held selection values over by name (EffectDesc::carryOver) and rebuild the effect's pipelines.
		Description,
		// A variant failed to compile, or the description failed (variant "desc"; this includes a layout the host was
		// not built for). Sent once per distinct message; the old bytecode and description stay in use.
		Error,
		// The remap table changed (effect: the table's file stem, variant "remap"). Look the materials up again with
		// ShaderCache::remap.
		Remap,
		Cleared, // the error of effect/variant is gone (it builds, or its source is back to the compiled text)
		Aliases, // aliases.txt may have changed without changing any description or bytecode
	};
	Kind				   kind = Kind::Shader;
	eastl::string		   effect;
	eastl::string		   technique;
	eastl::string		   pass;
	Stage				   stage = Stage::Vertex;
	SelectionValues		   values;
	Path				   path = Path::Vertices; // the variant's path
	eastl::string		   entry;				  // the DXIL entry point
	eastl::string		   variant;				  // logical variant ID, "desc" for the description
	eastl::vector<uint8_t> bytecode;
	eastl::string		   message;
};

class Watcher
{
  public:
	Watcher() = default;
	Watcher(const Watcher &) = delete;
	Watcher &operator=(const Watcher &) = delete;
	~Watcher();

	// Watches cache.root(). The cache must be in Develop mode and outlive the watcher. Returns after the worker has
	// started watching (or after a 5 s timeout).
	bool start(ShaderCache &cache, eastl::string &error);
	void stop();
	bool running() const
	{
		return m_thread != nullptr;
	}
	// The changes the worker produced since the last call, in order; the caller's vector is replaced. Returns the
	// number of completed rounds, sampled under the same lock as the changes. If the worker cannot watch the folder
	// it reports one Error change and exits.
	uint32_t poll(eastl::vector<ShaderChange> &changes);
	// The number of rebuild rounds the worker has finished (useful for tests).
	uint32_t rounds() const;

  private:
	static unsigned long __stdcall threadMain(void *self);
	void						   run();
	void						   rebuild(bool aliasesChanged);

	ShaderCache				   *m_cache = nullptr;
	void					   *m_thread = nullptr;
	void					   *m_stopEvent = nullptr;
	void					   *m_readyEvent = nullptr; // set once the first ReadDirectoryChangesW is issued
	void					   *m_lock = nullptr;		// SRWLOCK around m_pending
	eastl::vector<ShaderChange> m_pending;
	// Worker thread only: the entries in error ("effect.variant") with their last message.
	eastl::vector<eastl::pair<eastl::string, eastl::string>> m_errors;
	volatile long											 m_rounds = 0;
};

} // namespace render_backend::shader
