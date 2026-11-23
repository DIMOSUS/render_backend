// Playback of a published batch of contexts, sorted by key, into an NVRHI command list. Marker commands go to the
// marker stack (markers.hpp), which opens and closes ranges in the list and runs their timers. Callbacks are called
// with the list. Commands that need host state go to the Player.
#include <render_backend/gpu/context.hpp>
#include <render_backend/gpu/markers.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/sort.h>
#include <windows.h>

using namespace render_backend::gpu;
using namespace render_backend::gpu::ctx;

static_assert(sizeof(PlayState::levels) / sizeof(uint32_t) == markers::Stack::C_DEPTH, "PlayState::levels");

namespace
{
int64_t ticksNow()
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}

void releaseUses(ResourceUses uses)
{
	for (uint32_t i = 0; i < uses.textureCount; ++i)
		if (nvrhi::ITexture *texture = uses.textures[i].texture)
			texture->Release();
	for (uint32_t i = 0; i < uses.bufferCount; ++i)
		if (nvrhi::IBuffer *buffer = uses.buffers[i].buffer)
			buffer->Release();
}

void releasePipelineBuffers(const SetPipeline &state)
{
	if (state.vertices != nullptr)
		state.vertices->Release();
	if (state.indices != nullptr)
		state.indices->Release();
	if (state.instances != nullptr)
		state.instances->Release();
}
} // namespace

void render_backend::gpu::ctx::discard(FrameContexts &frame)
{
	uint32_t index = frame.m_published.exchange(CommandContext::C_NONE, std::memory_order_acquire);
	while (index != CommandContext::C_NONE)
	{
		const CommandContext &context = frame.m_pool[index];
		frame.release(context);
		index = context.m_nextPublished;
	}
}

