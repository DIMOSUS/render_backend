// Upload queue for texture and buffer data, independent of the command contexts. Uses only NVRHI and D3D12.
//
// Staging: the queue owns chunks of upload-heap memory that stay mapped. A producer opens a request with begin(),
// obtains staging space for each texture subresource or buffer range (texture(), buffer(); writeTexture() and
// writeBuffer() also copy the data) and closes it with end(). Data never passes through the command context arena, so
// a request is limited only by available memory. Chunks are filled front to back. A chunk is reset once every part in
// it has been recorded or cancelled and every recording that references it has completed on the GPU or been
// discarded. Chunks that stay unused for Config::idleFrames are released, so staging shrinks back to one chunk after a
// burst of uploads.
//
// When copies are recorded:
//   Queued requests (the default) are recorded by the next issue(), ahead of anything recorded after that call.
//   Placed requests are recorded only by issueTicket(), at a specific point in the list. Use them when a resource is
//   overwritten while earlier draws in the same list must still read the old contents; the caller is responsible for
//   ordering, for example with a StagedUpload command.
// Recording emits one barrier batch moving every destination to CopyDest, then the copies, then one barrier batch
// moving each destination to the state its request asked for. The NVRHI command list keeps the destinations alive. The
// queue keeps staging memory alive until the fence of the submission that read it has signalled.
//
// Budget: the queue never defers a request on its own. Callers with work that can wait (for example the first upload
// of a texture nothing reads yet) call admit(bytes) first. admit() succeeds while the frame budget is not yet spent,
// so the first such request in a frame is always accepted regardless of size, and the request that crosses the budget
// is the last one accepted that frame. After a refusal the caller stages nothing and retries later.
//
// Threading: every public call takes the queue lock; data is copied outside it. A request belongs to one thread from
// begin() to end(). beginFrame, issue, issueTicket, submit and discard must be called from the thread that records and
// submits the graphics command lists.
#pragma once
#include <nvrhi/nvrhi.h>
#include <EASTL/vector.h>
#include <mutex>
#include <stdint.h>

struct ID3D12Device;
struct ID3D12Resource;

namespace render_backend::gpu::upload
{
struct Config
{
	uint64_t chunkBytes = 32ull << 20;	  // Staging chunk size (minimum 1 MB). Larger parts get a dedicated chunk.
	uint64_t budgetBytes = 1024ull << 20; // Bytes admit() accepts per frame.
	uint64_t markBytes = 2048ull << 20;	  // Staging capacity above this is counted in overMark; nothing is refused.
	uint32_t idleFrames = 240;			  // Unused frames before a standard chunk is released. One chunk is kept.
};

struct Stats
{
	// Staging: live chunks; chunks created and released since create(); chunks created while capacity exceeded
	// Config::markBytes. Total chunk bytes and bytes in use.
	uint32_t chunks = 0, chunksMade = 0, chunksReleased = 0, overMark = 0;
	uint64_t capacity = 0, capacityPeak = 0, used = 0, usedPeak = 0;
	// Requests since create(), how many were placed, and how many admit() refused.
	uint64_t requests = 0, placed = 0, refused = 0;
	// Copies recorded: last finished frame, peak per frame, and totals since create(). admittedBytes is what admit()
	// accepted in the last finished frame.
	uint64_t frameBytes = 0, frameBytesPeak = 0, runBytes = 0, runCopies = 0, admittedBytes = 0;
	uint32_t frameCopies = 0, frameCopiesPeak = 0;
};

class Queue
{
  public:
	// Staging space for a texture subresource: `rows` rows (block rows for compressed formats), `rowPitch` bytes
	// apart, of which the first `rowBytes` bytes are used.
	struct Room
	{
		uint8_t *bytes = nullptr;
		uint32_t rowPitch = 0, rows = 0, rowBytes = 0;
	};

	bool create(nvrhi::IDevice *device, const Config &config);
	// Waits for submitted copies. Stop producers and abandon any unsubmitted recordings before destroying the queue.
	void		  destroy();
	const Config &config() const
	{
		return m_config;
	}

	// True when the frame budget is spent and admit() would refuse. Does not change the budget.
	bool spent();
	// Asks to stage about `bytes` of deferrable work this frame. True counts the bytes against the budget; false means
	// retry in a later frame.
	bool admit(uint64_t bytes);

