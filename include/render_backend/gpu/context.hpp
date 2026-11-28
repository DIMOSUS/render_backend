// A context records commands and their payloads in runs from a frame arena. Producers
// record independently, then seal each context to publish it. Playback orders each
// published batch by pass rank and by the ordering key supplied by the caller.
// Command payloads and any resources they refer to must remain valid until playback
// or discard. The frame owns fixed storage for both contexts and command runs.
#pragma once
#include <EASTL/vector.h>
#include <atomic>
#include <new>
#include <nvrhi/nvrhi.h>
#include <optional>
#include <render_backend/gpu/resource_use.hpp>
#include <stdint.h>
#include <string.h>
#include <thread>

namespace render_backend::gpu::markers
{
class Stack;
}

namespace render_backend::gpu::ctx
{
// Command types. Each command has one header followed by a fixed or trailing payload.
enum class Command : uint8_t
{
	MarkerPush,	   // Opens a marker range; the name follows. Timed kinds (view, pass, stage) also start a timer.
	MarkerPop,	   // Closes the context's innermost open range.
	Callback,	   // Calls a function with the command list; its argument bytes follow.
	Application,   // Host-defined operation with a trailing payload and an optional held resource.
	Clear,		   // Clears a depth target.
	BeginPass,	   // Sets the framebuffer and viewport used until the matching EndPass.
	EndPass,	   // Ends the open pass and releases its framebuffer.
	State,		   // Resource state requests; texture and buffer uses follow.
	SetPipeline,   // Binds a pipeline, its binding sets and, for graphics, its vertex and index buffers.
	PushConstants, // Root constants; the bytes follow.
	Draw,		   // Indexed or non-indexed draw.
	DispatchMesh,  // Mesh shader dispatch.
	WriteBuffer,   // Writes bytes into a buffer; the bytes follow.
	StagedUpload,  // Copies a placed upload queue request into its texture at this point in the list.
	ClearTexture,  // Clears texture subresources to a colour or a depth value.
	Copy,		   // Copies one texture slice into another.
	Written,	   // Marks a texture range as written so later reads can transition it.
	Dispatch,	   // Compute shader dispatch.
	Count
};
const char *commandName(Command c);

// First field of every command. `size` includes the payload and is a multiple of 8.
struct CommandHeader
{
	static constexpr uint32_t C_MAX_SIZE = (1u << 24) - 1;
	uint32_t				  type : 8;
	uint32_t				  size : 24;
};

// Sort key of a context within a batch.
//   Bits 63..48: pass rank. Rank 0 is work outside any pass. C_DEAD_RANK marks a culled pass; playback discards such
//                contexts without playing them and releases their references.
//   Bits 47..0:  order within the rank, normally a submission number. Order 0 is reserved for the upload context, which
//                plays first.
struct ContextKey
{
	uint64_t				  value = 0;
	static constexpr uint16_t C_DEAD_RANK = 0xffff;
	static constexpr uint64_t C_ORDER_BITS = 48;
	static constexpr uint64_t C_ORDER_MASK = (uint64_t(1) << C_ORDER_BITS) - 1;
	static ContextKey		  make(uint16_t rank, uint64_t order)
	{
		return ContextKey{uint64_t(rank) << C_ORDER_BITS | (order & C_ORDER_MASK)};
	}
	uint16_t rank() const
	{
		return uint16_t(value >> C_ORDER_BITS);
	}
	uint64_t order() const
	{
		return value & C_ORDER_MASK;
	}
};

struct MarkerPush
{
	static constexpr Command C_TYPE = Command::MarkerPush;
	CommandHeader			 header;
	uint8_t					 kind = 0; // markers::Kind
	uint8_t					 pad = 0;
	uint16_t				 length = 0; // Name length without the terminating zero; the name follows.
};
static_assert(sizeof(MarkerPush) == 8, "MarkerPush");

struct MarkerPop
{
	static constexpr Command C_TYPE = Command::MarkerPop;
	CommandHeader			 header;
	uint32_t				 pad = 0;
};
static_assert(sizeof(MarkerPop) == 8, "MarkerPop");

// Callback function. Receives the owner pointer, the command list and the argument bytes stored after the command.
// This is the only way to record into the list outside playback. The callback must leave no state open (passes,
// marker ranges) when it returns.
using CallbackRun = void (*)(void *owner, nvrhi::ICommandList *list, const uint8_t *data, uint32_t bytes);
struct Callback
{
	static constexpr Command C_TYPE = Command::Callback;
	CommandHeader			 header;
	uint32_t				 valueBytes = 0; // Size of the argument bytes that follow.
	CallbackRun				 run = nullptr;
	void					*owner = nullptr;
};
static_assert(sizeof(Callback) == 24, "Callback");

// The host decodes this command's payload. `preparesReads` declares that it drains pending bindless reads on its list.
// Playback and discard release the optional reference acquired by the recorder.
struct ApplicationCommand
{
	static constexpr Command C_TYPE = Command::Application;
	CommandHeader			 header;
	uint32_t				 preparesReads = 0;
	nvrhi::IResource		*resource = nullptr;
};
static_assert(sizeof(ApplicationCommand) == 16, "ApplicationCommand");

// A depth clear on a held texture. Playback releases the reference after the player handles the clear.
struct Clear
{
	static constexpr Command C_TYPE = Command::Clear;
	CommandHeader			 header;
	float					 depth = 1.0f;
	nvrhi::ITexture			*texture = nullptr;
};
static_assert(sizeof(Clear) == 16, "Clear");

// Pass commands. A pass is recorded as one unit: its total size is reserved with CommandContext::reserve before the
// first command is written, so a full arena drops the whole pass rather than truncating it. Pipelines and binding sets
// must stay alive until the contexts that name them are played; recreate them only after playback. Objects that may
// be released during the frame (framebuffers, caller-owned vertex and index buffers) are held by reference from
// recording until playback or discard.

// A pass over `framebuffer` (held until its EndPass is played). The viewport also supplies the scissor rectangle.
// Bounds are min x, max x, min y, max y, min z, max z.
struct BeginPass
{
	static constexpr Command C_TYPE = Command::BeginPass;
	CommandHeader			 header;
	uint32_t				 pad = 0;
	nvrhi::IFramebuffer		*framebuffer = nullptr;
	float					 viewport[6] = {};
};
static_assert(sizeof(BeginPass) == 40, "BeginPass");

struct EndPass
{
	static constexpr Command C_TYPE = Command::EndPass;
	CommandHeader			 header;
	uint32_t				 pad = 0;
};
static_assert(sizeof(EndPass) == 8, "EndPass");

// Native texture and buffer state requests precede the draws that read them. The trailing payload contains
// texture uses, then buffer uses. Use recordResourceUses to retain every resource until playback or discard.
struct State
{
	static constexpr Command C_TYPE = Command::State;
	CommandHeader header;
	uint32_t textureCount = 0, bufferCount = 0;
	uint32_t pad = 0;
	ResourceUses uses() const;
};
static_assert(sizeof(State) == 16, "State");

enum class PipelineKind : uint8_t
{
	Meshlet,  // nvrhi::IMeshletPipeline, bound with setMeshletState.
	Graphics, // nvrhi::IGraphicsPipeline, bound with setGraphicsState, optionally with vertex input.
	Compute,  // nvrhi::IComputePipeline, bound with setComputeState outside a graphics pass.
};
// Binds a pipeline. The pipeline must not be null, and it and its binding sets must stay alive until playback. Graphics
// and meshlet pipelines use the framebuffer and viewport of the open pass. Non-null `vertices`, `indices` and
// `instances` are references owned by the command and released at playback or discard, for every kind; only graphics
// pipelines bind them. A compute pipeline must be set outside a graphics pass, in the same context as its dispatches.
struct SetPipeline
{
	static constexpr Command C_TYPE = Command::SetPipeline;
	CommandHeader			 header;
	PipelineKind			 kind = PipelineKind::Meshlet;
	uint8_t					 indexFormat = 0; // nvrhi::Format
	uint16_t				 instanced = 0;	  // 1: bind a per-instance vertex buffer in slot 1 at instanceOffset.
	uint32_t				 vertexOffset = 0;
	uint32_t				 instanceOffset = 0;
	nvrhi::IResource		*pipeline = nullptr;
	nvrhi::IBindingSet		*sets[3] = {};
	nvrhi::IBuffer			*vertices = nullptr;
	nvrhi::IBuffer			*indices = nullptr;
	nvrhi::IBuffer			*instances = nullptr; // Slot 1 buffer. Null uses `vertices` for both slots.
};
static_assert(sizeof(SetPipeline) == 72, "SetPipeline");

// Root constants for the bound pipeline. `bytes` bytes follow.
struct PushConstants
{
	static constexpr Command C_TYPE = Command::PushConstants;
	CommandHeader			 header;
	uint32_t				 bytes = 0;
};
static_assert(sizeof(PushConstants) == 8, "PushConstants");

struct Draw
{
	static constexpr Command C_TYPE = Command::Draw;
	CommandHeader			 header;
	uint32_t				 vertexCount = 0; // Index count when `indexed` is set.
	uint32_t				 instanceCount = 1;
	uint32_t				 startIndex = 0;
	uint32_t				 startVertex = 0;
	uint32_t				 indexed = 0;
};
static_assert(sizeof(Draw) == 24, "Draw");

struct DispatchMesh
{
	static constexpr Command C_TYPE = Command::DispatchMesh;
	CommandHeader			 header;
	uint32_t				 groups = 0; // Amplification shader groups.
};
static_assert(sizeof(DispatchMesh) == 8, "DispatchMesh");

// Compute groups after SetPipeline(Compute), with optional PushConstants.
// Rebind after operations that change pipeline state or the state of bound
// resources.
struct Dispatch
{
	static constexpr Command C_TYPE = Command::Dispatch;
	CommandHeader			 header;
	uint32_t				 groupsX = 0;
	uint32_t				 groupsY = 1;
	uint32_t				 groupsZ = 1;
};
static_assert(sizeof(Dispatch) == 16, "Dispatch");

// Writes the `bytes` bytes that follow into `buffer` at `offset`. The data is copied at record time.
struct WriteBuffer
{
	static constexpr Command C_TYPE = Command::WriteBuffer;
	CommandHeader			 header;
	uint32_t				 bytes = 0;
	nvrhi::IBuffer			*buffer = nullptr;
	uint64_t				 offset = 0;
};
static_assert(sizeof(WriteBuffer) == 24, "WriteBuffer");

// Texture clears and copies. The caller may release these textures during the frame, so each command holds a reference
// from recording until playback or discard.

// Copies the staged bytes of a Placed upload queue request (upload_queue.hpp) into `texture` at this point in the list.
// Use it when a texture is overwritten while earlier draws in the frame must still read the old contents. The queue
// owns the texture reference and the staged bytes; `texture` is only used to identify what the context writes.
struct StagedUpload
{
	static constexpr Command C_TYPE = Command::StagedUpload;
	CommandHeader			 header;
	uint32_t				 ticket = 0;
	nvrhi::ITexture			*texture = nullptr;
};
static_assert(sizeof(StagedUpload) == 16, "StagedUpload");

// Clears `subresources` of `texture` to the colour `value`, or, when `depth` is non-zero, to the depth value[0].
// Stencil is not cleared.
struct ClearTexture
{
	static constexpr Command	 C_TYPE = Command::ClearTexture;
	CommandHeader				 header;
	uint32_t					 depth = 0;
	nvrhi::ITexture				*texture = nullptr;
	nvrhi::TextureSubresourceSet subresources;
	float						 value[4] = {};
};
static_assert(sizeof(ClearTexture) == 48, "ClearTexture");

// Copies slice `from` of `source` into slice `to` of `target`.
struct Copy
{
	static constexpr Command C_TYPE = Command::Copy;
	CommandHeader			 header;
	uint32_t				 pad = 0;
	nvrhi::ITexture			*target = nullptr;
	nvrhi::ITexture			*source = nullptr;
	nvrhi::TextureSlice		 to, from;
};
static_assert(sizeof(Copy) == 88, "Copy");

// Defer a written texture range's bindless ShaderResource transition until the next State or preparing application.
// The recorder holds one reference to texture until playback or discard.
struct Written
{
	static constexpr Command C_TYPE = Command::Written;
	CommandHeader header;
	uint32_t pad = 0;
	nvrhi::ITexture *texture = nullptr;
	nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources;
};
static_assert(sizeof(Written) == 32, "Written");

// Aligned size of an encoded command. Zero means the payload is too large for
// the size field.
template <class C> constexpr uint32_t commandBytes(uint32_t dataBytes)
{
	const uint64_t size = (uint64_t(sizeof(C)) + dataBytes + 7) & ~uint64_t(7);
	return size <= CommandHeader::C_MAX_SIZE ? uint32_t(size) : 0;
}

class FrameContexts;
class Player;
class Recorder;

// A context has one producer until it is sealed or cancelled.
class CommandContext
{
  public:
	// Publish all commands and payload writes. Writes through previously returned
	// command pointers must finish first. A closed context cannot be edited.
	void seal();
	// Release an open context's command references without publishing it.
	// Both operations are no-ops on a closed context.
	void cancel();

