// Batches many small, scattered buffer updates into a single compute dispatch.
#pragma once

#include <EASTL/vector.h>
#include <nvrhi/nvrhi.h>

namespace render_backend::gpu::upload
{
// Collects disjoint CPU writes into up to C_MAX_TARGETS raw UAV buffers and records them into one command list,
// scattered by a compute shader. If the compute path is unavailable, falls back to writeBuffer calls that merge
// adjacent words. Each frame slot owns its staging buffer and command list; record a slot again only after the GPU has
// finished the previous list recorded for it.
class ScatterBatch
{
public:
	struct Target
	{
		nvrhi::BufferHandle buffer;
		nvrhi::ResourceStates after = nvrhi::ResourceStates::ShaderResource;
	};
	static constexpr uint32_t C_MAX_TARGETS = 16;
	static constexpr uint64_t C_MAX_TARGET_BYTES = 1ull << 28;

	ScatterBatch() = default;
	ScatterBatch(const ScatterBatch &) = delete;
	ScatterBatch &operator=(const ScatterBatch &) = delete;

	bool create(nvrhi::IDevice *device, const Target *targets, uint32_t targetCount, uint32_t frameSlots);
	// `offset` must be a multiple of 4. A partial final DWORD is zero-padded, and the padding is written too. An
	// invalid append invalidates the whole batch.
	bool append(uint32_t target, uint64_t offset, const void *data, size_t bytes);
	// Records the batch into the frame slot's command list and returns it closed in `list`. On success a null `list`
	// means there were no updates. The caller is responsible for ordering and submission. An invalidated batch is
	// discarded and record() returns false; the next batch starts clean.
	bool record(uint32_t frameSlot, const char *marker, nvrhi::CommandListHandle &list);

private:
	// Bits 31..28: target index. Bits 27..2: byte offset. Bits 1..0: DWORD count minus one.
	struct Word
	{
		uint32_t destination;
		uint8_t data[16];
	};
	static_assert(sizeof(Word) == 20, "scatter upload shader layout");
	struct Source
	{
		nvrhi::BufferHandle buffer;
		uint8_t *mapped = nullptr;
		uint64_t capacity = 0;
		nvrhi::CommandListHandle commands;
	};
	bool reserveSource(uint32_t frameSlot, uint64_t bytes);

	nvrhi::IDevice *m_device = nullptr;
	eastl::vector<Target> m_targets;
	eastl::vector<Source> m_sources;
	eastl::vector<Word> m_words;
	uint32_t m_writtenTargets = 0;
	bool m_invalid = false;
	nvrhi::BindingLayoutHandle m_constantsLayout, m_heapLayout;
	nvrhi::BindingSetHandle m_constants;
	nvrhi::DescriptorTableHandle m_heap;
	nvrhi::ComputePipelineHandle m_pipeline;
};
} // namespace render_backend::gpu::upload
