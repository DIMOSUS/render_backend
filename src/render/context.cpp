// Command context pool and arena.
#include <render_backend/gpu/context.hpp>
#include <render_backend/gpu/diagnostics.hpp>
#include <render_backend/gpu/markers.hpp>
#include <render_backend/gpu/memory.hpp>
#include <stdarg.h>
#include <stdio.h>

#include "platform.hpp"

using namespace render_backend::gpu::ctx;

namespace
{
// Estimated playback cost used to balance work across command lists. A callback usually records a whole pass, so it
// counts as ten draw-sized commands.
uint32_t weightOf(Command type)
{
	return type == Command::Callback ? 10u : 1u;
}
} // namespace

const char *render_backend::gpu::ctx::commandName(Command c)
{
	static const char *const C_NAMES[uint32_t(Command::Count)] = {
		"markerPush",  "markerPop",	   "callback",	   "application",	"clear",   "beginPass",
		"endPass",	   "state",		   "setPipeline",  "pushConstants", "draw",	   "dispatchMesh",
		"writeBuffer", "stagedUpload", "clearTexture", "copy",			"written", "dispatch"};
	return uint32_t(c) < uint32_t(Command::Count) ? C_NAMES[uint32_t(c)] : "?";
}

static_assert(C_MARKER_NAME == render_backend::gpu::markers::Stack::C_NAME, "C_MARKER_NAME");

void render_backend::gpu::ctx::markerPushf(Recorder &r, uint8_t kind, const char *format, ...)
{
	char	name[C_MARKER_NAME];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(name, sizeof name, _TRUNCATE, format, args);
	va_end(args);
	markerPushTo(r, kind, name);
}

void *Recorder::beginDirectCommand(uint32_t size)
{
	DirectRecording &direct = *m_direct;
	if (direct.completion != DirectRecording::Completion::Open)
		return nullptr;
	while (Recorder *pending = direct.player.m_pendingDirect)
		pending->playDirectCommand();
	// A playback callback may have requested that this unit finish.
	if (direct.completion != DirectRecording::Completion::Open)
		return nullptr;
	const Recorder *bound = direct.player.m_boundDirect;
	if (bound != nullptr && bound != this && bound->m_direct->hasOpenState())
	{
		diag::report(true, "command recorder", "Direct recording would interrupt an open pass, compute binding or marker");
		return nullptr;
	}

	direct.command.swap(direct.scratch);
	if (direct.command.size() < size)
		direct.command.resize(size);
	direct.player.m_pendingDirect = this;
	return direct.command.data();
}

void Recorder::playDirectCommand()
{
	DirectRecording &direct = *m_direct;
	direct.player.m_pendingDirect = nullptr;
	// Playback callbacks can record again. Keep the current payload alive while they use the scratch cache.
	eastl::vector<uint8_t> command;
	command.swap(direct.command);
	const bool wasPlaying = direct.playing;
	direct.playing = true;
	playCommand(command.data(), direct.state, direct.player);
	direct.playing = wasPlaying;
	if (command.capacity() > direct.scratch.capacity())
		direct.scratch.swap(command);
	if (direct.hasOpenState())
		direct.player.m_boundDirect = this;
	else if (direct.player.m_boundDirect == this)
		direct.player.m_boundDirect = nullptr;
	if (!wasPlaying && direct.completion == DirectRecording::Completion::Requested)
		closeDirect();
}

void Recorder::finishDirect()
{
	DirectRecording &direct = *m_direct;
	if (direct.completion != DirectRecording::Completion::Open)
		return;
	if (direct.playing)
	{
		direct.completion = DirectRecording::Completion::Requested;
		return;
	}
	closeDirect();
}

void Recorder::closeDirect()
{
	DirectRecording &direct = *m_direct;
	direct.completion = DirectRecording::Completion::Closed;
	while (direct.player.m_pendingDirect == this)
		playDirectCommand();
	if (direct.player.m_boundDirect == this)
		direct.player.m_boundDirect = nullptr;
	endPlay(direct.state, direct.player);
}