	// A command with storage for its payload. An oversized command or a full
	// arena returns null and counts a drop.
	template <class C> C *add(uint32_t dataBytes = 0)
	{
		const uint32_t size = commandBytes<C>(dataBytes);
		void		  *p = allocate(C::C_TYPE, size);
		if (p == nullptr)
			return nullptr;
		C *c = new (p) C{};
		c->header.type = uint32_t(C::C_TYPE);
		c->header.size = size;
		return c;
	}
	// Same as above, and copies `bytes` bytes of payload from `data`.
	template <class C> C *add(const void *data, uint32_t bytes)
	{
		C *c = add<C>(bytes);
		if (c != nullptr && bytes != 0)
			memcpy(c + 1, data, bytes);
		return c;
	}
	// Payload that follows a command.
	template <class C> static uint8_t *data(C *c)
	{
		return reinterpret_cast<uint8_t *>(c + 1);
	}
	// Ensures the last run has room for `bytes` (the sum of commandBytes of the commands to follow), so adding them
	// cannot fail part way. Returns false when the arena cannot provide that much; the caller should then record the
	// unit directly into the command list with a Recorder. Such refusals are counted in directUnits and logged once,
	// unless `counted` is false (used when the caller has another fallback before going direct).
	bool	   reserve(uint64_t bytes, bool counted = true);
	ContextKey key() const
	{
		return m_key;
	}
	// Sum of command weights. Used to balance contexts across command lists.
	uint32_t weight() const
	{
		return m_weight;
	}
	// True when the context must play on the primary command list: it contains a Callback (which records into the
	// list it is given), a StagedUpload (the upload queue copies on the primary list), or a marker range (the marker
	// stack belongs to the primary list).
	bool bound() const
	{
		return m_bound;
	}
	uint32_t commands() const
	{
		return m_commands;
	}
	// Caller-defined view and pass tags. Playback counts contexts per tag pair and passes the tags to Player::slot for
	// markers.
	uint8_t view() const
	{
		return m_view;
	}
	uint8_t pass() const
	{
		return m_pass;
	}

