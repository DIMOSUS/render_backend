#include <render_backend/gpu/frame_executor.hpp>
#include <render_backend/gpu/upload_queue.hpp>
#include <cassert>

#include "platform.hpp"

namespace render_backend::gpu::ctx
{
namespace
{
int64_t ticksNow()
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}

using Bounds = std::array<uint32_t, FrameExecutor::C_LISTS + 1>;

uint32_t partition(const FrameContexts &contexts, uint32_t first, uint32_t end, uint32_t passes, uint64_t commands,
				   uint32_t limit, Bounds &bounds)
{
	const uint32_t requested = passes < limit ? passes : limit;
	uint32_t	   ranges = 1;
	uint64_t	   seen = 0;
	bounds[0] = first;
	for (uint32_t i = first; i < end && ranges < requested; ++i)
	{
		const bool passStart = i != first && contexts.sorted(i).key().rank() != contexts.sorted(i - 1).key().rank();
		if (passStart && seen * requested >= commands * ranges)
			bounds[ranges++] = i;
		seen += contexts.sorted(i).commands();
	}
	bounds[ranges] = end;
	return ranges;
}
} // namespace

void ResourcePlayer::bindResourceTransitions(ResourceTransitions &transitions)
{
	m_transitions = &transitions;
}

void ResourcePlayer::prepareReads(ResourceUses uses)
{
	assert(m_transitions != nullptr);
	m_transitions->prepare(*list, uses);
}

void ResourcePlayer::written(nvrhi::ITexture *texture, nvrhi::TextureSubresourceSet subresources)
{
	assert(m_transitions != nullptr);
	m_transitions->addRead(texture, subresources);
}

void WindowPlayer::clear(const Clear &command)
{
	list->clearDepthStencilTexture(command.texture, nvrhi::AllSubresources, true, command.depth, false, 0);
}

void WindowPlayer::staged(const StagedUpload &)
{
}

void WindowPlayer::slot(uint8_t, uint8_t)
{
}

void WindowPlayer::slotsEnd()
{
}

bool FrameExecutor::setPrelude(nvrhi::ICommandList *list)
{
	if (!list)
		return true;
	if (m_prelude || list->getDesc().queueType != nvrhi::CommandQueue::Graphics)
		return false;
	m_prelude = list;
	return true;
}

uint64_t FrameExecutor::submit(nvrhi::IDevice &device, nvrhi::ICommandList *list, upload::Queue *uploads)
{
	if (!list || list == m_prelude.Get() || list->getDesc().queueType != nvrhi::CommandQueue::Graphics)
		return 0;
	nvrhi::ICommandList *lists[] = {m_prelude.Get(), list};
	const uint64_t result = uploads ? uploads->submit(list, m_prelude.Get())
		: device.executeCommandLists(m_prelude ? lists : lists + 1, m_prelude ? 2 : 1);
	if (result != 0)
		m_prelude.Reset();
	return result;
}

void FrameExecutor::discardPrelude(upload::Queue *uploads)
{
	if (uploads && m_prelude)
		uploads->discard(m_prelude.Get());
	m_prelude.Reset();
}

void FrameExecutor::reset()
{
	m_prelude.Reset();
	m_threads.stop();
	for (auto &list : m_lists)
		list.Reset();
	m_primaryTransitions.reset();
	for (ResourceTransitions &transitions : m_windowTransitions)
		transitions.reset();
	m_device = nullptr;
}

void FrameExecutor::beginFrame(bool carryAcrossLists)
{
	m_primaryTransitions.reset(carryAcrossLists);
}

void FrameExecutor::primaryReopened()
{
	m_primaryTransitions.reopen();
}

bool FrameExecutor::allocateLists(nvrhi::IDevice &device, uint32_t count)
{
	if (m_device != &device)
	{
		// Lists from another device cannot be reused. Only the auxiliary lists are reset here: this may be the first
		// split of a frame, and the primary transitions already hold this frame's read intent.
		m_threads.stop();
		for (auto &list : m_lists)
			list.Reset();
		m_device = &device;
	}
	for (uint32_t range = 0; range < count; ++range)
	{
		if (!m_lists[range])
			m_lists[range] =
				device.createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
		if (!m_lists[range])
			return false;
	}
	return true;
}