//----------------------------------------------------------------------------------------------------------------------
bool CommandContext::addRun(uint32_t bytes)
{
	// Runs start at C_FIRST_RUN and double up to C_CHUNK. A command or reservation larger than the next run gets a run
	// of its own size.
	FrameContexts &f = *m_frame;
	const uint32_t head = uint32_t(sizeof(FrameContexts::RunHeader));
	if (uint64_t(head) + bytes + 15 > FrameContexts::C_ARENA) // Larger than the whole arena.
		return false;
	m_nextRun = m_nextRun == 0						 ? FrameContexts::C_FIRST_RUN
				: m_nextRun < FrameContexts::C_CHUNK ? m_nextRun * 2
													 : FrameContexts::C_CHUNK;
	const uint32_t capacity = head + bytes > m_nextRun ? (head + bytes + 15) & ~15u : m_nextRun;
	const uint32_t run = f.takeRun(capacity);
	if (run == C_NONE)
		return false;
	FrameContexts::RunHeader &h = f.header(run);
	h = FrameContexts::RunHeader{};
	h.capacity = capacity;
	h.used = head;
	if (m_lastRun != C_NONE)
		f.header(m_lastRun).next = run;
	else
		m_firstRun = run;
	m_lastRun = run;
	return true;
}

void CommandContext::seal()
{
	if (m_frame == nullptr)
		return;
	FrameContexts &frame = *m_frame;
	m_frame = nullptr;

	const uint32_t index = uint32_t(this - frame.m_pool);
	uint32_t previous = frame.m_published.load(std::memory_order_acquire);
	do
	{
		m_nextPublished = previous;
	} while (!frame.m_published.compare_exchange_weak(previous, index, std::memory_order_acq_rel,
													  std::memory_order_acquire));
}

void CommandContext::cancel()
{
	if (m_frame == nullptr)
		return;
	FrameContexts &frame = *m_frame;
	m_frame = nullptr;
	frame.release(*this);
}

void *CommandContext::allocate(Command type, uint32_t size)
{
	if (m_frame == nullptr)
		return nullptr;
	FrameContexts &f = *m_frame;
	if (size == 0)
	{
		f.dropped(f.m_droppedCommands);
		return nullptr;
	}
	if (m_lastRun == C_NONE || uint64_t(f.header(m_lastRun).used) + size > f.header(m_lastRun).capacity)
		if (!addRun(size))
		{
			f.dropped(f.m_droppedCommands);
			return nullptr;
		}
	FrameContexts::RunHeader &last = f.header(m_lastRun);
	void					 *p = f.at(m_lastRun, last.used);
	last.used += size;
	++m_commands;
	m_weight += weightOf(type);
	m_bound = m_bound || type == Command::Callback || type == Command::StagedUpload || type == Command::MarkerPush ||
			  type == Command::MarkerPop;
	return p;
}

bool CommandContext::reserve(uint64_t bytes, bool counted)
{
	if (m_frame == nullptr)
		return false;
	FrameContexts &f = *m_frame;
	if (m_lastRun != C_NONE)
	{
		const auto &run = f.header(m_lastRun);
		if (bytes <= run.capacity - run.used)
			return true;
	}
	if (f.couldTake(bytes) && addRun(uint32_t(bytes)))
		return true;
	if (counted)
		f.directUnit();
	return false;
}

bool FrameContexts::couldTake(uint64_t bytes) const
{
	if (bytes > C_ARENA)
		return false;
	// A new run needs its header and alignment, or a chunk when that is larger.
	const uint64_t withHeader = bytes + sizeof(RunHeader) + 15;
	const uint64_t run = withHeader > C_CHUNK ? withHeader : C_CHUNK;
	return m_taken.load(std::memory_order_relaxed) + run <= C_ARENA;
}

void FrameContexts::directUnit()
{
	++m_directUnits;
	if (!m_directLogged.exchange(true))
	{
		char text[256];
		sprintf_s(text,
				  "%s: a unit did not fit in the command context arena (%u MB) or pool; "
				  "recorded directly into the command list",
				  diag::prefix(), C_ARENA >> 20);
		diag::info(text);
	}
}