  private:
	friend class FrameContexts;
	CommandContext() = default;
	CommandContext(const CommandContext &) = delete;
	CommandContext &operator=(const CommandContext &) = default;
	friend void				  play(FrameContexts &frame, Player &player);
	friend void				  discard(FrameContexts &frame);
	static constexpr uint32_t C_NONE = ~0u;
	// Allocate the aligned command size at the end of the chain. A zero size is
	// invalid.
	void *allocate(Command type, uint32_t size);
	// Appends a run with room for `bytes` after its header. False when the arena is full.
	bool addRun(uint32_t bytes);

	FrameContexts *m_frame = nullptr; // Non-null only while the context is open for recording.
	uint32_t	   m_nextPublished = C_NONE;
	const char	  *m_name = nullptr; // Static string supplied by the caller.
	ContextKey	   m_key{};
	uint32_t	   m_firstRun = C_NONE, m_lastRun = C_NONE; // Arena offsets of the first and last run.
	uint32_t	   m_nextRun = 0;							// Size of the next run; 0 before the first.
	uint32_t	   m_commands = 0, m_weight = 0;
	uint8_t		   m_view = 0, m_pass = 0;
	bool		   m_bound = false;
};

// Per-frame storage for command contexts: a fixed context pool, a fixed arena allocated once, the playback order and
// statistics. Each context records into a chain of runs taken lock-free from the end of the arena. The first run is
// 1 KB and each following run doubles, up to 64 KB, so small contexts do not hold a whole chunk. A command larger than
// the next run gets a run of its own size.
class FrameContexts
{
  public:
	static constexpr uint32_t C_FIRST_RUN = 1u << 10;
	static constexpr uint32_t C_CHUNK = 64u << 10;
	static constexpr uint32_t C_ARENA = 128u << 20;
	static constexpr uint32_t C_CONTEXTS = 8192;