	// Opens a request and returns its ticket, or 0 if the queue has not been created. A placed request is recorded only
	// by issueTicket().
	uint32_t begin(bool placed = false);
	// Staging space for subresource (arraySlice, mipLevel) of `texture`. After the copy the whole texture is in
	// `after`. Fails for an out-of-range subresource.
	bool texture(uint32_t ticket, nvrhi::ITexture *texture, uint32_t arraySlice, uint32_t mipLevel, Room &room,
				 nvrhi::ResourceStates after = nvrhi::ResourceStates::ShaderResource);
	// Like texture(), and copies `data` with rows `rowPitch` bytes apart. The last row only needs to be as long as a
	// subresource row.
	bool writeTexture(uint32_t ticket, nvrhi::ITexture *texture, uint32_t arraySlice, uint32_t mipLevel,
					  const void *data, size_t rowPitch,
					  nvrhi::ResourceStates after = nvrhi::ResourceStates::ShaderResource);
	// Staging space for `size` bytes of `buffer` at `offset`, which must lie within the buffer. After the copy the
	// buffer is in `after`. Set `permanent` for an untracked buffer created in COMMON (for example the initial
	// contents of a vertex or index buffer); it then keeps `after` as its permanent state.
	uint8_t *buffer(uint32_t ticket, nvrhi::IBuffer *buffer, uint64_t offset, uint64_t size,
					nvrhi::ResourceStates after, bool permanent = false);
	bool	 writeBuffer(uint32_t ticket, nvrhi::IBuffer *buffer, uint64_t offset, const void *data, uint64_t size,
						 nvrhi::ResourceStates after, bool permanent = false);
	// Stages a subresource before its texture is known, for producers that do not hold GPU objects. Lays out a
	// `width` x `height` subresource of DXGI format `dxgiFormat` and copies `data` with rows `rowPitch` bytes apart.
	// bind() later supplies the texture; the data goes to its subresource (arraySlice, mipLevel). Placed requests only.
	bool stageTexture(uint32_t ticket, uint32_t dxgiFormat, uint32_t width, uint32_t height, uint32_t arraySlice,
					  uint32_t mipLevel, const void *data, size_t rowPitch,
					  nvrhi::ResourceStates after = nvrhi::ResourceStates::ShaderResource);
	// Sets `texture` as the destination of every part of a placed request staged with stageTexture().
	void bind(uint32_t ticket, nvrhi::ITexture *texture);
	// Abandons a request before it is recorded. Its parts no longer keep their chunks from being reset.
	void cancel(uint32_t ticket);
	// Closes a request. A queued request is recorded by the next issue(); a placed one waits for issueTicket().
	void end(uint32_t ticket);

	// Starts a frame. `frame` increases by one per frame. Resets or releases chunks whose copies have completed and
	// resets the budget.
	void beginFrame(uint64_t frame);
	// True when queued copies have not been issued yet.
	bool pending();
	// Records all queued copies into the open `list`. False if there were none or recording failed.
	bool issue(nvrhi::ICommandList *list);
	// Records a placed request's copies into `list` at the current position.
	void issueTicket(nvrhi::ICommandList *list, uint32_t ticket);
	// Submit a closed graphics list and fence any copies recorded by this queue. A zero result leaves the recording
	// pinned for retry or discard. Submit every recording that contains uploads through this method.
	// An optional closed graphics prelude is submitted in order, in the same batch.
	uint64_t submit(nvrhi::ICommandList *list, nvrhi::ICommandList *prelude = nullptr);
	// Release staging references from an abandoned recording. The caller must never execute that recording.
	void  discard(nvrhi::ICommandList *list);
	Stats stats();

  private:
	struct Chunk
	{
		ID3D12Resource *resource = nullptr;
		uint8_t		   *mapped = nullptr;
		uint64_t		size = 0, used = 0;
		uint32_t		unissued = 0; // Parts staged here and not yet recorded.
		uint32_t		pins = 0;	  // Recorded lists and in-flight submissions that read this chunk.
		uint64_t		idleFrom = 0; // Frame at which the chunk last became empty.
	};
	struct Part // One texture subresource or buffer range; the unit of copying.
	{
		nvrhi::TextureHandle  texture; // Exactly one of texture and buffer is set once the part is bound.
		nvrhi::BufferHandle	  buffer;
		nvrhi::ResourceStates after = nvrhi::ResourceStates::Unknown;
		uint32_t			  chunk = 0;
		uint64_t			  offset = 0, bytes = 0; // Location within the chunk.
		// Texture: destination subresource and staged footprint.
		uint32_t subresource = 0, format = 0, footWidth = 0, footHeight = 0, rowPitch = 0;
		uint32_t arraySlice = 0, mipLevel = 0; // Set by stageTexture(); bind() converts them to `subresource`.
		// Buffer.
		uint64_t destOffset = 0;
		bool	 permanent = false;
	};
	struct Request
	{
		eastl::vector<Part> parts;
		bool				used = false, placed = false;
	};
	struct CopyBatch
	{
		// A recording holds the command list; after submission it holds only the completion query.
		nvrhi::CommandListHandle list;
		nvrhi::EventQueryHandle	 completion;
		eastl::vector<uint32_t>	 chunks;
	};
	using CopyBatches = eastl::vector<CopyBatch>;

	Request				 *find(uint32_t ticket);
	void				  release(Request &r);
	bool				  allocate(uint64_t size, uint32_t alignment, uint32_t &chunk, uint64_t &offset);
	bool				  record(nvrhi::ICommandList *list, const Part *parts, size_t count);
	CopyBatches::iterator findRecording(nvrhi::ICommandList *list);
	void				  releasePins(const CopyBatch &batch);

	nvrhi::DeviceHandle		m_device;
	ID3D12Device		   *m_d3d = nullptr;
	Config					m_config;
	std::mutex				m_lock;
	eastl::vector<Chunk>	m_chunks;
	eastl::vector<Request>	m_requests;
	eastl::vector<uint32_t> m_freeRequests;
	eastl::vector<Part>		m_queued; // Parts of ended queued requests, not yet issued.
	CopyBatches				m_copyBatches;
	uint32_t				m_current = ~0u; // Chunk currently being filled.
	uint64_t				m_frame = 0;
	uint64_t				m_admitted = 0; // Bytes admit() accepted this frame.
	uint64_t				m_frameBytes = 0;
	uint32_t				m_frameCopies = 0;
	Stats					m_stats;
};
} // namespace render_backend::gpu::upload