void FrameContexts::release(const CommandContext &c)
{
	for (uint32_t run = c.m_firstRun; run != CommandContext::C_NONE; run = header(run).next)
	{
		const RunHeader &h = header(run);
		for (uint32_t offset = uint32_t(sizeof(RunHeader)); offset < h.used;)
		{
			const uint8_t		*p = at(run, offset);
			const CommandHeader &command = *reinterpret_cast<const CommandHeader *>(p);
			offset += command.size;
			switch (Command(command.type))
			{
			case Command::Application:
				if (nvrhi::IResource *resource = reinterpret_cast<const ApplicationCommand *>(p)->resource)
					resource->Release();
				break;
			case Command::Clear:
				reinterpret_cast<const Clear *>(p)->texture->Release();
				break;
			case Command::BeginPass:
				reinterpret_cast<const BeginPass *>(p)->framebuffer->Release();
				break;
			case Command::SetPipeline:
				releasePipelineBuffers(*reinterpret_cast<const SetPipeline *>(p));
				break;
			case Command::ClearTexture:
				reinterpret_cast<const ClearTexture *>(p)->texture->Release();
				break;
			case Command::Copy:
				reinterpret_cast<const Copy *>(p)->target->Release();
				reinterpret_cast<const Copy *>(p)->source->Release();
				break;
			case Command::State:
				releaseUses(reinterpret_cast<const State *>(p)->uses());
				break;
			case Command::Written:
				if (nvrhi::ITexture *texture = reinterpret_cast<const Written *>(p)->texture)
					texture->Release();
				break;
			default:
				break;
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
bool render_backend::gpu::ctx::playCommand(const uint8_t *p, PlayState &st, Player &player)
{
	const Command type = Command(reinterpret_cast<const CommandHeader *>(p)->type);
	switch (type)
	{
	case Command::MarkerPush:
	{
		const MarkerPush &m = *reinterpret_cast<const MarkerPush *>(p);
		if (player.markers == nullptr)
			break;
		const uint32_t level =
			player.markers->push(markers::Kind(m.kind), reinterpret_cast<const char *>(p + sizeof m));
		if (st.open < markers::Stack::C_DEPTH)
			st.levels[st.open++] = level;
		break;
	}
	case Command::MarkerPop:
		if (player.markers != nullptr && st.open != 0)
			player.markers->popTo(st.levels[--st.open]);
		break;
	case Command::Callback:
	{
		st.computeBound = false;
		const Callback &b = *reinterpret_cast<const Callback *>(p);
		b.run(b.owner, player.list, p + sizeof b, b.valueBytes);
		break;
	}
	case Command::Application:
	{
		st.computeBound = false;
		const auto &command = *reinterpret_cast<const ApplicationCommand *>(p);
		player.application(command, p + sizeof command);
		if (command.resource != nullptr)
			command.resource->Release();
		break;
	}
	case Command::Clear:
		st.computeBound = false;
		player.clear(*reinterpret_cast<const Clear *>(p));
		reinterpret_cast<const Clear *>(p)->texture->Release();
		break;
	case Command::BeginPass:
	{
		st.computeBound = false;
		const BeginPass &b = *reinterpret_cast<const BeginPass *>(p);
		if (st.pass != nullptr)
			st.pass->Release();
		st.pass = b.framebuffer;
		st.viewport =
			nvrhi::Viewport(b.viewport[0], b.viewport[1], b.viewport[2], b.viewport[3], b.viewport[4], b.viewport[5]);
		break;
	}
	case Command::EndPass:
		if (st.pass != nullptr)
			st.pass->Release();
		st.pass = nullptr;
		break;
	case Command::State:
	{
		const ResourceUses uses = reinterpret_cast<const State *>(p)->uses();
		player.prepareReads(uses);
		releaseUses(uses);
		break;
	}
	case Command::SetPipeline:
	{
		const SetPipeline &s = *reinterpret_cast<const SetPipeline *>(p);
		st.computeBound = false;
		const bool compute = s.kind == PipelineKind::Compute;
		if (s.pipeline == nullptr || (compute ? st.pass != nullptr : st.pass == nullptr))
		{
			releasePipelineBuffers(s);
			return false;
		}
		if (compute)
		{
			nvrhi::ComputeState state;
			state.setPipeline(static_cast<nvrhi::IComputePipeline *>(s.pipeline));
			for (nvrhi::IBindingSet *set : s.sets)
				if (set != nullptr)
					state.addBindingSet(set);
			player.list->setComputeState(state);
			st.computeBound = true;
		}
		else if (s.kind == PipelineKind::Meshlet)
		{
			nvrhi::MeshletState state;
			state.setPipeline(static_cast<nvrhi::IMeshletPipeline *>(s.pipeline))
				.setFramebuffer(st.pass)
				.setViewport(nvrhi::ViewportState().addViewport(st.viewport).addScissorRect(nvrhi::Rect(st.viewport)));
			for (nvrhi::IBindingSet *set : s.sets)
				if (set != nullptr)
					state.addBindingSet(set);
			player.list->setMeshletState(state);
		}
		else
		{
			nvrhi::GraphicsState state;
			state.setPipeline(static_cast<nvrhi::IGraphicsPipeline *>(s.pipeline))
				.setFramebuffer(st.pass)
				.setViewport(nvrhi::ViewportState().addViewport(st.viewport).addScissorRect(nvrhi::Rect(st.viewport)));
			for (nvrhi::IBindingSet *set : s.sets)
				if (set != nullptr)
					state.addBindingSet(set);
			if (s.vertices != nullptr)
			{
				state.addVertexBuffer(nvrhi::VertexBufferBinding{s.vertices, 0, s.vertexOffset});
				if (s.instanced != 0)
					state.addVertexBuffer(nvrhi::VertexBufferBinding{s.instances != nullptr ? s.instances : s.vertices,
																	 1, s.instanceOffset});
			}
			if (s.indices != nullptr)
				state.setIndexBuffer(nvrhi::IndexBufferBinding{s.indices, nvrhi::Format(s.indexFormat), 0});
			player.list->setGraphicsState(state);
		}
		// The command list holds its own references from here. Release the recorder's references on every path, as
		// FrameContexts::release does for discarded commands.
		releasePipelineBuffers(s);
		break;
	}
	case Command::PushConstants:
		if (st.pass == nullptr && !st.computeBound)
			return false;
		player.list->setPushConstants(p + sizeof(PushConstants), reinterpret_cast<const PushConstants *>(p)->bytes);
		break;
	case Command::Draw:
	{
		if (st.pass == nullptr)
			return false;
		const Draw			&d = *reinterpret_cast<const Draw *>(p);
		nvrhi::DrawArguments args;
		args.setVertexCount(d.vertexCount)
			.setInstanceCount(d.instanceCount)
			.setStartIndexLocation(d.startIndex)
			.setStartVertexLocation(d.startVertex);
		if (d.indexed != 0)
			player.list->drawIndexed(args);
		else
			player.list->draw(args);
		break;
	}
	case Command::DispatchMesh:
		if (st.pass == nullptr)
			return false;
		player.list->dispatchMesh(reinterpret_cast<const DispatchMesh *>(p)->groups);
		break;
	case Command::Dispatch:
	{
		if (!st.computeBound)
			return false;
		const Dispatch &dispatch = *reinterpret_cast<const Dispatch *>(p);
		player.list->dispatch(dispatch.groupsX, dispatch.groupsY, dispatch.groupsZ);
		break;
	}
	case Command::WriteBuffer:
	{
		st.computeBound = false;
		const WriteBuffer &w = *reinterpret_cast<const WriteBuffer *>(p);
		player.list->writeBuffer(w.buffer, p + sizeof w, w.bytes, w.offset);
		break;
	}
	case Command::StagedUpload:
		st.computeBound = false;
		player.staged(*reinterpret_cast<const StagedUpload *>(p));
		break;
	// The recorder's references are released once the command list holds its own.
	case Command::ClearTexture:
	{
		st.computeBound = false;
		const ClearTexture &c = *reinterpret_cast<const ClearTexture *>(p);
		if (c.depth != 0)
			player.list->clearDepthStencilTexture(c.texture, c.subresources, true, c.value[0], false, 0);
		else
			player.list->clearTextureFloat(c.texture, c.subresources,
										   nvrhi::Color(c.value[0], c.value[1], c.value[2], c.value[3]));
		c.texture->Release();
		break;
	}
	case Command::Copy:
	{
		st.computeBound = false;
		const Copy &c = *reinterpret_cast<const Copy *>(p);
		player.list->copyTexture(c.target, c.to, c.source, c.from);
		c.target->Release();
		c.source->Release();
		break;
	}
	case Command::Written:
	{
		const Written &w = *reinterpret_cast<const Written *>(p);
		if (w.texture != nullptr)
		{
			player.written(w.texture, w.subresources);
			w.texture->Release();
		}
		break;
	}
	case Command::Count:
		break;
	}
	return true;
}

void render_backend::gpu::ctx::endPlay(PlayState &st, Player &player)
{
	if (st.pass != nullptr) // Defensive: recorders normally close every pass they open.
		st.pass->Release();
	if (st.open != 0 && player.markers != nullptr)
		player.markers->popTo(st.levels[0]);
	st = PlayState{};
}

//----------------------------------------------------------------------------------------------------------------------
uint64_t FrameContexts::pendingCommands() const
{
	uint64_t commands = 0;
	for (uint32_t i = m_published.load(std::memory_order_acquire); i != CommandContext::C_NONE;
		 i = m_pool[i].m_nextPublished)
		commands += m_pool[i].m_commands;
	return commands;
}

uint32_t FrameContexts::sort()
{
	const uint32_t first = m_published.exchange(CommandContext::C_NONE, std::memory_order_acquire);
	if (first == CommandContext::C_NONE)
		return 0;
	const int64_t sortStart = ticksNow();
	uint32_t n = 0;
	for (uint32_t i = first; i != CommandContext::C_NONE; i = m_pool[i].m_nextPublished)
		m_order[n++] = i;
	// Sort by key. Equal keys keep allocation order (pool index).
	eastl::sort(m_order, m_order + n,
				[this](uint32_t a, uint32_t b)
				{
					const uint64_t ka = m_pool[a].m_key.value, kb = m_pool[b].m_key.value;
					return ka != kb ? ka < kb : a < b;
				});
	for (uint32_t i = 1; i < n; ++i)
	{
		const ContextKey k = m_pool[m_order[i]].m_key;
		m_counts.ties += k.value == m_pool[m_order[i - 1]].m_key.value ? 1u : 0u;
		m_counts.uploadNotFirst += k.order() == 0 ? 1u : 0u;
	}
	m_counts.sortTicks += ticksNow() - sortStart;
	return n;
}

void FrameContexts::playSorted(uint32_t from, uint32_t to, Player &player, Stats &counts)
{
	// Measures the time spent in upload contexts and in passes, with two counter reads per context or pass.
	for (uint32_t n = from; n < to; ++n)
	{
		const CommandContext &c = sorted(n);
		if (c.m_key.rank() == ContextKey::C_DEAD_RANK) // Culled pass: sorted last and discarded unplayed.
		{
			release(c);
			++counts.dead;
			continue;
		}
		const bool	  inPlace = c.m_key.order() != 0; // Not an upload context.
		const int64_t contextStart = inPlace ? 0 : ticksNow();
		int64_t		  passStart = 0;
		if (m_tags[0].counts != nullptr) // make() bounds both tags; contexts with one tag may play on several threads.
		{
			TagCounts::Count &count = m_tags[0].counts[c.m_view * m_passTags + c.m_pass];
			InterlockedIncrement16(reinterpret_cast<volatile SHORT *>(&count.contexts));
			InterlockedExchangeAdd(reinterpret_cast<volatile LONG *>(&count.commands), LONG(c.m_commands));
		}
		if (player.markers != nullptr) // Lets the player switch its view and pass marker ranges.
			player.slot(c.m_view, c.m_pass);
		PlayState st; // Per context: endPlay closes whatever the context left open.
		for (uint32_t run = c.m_firstRun; run != CommandContext::C_NONE; run = header(run).next)
		{
			const RunHeader &h = header(run);
			for (uint32_t offset = uint32_t(sizeof(RunHeader)); offset < h.used;)
			{
				const uint8_t		*p = at(run, offset);
				const CommandHeader &command = *reinterpret_cast<const CommandHeader *>(p);
				const Command		 type = Command(command.type);
				offset += command.size;
				counts.bytes += command.size;
				++counts.commands[uint32_t(type) < uint32_t(Command::Count) ? uint32_t(type) : 0];
				if (inPlace)
				{
					counts.inPlaceUploads += type == Command::StagedUpload || type == Command::ClearTexture ? 1u : 0u;
					counts.inPlaceWrites += type == Command::WriteBuffer ? 1u : 0u;
				}
				if (type == Command::BeginPass)
					passStart = ticksNow();
				if (!playCommand(p, st, player))
					dropped(m_droppedCommands);
				if (type == Command::EndPass && passStart != 0)
				{
					counts.passTicks += ticksNow() - passStart;
					passStart = 0;
				}
			}
		}
		endPlay(st, player);
		if (!inPlace)
			counts.uploadTicks += ticksNow() - contextStart;
	}
}

void FrameContexts::discardSorted(uint32_t from, uint32_t to)
{
	for (uint32_t n = from; n < to; ++n)
		release(sorted(n));
}

void FrameContexts::addCounts(const Stats &counts)
{
	m_counts.bytes += counts.bytes;
	for (uint32_t i = 0; i < uint32_t(Command::Count); ++i)
		m_counts.commands[i] += counts.commands[i];
	m_counts.dead += counts.dead;
	m_counts.inPlaceUploads += counts.inPlaceUploads;
	m_counts.inPlaceWrites += counts.inPlaceWrites;
	m_counts.playTicks += counts.playTicks;
	m_counts.uploadTicks += counts.uploadTicks;
	m_counts.passTicks += counts.passTicks;
	m_counts.replayTicks += counts.replayTicks;
}

void FrameContexts::followReads(uint32_t from, uint32_t to, ResourceTransitions &transitions)
{
	for (uint32_t n = from; n < to; ++n)
	{
		const CommandContext &c = sorted(n);
		if (c.m_key.rank() == ContextKey::C_DEAD_RANK)
			continue;
		for (uint32_t run = c.m_firstRun; run != CommandContext::C_NONE; run = header(run).next)
		{
			const RunHeader &h = header(run);
			for (uint32_t offset = uint32_t(sizeof(RunHeader)); offset < h.used;)
			{
				const uint8_t		*p = at(run, offset);
				const CommandHeader &command = *reinterpret_cast<const CommandHeader *>(p);
				offset += command.size;
				if (Command(command.type) == Command::Written)
				{
					const auto &written = *reinterpret_cast<const Written *>(p);
					transitions.addRead(written.texture, written.subresources);
					continue;
				}
				if (Command(command.type) != Command::State &&
					(Command(command.type) != Command::Application ||
					 !reinterpret_cast<const ApplicationCommand *>(p)->preparesReads))
					continue;
				transitions.consume();
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
void render_backend::gpu::ctx::play(FrameContexts &frame, Player &player)
{
	const uint32_t n = frame.sort();
	if (n == 0)
		return;
	const int64_t playStart = ticksNow();
	frame.playSorted(0, n, player, frame.m_counts);
	if (player.markers != nullptr)
		player.slotsEnd();
	const int64_t elapsed = ticksNow() - playStart;
	frame.m_counts.playTicks += elapsed;
	frame.m_counts.replayTicks += elapsed;
}

//----------------------------------------------------------------------------------------------------------------------
void PlayThreads::dispatch(uint32_t count)
{
	if (!m_started)
	{
		m_started = true;
		m_wake = CreateSemaphoreA(nullptr, 0, C_THREADS, nullptr);
		m_done = CreateSemaphoreA(nullptr, 0, C_THREADS, nullptr);
		for (std::thread &t : m_threads)
			t = std::thread([this] { loop(); });
	}
	m_count = count;
	m_next.store(0, std::memory_order_release);
	// Wake one worker per job beyond the caller's own, up to C_THREADS. Each releases `m_done` once after its share.
	const uint32_t woken = count > C_THREADS ? C_THREADS : (count > 0 ? count - 1 : 0);
	if (woken != 0)
		ReleaseSemaphore(m_wake, LONG(woken), nullptr);
	work();
	// Wait until every woken worker is idle, so none still reads m_next or m_count when the next dispatch resets them.
	for (uint32_t i = 0; i < woken; ++i)
		WaitForSingleObject(m_done, INFINITE);
}

void PlayThreads::work()
{
	for (uint32_t i = m_next.fetch_add(1, std::memory_order_acq_rel); i < m_count;
		 i = m_next.fetch_add(1, std::memory_order_acq_rel))
		m_call(m_job, i);
}

void PlayThreads::loop()
{
	for (;;)
	{
		WaitForSingleObject(m_wake, INFINITE);
		if (m_stop.load(std::memory_order_acquire))
			return;
		work();
		ReleaseSemaphore(m_done, 1, nullptr);
	}
}

void PlayThreads::stop()
{
	if (!m_started)
		return;
	m_stop.store(true, std::memory_order_release);
	ReleaseSemaphore(m_wake, LONG(C_THREADS), nullptr);
	for (std::thread &t : m_threads)
		t.join();
	CloseHandle(m_wake);
	CloseHandle(m_done);
	m_wake = m_done = nullptr;
	m_started = false;
	m_stop.store(false, std::memory_order_relaxed);
}
