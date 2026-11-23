#include <render_backend/gpu/scatter_upload.hpp>
#include <algorithm>
#include <cstring>

#include "scatter_upload_dxil.hpp"

namespace render_backend::gpu::upload
{
namespace
{
constexpr uint32_t C_THREADS = 64, C_MAX_GROUPS = 65535;
constexpr uint32_t C_OFFSET_MASK = 0x0ffffffcu;
struct Constants
{
	uint32_t sourceIndex, targetsBase, wordCount, rowWidth;
};
}

bool ScatterBatch::create(nvrhi::IDevice *device, const Target *targets, uint32_t targetCount, uint32_t frameSlots)
{
	if (m_device || !device || !targets || targetCount == 0 || targetCount > C_MAX_TARGETS || frameSlots == 0)
		return false;
	for (uint32_t i = 0; i < targetCount; ++i)
		if (!targets[i].buffer || !targets[i].buffer->getDesc().canHaveRawViews || !targets[i].buffer->getDesc().canHaveUAVs)
			return false;

	m_device = device;
	m_targets.assign(targets, targets + targetCount);
	m_sources.resize(frameSlots);
	m_constantsLayout = device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
		.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants))));
	m_heapLayout = device->createBindlessLayout(nvrhi::BindlessLayoutDesc().setVisibility(nvrhi::ShaderType::Compute)
		.setMaxCapacity(targetCount + frameSlots).setLayoutType(nvrhi::BindlessLayoutDesc::LayoutType::MutableSrvUavCbv));
	const auto shader = device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Compute)
		.setEntryName("ScatterUpload").setDebugName("scatter buffer upload"), c_scatterUploadDxil, sizeof c_scatterUploadDxil);
	if (m_constantsLayout && m_heapLayout && shader)
	{
		m_constants = device->createBindingSet(nvrhi::BindingSetDesc()
			.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants))), m_constantsLayout);
		m_heap = device->createDescriptorTable(m_heapLayout);
		if (m_constants && m_heap)
		{
			device->resizeDescriptorTable(m_heap, targetCount + frameSlots, false);
			for (uint32_t i = 0; i < targetCount; ++i)
				device->writeDescriptorTable(m_heap, nvrhi::BindingSetItem::RawBuffer_UAV(i, targets[i].buffer));
			m_pipeline = device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(shader)
				.addBindingLayout(m_constantsLayout).addBindingLayout(m_heapLayout));
		}
	}
	return true;
}

bool ScatterBatch::append(uint32_t target, uint64_t offset, const void *data, size_t bytes)
{
	if (m_invalid)
		return false;
	const uint64_t padded = (uint64_t(bytes) + 3) & ~uint64_t(3);
	if (target >= m_targets.size() || (offset & 3) != 0 || (bytes != 0 && data == nullptr) ||
		bytes > C_MAX_TARGET_BYTES || offset > C_MAX_TARGET_BYTES - padded ||
		offset + padded > m_targets[target].buffer->getDesc().byteSize)
	{
		m_invalid = true;
		return false;
	}
	const auto *input = static_cast<const uint8_t *>(data);
	const uint32_t targetBits = target << 28;
	for (size_t at = 0; at < bytes; at += 16)
	{
		const size_t count = std::min(bytes - at, size_t(16));
		Word &word = m_words.push_back();
		word.destination = targetBits | uint32_t(offset + at) | uint32_t((count - 1) / 4);
		if (count != sizeof word.data)
			std::memset(word.data, 0, sizeof word.data);
		std::memcpy(word.data, input + at, count);
	}
	if (bytes != 0)
		m_writtenTargets |= 1u << target;
	return true;
}

