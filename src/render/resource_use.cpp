#include <render_backend/gpu/context.hpp>
#include <EASTL/algorithm.h>

namespace render_backend::gpu::ctx
{
namespace
{
bool initialStateIsReadable(const nvrhi::ITexture &texture)
{
	const nvrhi::TextureDesc &desc = texture.getDesc();
	return desc.keepInitialState && desc.initialState == nvrhi::ResourceStates::ShaderResource;
}

template <class Target> bool recordUses(Target &target, ResourceUses uses)
{
	const uint32_t bytes = resourceUseCommandBytes(uses.textureCount, uses.bufferCount);
	if (bytes == 0 || (uses.textureCount != 0 && uses.textures == nullptr) ||
		(uses.bufferCount != 0 && uses.buffers == nullptr))
		return false;
	State *command = target.template add<State>(bytes - uint32_t(sizeof(State)));
	if (command == nullptr)
		return false;
	command->textureCount = uses.textureCount;
	command->bufferCount = uses.bufferCount;
	auto *data = reinterpret_cast<uint8_t *>(command + 1);
	for (uint32_t i = 0; i < uses.textureCount; ++i, data += sizeof(TextureUse))
	{
		const TextureUse &use = uses.textures[i];
		if (use.texture != nullptr)
			use.texture->AddRef();
		::new (data) TextureUse(use);
	}
	for (uint32_t i = 0; i < uses.bufferCount; ++i, data += sizeof(BufferUse))
	{
		const BufferUse &use = uses.buffers[i];
		if (use.buffer != nullptr)
			use.buffer->AddRef();
		::new (data) BufferUse(use);
	}
	return true;
}
} // namespace

ResourceUses State::uses() const
{
	static_assert(alignof(TextureUse) <= 8 && alignof(BufferUse) <= 8);
	const auto *data = reinterpret_cast<const uint8_t *>(this + 1);
	return {textureCount == 0 ? nullptr : reinterpret_cast<const TextureUse *>(data), textureCount,
			bufferCount == 0 ? nullptr : reinterpret_cast<const BufferUse *>(data + textureCount * sizeof(TextureUse)),
			bufferCount};
}

bool recordResourceUses(CommandContext &context, ResourceUses uses)
{
	return recordUses(context, uses);
}

bool recordResourceUses(Recorder &recorder, ResourceUses uses)
{
	return recordUses(recorder, uses);
}

void ResourceTransitions::reset(bool carryAcrossLists)
{
	m_pending.clear();
	m_kept.clear();
	m_carryAcrossLists = carryAcrossLists;
}

void ResourceTransitions::append(eastl::vector<Read> &reads, nvrhi::ITexture *texture,
								 nvrhi::TextureSubresourceSet subresources)
{
	for (const Read &read : reads)
		if (read.texture.Get() == texture && read.subresources == subresources)
			return;
	reads.push_back(Read{nvrhi::TextureHandle(texture), subresources});
}

void ResourceTransitions::addRead(nvrhi::ITexture *texture, nvrhi::TextureSubresourceSet subresources)
{
	if (texture != nullptr)
		m_pending.push_back(Read{nvrhi::TextureHandle(texture), subresources});
}

void ResourceTransitions::prepare(nvrhi::ICommandList &list, ResourceUses uses)
{
	for (const Read &read : m_pending)
		list.setTextureState(read.texture, read.subresources, nvrhi::ResourceStates::ShaderResource);
	for (uint32_t i = 0; uses.textures != nullptr && i < uses.textureCount; ++i)
	{
		const TextureUse &use = uses.textures[i];
		if (use.texture != nullptr)
			list.setTextureState(use.texture, use.subresources, use.state);
	}
	for (uint32_t i = 0; uses.buffers != nullptr && i < uses.bufferCount; ++i)
	{
		const BufferUse &use = uses.buffers[i];
		if (use.buffer != nullptr)
			list.setBufferState(use.buffer, use.state);
	}
	list.commitBarriers();
	consume();
}

void ResourceTransitions::reopen()
{
	m_pending.erase(eastl::remove_if(m_pending.begin(), m_pending.end(),
									 [](const Read &read) { return initialStateIsReadable(*read.texture.Get()); }),
					m_pending.end());
	for (const Read &read : m_kept)
		append(m_pending, read.texture, read.subresources);
}

void ResourceTransitions::consume()
{
	if (m_carryAcrossLists)
		for (const Read &read : m_pending)
			if (!initialStateIsReadable(*read.texture.Get()))
				append(m_kept, read.texture, read.subresources);
	m_pending.clear();
}
} // namespace render_backend::gpu::ctx