//----------------------------------------------------------------------------------------------------------------------
bool FrameContexts::create(uint32_t viewTags, uint32_t passTags)
{
	if (viewTags == 0 || viewTags > 256 || passTags == 0 || passTags > 256)
		return false;
	m_viewTags = viewTags;
	m_passTags = passTags;
	const uint32_t tagCount = viewTags * passTags;
	// Back the arena with demand-zero pages so startup only touches memory that gets used for commands.
	m_memory = static_cast<uint8_t *>(VirtualAlloc(nullptr, C_ARENA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	m_pool = static_cast<CommandContext *>(heapAlloc(sizeof(CommandContext) * C_CONTEXTS));
	m_order = static_cast<uint32_t *>(heapAlloc(sizeof(uint32_t) * C_CONTEXTS));
	m_tags[0].counts = static_cast<TagCounts::Count *>(heapAlloc(sizeof(TagCounts::Count) * tagCount * 2));
	if (m_memory == nullptr || m_pool == nullptr || m_order == nullptr || m_tags[0].counts == nullptr)
	{
		destroy();
		return false;
	}
	for (uint32_t i = 0; i < C_CONTEXTS; ++i)
		new (&m_pool[i]) CommandContext;
	for (uint32_t i = 0; i < tagCount * 2; ++i)
		new (&m_tags[0].counts[i]) TagCounts::Count;
	m_tags[1].counts = m_tags[0].counts + tagCount;
	m_tags[0].stride = m_tags[1].stride = passTags;
	return true;
}

void FrameContexts::destroy()
{
	resetRecordings();
	if (m_memory != nullptr)
		VirtualFree(m_memory, 0, MEM_RELEASE);
	heapFree(m_pool); // CommandContext is trivially destructible.
	heapFree(m_order);
	heapFree(m_tags[0].counts);
	m_tags[0] = m_tags[1] = TagCounts{};
	m_memory = nullptr;
	m_pool = nullptr;
	m_order = nullptr;
}

CommandContext *FrameContexts::make(ContextKey key, const char *name, uint8_t view, uint8_t pass)
{
	if (m_pool == nullptr)
		return nullptr;
	const uint32_t index = m_made.fetch_add(1, std::memory_order_relaxed);
	if (index >= C_CONTEXTS)
	{
		m_made.store(C_CONTEXTS, std::memory_order_relaxed); // Clamp so later readers never index past the pool.
		dropped(m_droppedContexts);
		return nullptr;
	}
	CommandContext &c = m_pool[index];
	c = CommandContext{};
	c.m_frame = this;
	c.m_key = key;
	c.m_name = name;
	c.m_view = view < m_viewTags ? view : uint8_t(m_viewTags - 1);
	c.m_pass = pass < m_passTags ? pass : uint8_t(m_passTags - 1);
	return &c;
}

uint32_t FrameContexts::takeRun(uint32_t bytes)
{
	// Reserve only when the run fits, so a failed request never changes m_taken. A smaller request may still succeed.
	if (bytes > C_ARENA)
		return CommandContext::C_NONE;
	uint32_t cur = m_taken.load(std::memory_order_relaxed);
	do
	{
		if (cur > C_ARENA - bytes)
			return CommandContext::C_NONE;
	} while (!m_taken.compare_exchange_weak(cur, cur + bytes, std::memory_order_relaxed, std::memory_order_relaxed));
	return cur;
}

void FrameContexts::dropped(std::atomic<uint32_t> &counter)
{
	++counter;
	if (!m_dropLogged.exchange(true))
	{
		char text[192];
		sprintf_s(text,
				  "%s: command context storage is full (%u contexts, %u MB arena); "
				  "commands dropped",
				  diag::prefix(), C_CONTEXTS, C_ARENA >> 20);
		diag::info(text);
	}
}

void FrameContexts::resetRecordings()
{
	discard(*this);
	const uint32_t made = m_made.exchange(0, std::memory_order_relaxed);
	for (uint32_t i = 0; i < made; ++i)
		m_pool[i].cancel();
	m_taken.store(0, std::memory_order_relaxed);
}

void FrameContexts::endFrame()
{
	m_counts.contexts = m_made.load(std::memory_order_relaxed);
	m_counts.arena = m_taken.load(std::memory_order_relaxed);
	m_last = m_counts;
	m_peakArena = m_counts.arena > m_peakArena ? m_counts.arena : m_peakArena;
	m_peakContexts = m_counts.contexts > m_peakContexts ? m_counts.contexts : m_peakContexts;
	m_peakBytes = m_counts.bytes > m_peakBytes ? m_counts.bytes : m_peakBytes;
	m_runPlayTicks += m_counts.playTicks;
	m_runUploadTicks += m_counts.uploadTicks;
	m_runPassTicks += m_counts.passTicks;
	m_runSortTicks += m_counts.sortTicks;
	m_runReplayTicks += m_counts.replayTicks;
	++m_runFrames;
	m_counts = Stats{};
	if (m_tags[0].counts != nullptr)
	{
		const size_t bytes = size_t(m_viewTags) * m_passTags * sizeof(TagCounts::Count);
		memcpy(m_tags[1].counts, m_tags[0].counts, bytes);
		memset(m_tags[0].counts, 0, bytes);
	}
	resetRecordings();
}