bool ScatterBatch::reserveSource(uint32_t frameSlot, uint64_t bytes)
{
	Source &source = m_sources[frameSlot];
	if (source.capacity >= bytes)
		return true;
	const uint64_t capacity = (bytes + 65535) & ~uint64_t(65535);
	auto buffer = m_device->createBuffer(nvrhi::BufferDesc().setByteSize(capacity)
		.setCpuAccess(nvrhi::CpuAccessMode::Write).setCanHaveRawViews(true)
		.setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true).setDebugName("scatter upload data"));
	auto *mapped = buffer ? static_cast<uint8_t *>(m_device->mapBuffer(buffer, nvrhi::CpuAccessMode::Write)) : nullptr;
	if (!mapped)
		return false;
	source.buffer = buffer;
	source.mapped = mapped;
	source.capacity = capacity;
	m_device->writeDescriptorTable(m_heap,
		nvrhi::BindingSetItem::RawBuffer_SRV(uint32_t(m_targets.size()) + frameSlot, source.buffer));
	return true;
}

bool ScatterBatch::record(uint32_t frameSlot, const char *marker, nvrhi::CommandListHandle &list)
{
	list = nullptr;
	if (frameSlot >= m_sources.size())
		return false;
	if (m_invalid)
	{
		m_invalid = false;
		m_words.clear();
		m_writtenTargets = 0;
		return false;
	}
	if (m_words.empty())
		return true;
	const uint64_t bytes = uint64_t(m_words.size()) * sizeof(Word);
	auto &commands = m_sources[frameSlot].commands;
	if (!commands)
		commands = m_device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
	if (!commands)
		return false;
	commands->open();
	if (marker)
		commands->beginMarker(marker);
	if (m_pipeline && bytes <= UINT32_MAX && reserveSource(frameSlot, bytes))
	{
		std::memcpy(m_sources[frameSlot].mapped, m_words.data(), size_t(bytes));
		for (uint32_t i = 0; i < m_targets.size(); ++i)
			if (m_writtenTargets & (1u << i))
				commands->setBufferState(m_targets[i].buffer, nvrhi::ResourceStates::UnorderedAccess);
		commands->commitBarriers();
		commands->setComputeState(nvrhi::ComputeState().setPipeline(m_pipeline).addBindingSet(m_constants).addBindingSet(m_heap));
		const uint32_t base = m_heap->getFirstDescriptorIndexInHeap();
		const uint32_t words = uint32_t(m_words.size());
		const Constants constants{base + uint32_t(m_targets.size()) + frameSlot, base, words, C_MAX_GROUPS * C_THREADS};
		commands->setPushConstants(&constants, sizeof constants);
		const uint32_t groups = (words + C_THREADS - 1) / C_THREADS;
		commands->dispatch(std::min(groups, C_MAX_GROUPS), (groups + C_MAX_GROUPS - 1) / C_MAX_GROUPS, 1);
	}
	else
	{
		// Fallback without the compute pipeline. Merge adjacent words so a large update does not become one copy per
		// 16 bytes.
		eastl::vector<uint8_t> run(65536);
		size_t i = 0;
		while (i < m_words.size())
		{
			const uint32_t first = m_words[i].destination;
			const uint32_t target = first >> 28;
			const uint32_t offset = first & C_OFFSET_MASK;
			uint32_t size = 0;
			for (; i < m_words.size(); ++i)
			{
				const Word &word = m_words[i];
				const uint32_t wordBytes = ((word.destination & 3) + 1) * 4;
				if ((word.destination >> 28) != target || (word.destination & C_OFFSET_MASK) != offset + size ||
					size + wordBytes > run.size())
					break;
				std::memcpy(run.data() + size, word.data, wordBytes);
				size += wordBytes;
			}
			commands->writeBuffer(m_targets[target].buffer, run.data(), size, offset);
		}
	}
	for (uint32_t i = 0; i < m_targets.size(); ++i)
		if (m_writtenTargets & (1u << i))
			commands->setBufferState(m_targets[i].buffer, m_targets[i].after);
	commands->commitBarriers();
	if (marker)
		commands->endMarker();
	commands->close();
	m_words.clear();
	m_writtenTargets = 0;
	list = commands;
	return true;
}
} // namespace render_backend::gpu::upload