FrameExecutionResult FrameExecutor::execute(FrameContexts &contexts, const FrameExecution &frame)
{
	frame.primary.bindResourceTransitions(m_primaryTransitions);
	FrameExecutionResult result;
	const uint32_t		 limit = frame.windowPlayerCount < C_LISTS ? frame.windowPlayerCount : C_LISTS;
	if (!frame.allowSubmission || frame.minimumCommands == 0 || frame.primary.markers != nullptr || frame.windowPlayers == nullptr || limit < 2 ||
		contexts.pendingCommands() < frame.minimumCommands)
	{
		play(contexts, frame.primary);
		return result;
	}
	for (uint32_t range = 0; range < limit; ++range)
		if (frame.windowPlayers[range] == nullptr)
		{
			play(contexts, frame.primary);
			return result;
		}

	const int64_t		 start = ticksNow();
	const uint32_t		 count = contexts.sort();
	const int64_t		 replay = ticksNow();
	FrameContexts::Stats counts;
	const auto			 finish = [&]()
	{
		const int64_t end = ticksNow();
		counts.playTicks = end - start;
		counts.replayTicks = end - replay;
		contexts.addCounts(counts);
		return result;
	};
	const auto windowed = [&](uint32_t index)
	{
		const CommandContext &context = contexts.sorted(index);
		const uint32_t		  rank = context.key().rank();
		return rank != 0 && rank != ContextKey::C_DEAD_RANK && !context.bound() &&
			   (frame.canRecordInWindow == nullptr || frame.canRecordInWindow(context, frame.policyOwner));
	};

	bool	   primaryOpen = true;
	const auto reopenPrimary = [&]()
	{
		if (primaryOpen)
			return;
		frame.primary.list->open();
		primaryReopened();
		primaryOpen = true;
	};
	const auto playPrimary = [&](uint32_t first, uint32_t end)
	{
		reopenPrimary();
		contexts.playSorted(first, end, frame.primary, counts);
	};

	for (uint32_t first = 0; first < count;)
	{
		uint32_t end = first;
		while (end < count && !windowed(end))
			++end;
		if (end != first)
		{
			playPrimary(first, end);
			first = end;
			continue;
		}

		uint32_t passes = 0;
		uint64_t commands = 0;
		for (; end < count && windowed(end); ++end)
		{
			commands += contexts.sorted(end).commands();
			passes += end == first || contexts.sorted(end).key().rank() != contexts.sorted(end - 1).key().rank();
		}
		Bounds		   bounds;
		const uint32_t ranges =
			commands < frame.minimumCommands ? 1 : partition(contexts, first, end, passes, commands, limit, bounds);
		const bool listsReady = ranges > 1 && allocateLists(frame.device, ranges);
		if (!listsReady)
		{
			result.allocationFallbacks += ranges > 1 ? 1u : 0u;
			playPrimary(first, end);
			first = end;
			continue;
		}

		if (primaryOpen)
		{
			frame.primary.list->close();
			primaryOpen = false;
			// Commit preceding uploads and resource states before recording lists that may read them.
			const uint64_t submission = submit(frame.device, frame.primary.list, frame.uploads);
			if (submission == 0)
			{
				if (frame.uploads != nullptr)
					frame.uploads->discard(frame.primary.list);
				discardPrelude(frame.uploads);
				contexts.discardSorted(first, count);
				result.submissionFailure = SubmissionFailure::Primary;
				return finish();
			}
			++result.splits;
		}

		ResourceTransitions						  planned = m_primaryTransitions;
		std::array<FrameContexts::Stats, C_LISTS> listCounts{};
		for (uint32_t range = 0; range < ranges; ++range)
		{
			WindowPlayer &player = *frame.windowPlayers[range];
			player.list = m_lists[range].Get();
			player.markers = nullptr;
			planned.reopen();
			m_windowTransitions[range] = planned;
			player.bindResourceTransitions(m_windowTransitions[range]);
			player.list->open();
			contexts.followReads(bounds[range], bounds[range + 1], planned);
		}
		auto record = [&](uint32_t range)
		{ contexts.playSorted(bounds[range], bounds[range + 1], *frame.windowPlayers[range], listCounts[range]); };
		m_threads.run(ranges, record);

		std::array<nvrhi::ICommandList *, C_LISTS> submit;
		for (uint32_t range = 0; range < ranges; ++range)
		{
			WindowPlayer &player = *frame.windowPlayers[range];
			player.list->close();
			submit[range] = player.list;
			player.list = nullptr;
			m_windowTransitions[range].reset();
			contexts.addCounts(listCounts[range]);
		}
		if (frame.device.executeCommandLists(submit.data(), ranges) == 0)
		{
			contexts.discardSorted(end, count);
			result.submissionFailure = SubmissionFailure::Auxiliary;
			return finish();
		}
		m_primaryTransitions = eastl::move(planned);
		++result.windows;
		result.lists += ranges;
		first = end;
	}
	reopenPrimary();
	return finish();
}
} // namespace render_backend::gpu::ctx