	// Provide host tag dimensions in [1, 256]. Out-of-range tags count in the last slot.
	bool create(uint32_t viewTags = 1, uint32_t passTags = 1);
	// All producers and replay workers must be done before destroying or ending the frame.
	void destroy();
	// Allocate an unpublished context for one producer. Null when the pool is full.
	CommandContext *make(ContextKey key, const char *name, uint8_t view = 0, uint8_t pass = 0);
	// True if a new run of `bytes` would currently fit. This is the test CommandContext::reserve uses; callers can use
	// it to avoid making a context for a unit that would be refused. The answer may be stale under concurrency.
	bool couldTake(uint64_t bytes) const;
	// Counts a unit recorded directly into the command list (directUnits) and logs the first one.
	void directUnit();
	// Published contexts waiting for the next batch. Open writers do not count.
	bool pending() const
	{
		return m_published.load(std::memory_order_acquire) != CommandContext::C_NONE;
	}
	// After playback and producer completion, discard leftover work, save the
	// frame's counters and reset the pool and arena.
	void endFrame();

	struct Stats
	{
		uint32_t contexts = 0;
		uint32_t arena = 0; // Arena bytes taken by runs.
		uint64_t bytes = 0; // Command bytes including payloads.
		uint32_t commands[uint32_t(Command::Count)] = {};
		// Key checks during playback: contexts with equal keys, and upload contexts that did not play first.
		uint32_t ties = 0, uploadNotFirst = 0;
		uint32_t dead = 0; // Contexts with ContextKey::C_DEAD_RANK, discarded unplayed.
		// Upload-like commands that played in submission order outside the upload context: texture uploads and
		// clears (StagedUpload, ClearTexture) and buffer writes (WriteBuffer).
		uint32_t inPlaceUploads = 0, inPlaceWrites = 0;
		// Playback time in QueryPerformanceCounter ticks: total, the upload contexts' share, and the share spent
		// between BeginPass and EndPass.
		int64_t playTicks = 0, uploadTicks = 0, passTicks = 0;
		// CPU wall time for ordering (including its key checks) and for replay
		// after ordering.
		int64_t sortTicks = 0, replayTicks = 0;
	};
	const Stats &lastFrame() const
	{
		return m_last;
	}

