// Explicit resource state requests (in NVRHI resource states) and deferred bindless read tracking.
#pragma once
#include <nvrhi/nvrhi.h>
#include <EASTL/vector.h>

namespace render_backend::gpu::ctx
{
struct TextureUse
{
	nvrhi::ITexture				*texture = nullptr;
	nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources;
	nvrhi::ResourceStates		 state = nvrhi::ResourceStates::ShaderResource;
};

struct BufferUse
{
	nvrhi::IBuffer		 *buffer = nullptr;
	nvrhi::ResourceStates state = nvrhi::ResourceStates::ShaderResource;
};

struct ResourceUses
{
	const TextureUse *textures = nullptr;
	uint32_t		  textureCount = 0;
	const BufferUse	 *buffers = nullptr;
	uint32_t		  bufferCount = 0;
};

// Holds deferred bindless reads across command-list boundaries. This remembers read intent only. NVRHI still owns
// native state tracking, including transitions back to initial state and submission of permanent transitions.
// Resources whose initial state is not kept must have their starting state set as required by NVRHI.
class ResourceTransitions
{
  public:
	void reset(bool carryAcrossLists = false);
	void addRead(nvrhi::ITexture *texture, nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources);
	// Drain deferred reads and issue the explicit requests at this point only. A null resource is ignored.
	void prepare(nvrhi::ICommandList &list, ResourceUses uses = {});
	// Update read intent for a new command list without doing any native work.
	void reopen();
	// Marks deferred reads as drained without issuing barriers. Used when planning auxiliary lists.
	void consume();

  private:
	struct Read
	{
		nvrhi::TextureHandle		 texture;
		nvrhi::TextureSubresourceSet subresources;
	};
	static void append(eastl::vector<Read> &reads, nvrhi::ITexture *texture, nvrhi::TextureSubresourceSet subresources);
	eastl::vector<Read> m_pending;
	eastl::vector<Read> m_kept;
	bool				m_carryAcrossLists = false;
};
} // namespace render_backend::gpu::ctx