	// Take the currently published batch and sort by key, then by allocation
	// index for ties. Replay or discard every sorted entry before sorting again.
	// Keys order a batch only. Publish dependent contexts in the intended batch
	// before starting its playback.
	uint32_t			  sort();
	uint64_t			  pendingCommands() const; // Commands in the published batch.
	const CommandContext &sorted(uint32_t n) const
	{
		return m_pool[m_order[n]];
	}
	// Plays sorted contexts [from, to) into player.list and accumulates statistics into `counts`. Contexts with
	// ContextKey::C_DEAD_RANK are discarded. May run on any thread, provided each concurrent range has its own player
	// and counts; merge the counts with addCounts afterwards.
	void playSorted(uint32_t from, uint32_t to, Player &player, Stats &counts);
	// Release an unplayed sorted range when its executor aborts. The ordering
	// cursor has already advanced.
	void discardSorted(uint32_t from, uint32_t to);
	void addCounts(const Stats &counts);
	// Advance only deferred read intent when planning an auxiliary range. NVRHI tracks native states during playback.
	void followReads(uint32_t from, uint32_t to, ResourceTransitions &transitions);
	// Contexts and commands played per [view][pass] tag. Allocated in create().
	struct TagCounts
	{
		struct Count
		{
			uint32_t commands = 0;
			uint16_t contexts = 0;
		};
		const Count &at(uint32_t view, uint32_t pass) const
		{
			return counts[view * stride + pass];
		}

	  private:
		friend class FrameContexts;
		Count	*counts = nullptr;
		uint32_t stride = 0;
	};
	const TagCounts *lastTags() const // Null before create().
	{
		return m_tags[1].counts != nullptr ? &m_tags[1] : nullptr;
	}
	uint32_t peakArena() const
	{
		return m_peakArena;
	}
	uint64_t peakBytes() const
	{
		return m_peakBytes;
	}
	// Accumulated timings in ticks since create(), over runFrames() frames. See Stats.
	int64_t runPlayTicks() const
	{
		return m_runPlayTicks;
	}
	int64_t runUploadTicks() const
	{
		return m_runUploadTicks;
	}
	int64_t runPassTicks() const
	{
		return m_runPassTicks;
	}
	int64_t runSortTicks() const
	{
		return m_runSortTicks;
	}
	int64_t runReplayTicks() const
	{
		return m_runReplayTicks;
	}
	uint64_t runFrames() const
	{
		return m_runFrames;
	}
	uint32_t peakContexts() const
	{
		return m_peakContexts;
	}
	uint32_t droppedContexts() const
	{
		return m_droppedContexts.load(std::memory_order_relaxed);
	}
	uint32_t droppedCommands() const
	{
		return m_droppedCommands.load(std::memory_order_relaxed);
	}
	// Units refused by reserve() and recorded directly into the command list instead.
	uint32_t directUnits() const
	{
		return m_directUnits.load(std::memory_order_relaxed);
	}

  private:
	friend class CommandContext;
	friend void play(FrameContexts &frame, Player &player);
	friend void discard(FrameContexts &frame);
	struct RunHeader // Stored at the start of each run; links the context's chain.
	{
		uint32_t next = CommandContext::C_NONE; // Arena offset of the next run.
		uint32_t used = 0;						// Bytes used, including this header.
		uint32_t capacity = 0;					// Total bytes of the run.
		uint32_t pad = 0;
	};
	// Takes `bytes` (a multiple of 16) from the end of the arena and returns the offset, or C_NONE when it does not
	// fit. Lock-free.
	uint32_t   takeRun(uint32_t bytes);
	RunHeader &header(uint32_t run)
	{
		return *reinterpret_cast<RunHeader *>(m_memory + run);
	}
	uint8_t *at(uint32_t run, uint32_t offset)
	{
		return m_memory + run + offset;
	}
	// Releases the references held by a context's commands without playing them.
	void release(const CommandContext &c);
	void resetRecordings();
	void dropped(std::atomic<uint32_t> &counter);

	uint8_t				 *m_memory = nullptr;
	CommandContext		 *m_pool = nullptr;	 // C_CONTEXTS entries.
	uint32_t			 *m_order = nullptr; // C_CONTEXTS entries: sorted pool indices.
	TagCounts			  m_tags[2];		 // Running and published tag counts.
	uint32_t			  m_viewTags = 1, m_passTags = 1;
	std::atomic<uint32_t> m_made{0}, m_taken{0};
	// Intrusive list of sealed pool slots. Each slot is published at most once per frame.
	std::atomic<uint32_t> m_published{CommandContext::C_NONE};
	Stats				  m_counts, m_last; // Current frame and last finished frame.
	uint32_t			  m_peakArena = 0, m_peakContexts = 0;
	uint64_t			  m_peakBytes = 0;
	int64_t				  m_runPlayTicks = 0, m_runUploadTicks = 0, m_runPassTicks = 0;
	int64_t				  m_runSortTicks = 0, m_runReplayTicks = 0;
	uint64_t			  m_runFrames = 0;
	std::atomic<uint32_t> m_droppedContexts{0}, m_droppedCommands{0}, m_directUnits{0};
	std::atomic<bool>	  m_dropLogged{false}, m_directLogged{false};
};

// Playback target: the command list, the marker stack, and host hooks for commands that need application state.
// Markers and callbacks are played by the playback itself.
class Player
{
  public:
	Player(const Player &) = delete;
	Player &operator=(const Player &) = delete;
	nvrhi::ICommandList *list = nullptr;
	markers::Stack		*markers = nullptr; // Null when markers are disabled.
	virtual void		 application(const ApplicationCommand &, const uint8_t *)
	{
	}
	virtual void clear(const Clear &c) = 0;
	virtual void prepareReads(ResourceUses uses = {}) = 0;
	virtual void written(nvrhi::ITexture *texture,
						 nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources) = 0;
	virtual void staged(const StagedUpload &c) = 0;
	// Called only when markers are enabled. slot() reports the (view, pass) tag of the next context; the player closes
	// its open view and pass ranges when the tag changes and opens the new ones. slotsEnd() is called at the end of
	// playback and must close every range the player opened, so each playback is balanced.
	virtual void slot(uint8_t view, uint8_t pass) = 0;
	virtual void slotsEnd() = 0;

  protected:
	Player() = default;
	~Player() = default;

  private:
	friend class Recorder;
	Recorder *m_pendingDirect = nullptr;
	Recorder *m_boundDirect = nullptr;
};

// Playback state of one context: the marker stack levels of its open ranges, the open pass (its framebuffer is held
// until EndPass) and viewport, and whether a compute pipeline is bound.
struct PlayState
{
	uint32_t			 levels[16]; // Size must match markers::Stack::C_DEPTH (checked in playback.cpp).
	uint32_t			 open = 0;
	nvrhi::IFramebuffer *pass = nullptr;
	nvrhi::Viewport		 viewport;
	bool				 computeBound = false;
};
// Plays a command into player.list or returns false and releases its references
// if it cannot be played here.
bool playCommand(const uint8_t *command, PlayState &state, Player &player);
// Ends a context's playback: releases an open pass and closes its open marker ranges.
void endPlay(PlayState &state, Player &player);

// Destination for a unit of commands. Records either into a context, played later, or, when the context could not
// reserve the unit, directly into a command list. Direct recorders on one player execute commands in add() order.
// Their scopes may overlap between passes, compute bindings and marker ranges on the same playback thread. Each command
// owns its bytes until playback returns. Scratch caches storage from completed commands. Finish direct units before
// switching lists or resuming buffered playback, and play preceding contexts before recording directly.
class Recorder
{
  public:
	explicit Recorder(CommandContext *context) : m_context(context)
	{
	}
	Recorder(Player &player, eastl::vector<uint8_t> &scratch) : m_direct(std::in_place, player, scratch)
	{
	}
	Recorder(const Recorder &) = delete;
	Recorder &operator=(const Recorder &) = delete;
	~Recorder()
	{
		finish();
	}
	// A command of type C followed by `dataBytes` of payload (CommandContext::data). The caller must fill it before the
	// next add() (including on another direct recorder using this player) or finish(). Null for an oversized command,
	// a full arena, a finished direct recorder, or a direct recording that would interrupt another unit's open state.
	template <class C> C *add(uint32_t dataBytes = 0)
	{
		if (m_context != nullptr)
			return m_context->add<C>(dataBytes);
		const uint32_t size = commandBytes<C>(dataBytes);
		if (size == 0 || !m_direct)
			return nullptr;
		void *storage = beginDirectCommand(size);
		if (storage == nullptr)
			return nullptr;
		C *c = new (storage) C{};
		c->header.type = uint32_t(C::C_TYPE);
		c->header.size = size;
		return c;
	}
	template <class C> C *add(const void *data, uint32_t bytes)
	{
		C *c = add<C>(bytes);
		if (c != nullptr && bytes != 0)
			memcpy(c + 1, data, bytes);
		return c;
	}
	// Ends the unit. In direct mode, plays the last command and closes any open pass and marker ranges.
	void finish()
	{
		if (m_direct)
			finishDirect();
	}

  private:
	struct DirectRecording
	{
		enum class Completion
		{
			Open,
			Requested,
			Closed
		};
		DirectRecording(Player &player, eastl::vector<uint8_t> &scratch) : player(player), scratch(scratch)
		{
		}
		bool hasOpenState() const
		{
			return state.pass != nullptr || state.computeBound || state.open != 0;
		}
		Player &player;
		eastl::vector<uint8_t> &scratch;
		eastl::vector<uint8_t> command;
		PlayState state;
		Completion completion = Completion::Open;
		bool playing = false;
	};
	void *beginDirectCommand(uint32_t size);
	void playDirectCommand();
	void finishDirect();
	void closeDirect();

	CommandContext *m_context = nullptr;
	std::optional<DirectRecording> m_direct;
};

// Marker ranges. markerPushTo copies the name, truncated to C_MARKER_NAME - 1 characters. MarkerPop closes the
// innermost open range of the context, and playback closes any range a context leaves open. Reserve C_MARKER_BYTES
// per range in a unit so markers are never dropped.
constexpr uint32_t C_MARKER_NAME = 96; // Must equal markers::Stack::C_NAME (checked in context.cpp).
constexpr uint32_t C_MARKER_BYTES = commandBytes<MarkerPush>(C_MARKER_NAME) + commandBytes<MarkerPop>(0); // One pair.
template <class Target> void markerPushTo(Target &target, uint8_t kind, const char *name)
{
	const size_t length = strnlen(name, C_MARKER_NAME - 1);
	if (MarkerPush *m = target.template add<MarkerPush>(uint32_t(length + 1)))
	{
		m->kind = kind;
		m->length = uint16_t(length);
		memcpy(CommandContext::data(m), name, length);
		CommandContext::data(m)[length] = 0;
	}
}
void markerPushf(Recorder &r, uint8_t kind, const char *format, ...);

// Replay one published batch. Contexts of ContextKey::C_DEAD_RANK are discarded.
void play(FrameContexts &frame, Player &player);
// Discard one published batch and release its command references. Open
// contexts are unaffected. Use the same consumer thread as sort or play.
void discard(FrameContexts &frame);

// Small dedicated thread pool for parallel playback. run(count, job) calls job(i) for every i < count, sharing the
// work between the calling thread and the pool, and returns when all jobs are done and every worker is idle again.
// Threads start on the first run() and stop in stop() or the destructor. run() must not be called concurrently.
class PlayThreads
{
  public:
	static constexpr uint32_t C_THREADS = 3;
	PlayThreads() = default;
	PlayThreads(const PlayThreads &) = delete;
	PlayThreads &operator=(const PlayThreads &) = delete;
	~PlayThreads()
	{
		stop();
	}
	template <class Job> void run(uint32_t count, Job &job)
	{
		m_job = &job;
		m_call = [](void *j, uint32_t i) { (*static_cast<Job *>(j))(i); };
		dispatch(count);
	}
	void stop();

  private:
	void dispatch(uint32_t count);
	void work(); // Takes jobs until none are left.
	void loop(); // Worker thread body.

	void (*m_call)(void *job, uint32_t i) = nullptr;
	void				 *m_job = nullptr;
	uint32_t			  m_count = 0;
	std::atomic<uint32_t> m_next{0};
	// Win32 semaphores. Each worker takes one wake per dispatch and releases one done after its share.
	void			 *m_wake = nullptr, *m_done = nullptr;
	std::thread		  m_threads[C_THREADS];
	bool			  m_started = false;
	std::atomic<bool> m_stop{false};
};
// Bytes to reserve for one State command, or zero if the requested payload is too large.
constexpr uint32_t resourceUseCommandBytes(uint32_t textureCount, uint32_t bufferCount)
{
	const uint64_t bytes = uint64_t(textureCount) * sizeof(TextureUse) + uint64_t(bufferCount) * sizeof(BufferUse);
	return bytes <= CommandHeader::C_MAX_SIZE ? commandBytes<State>(uint32_t(bytes)) : 0;
}
bool recordResourceUses(CommandContext &context, ResourceUses uses);
bool recordResourceUses(Recorder &recorder, ResourceUses uses);
} // namespace render_backend::gpu::ctx
