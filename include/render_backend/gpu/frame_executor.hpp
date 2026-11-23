// Ordered submission of recorded graphics contexts and optional parallel command-list recording.
#pragma once
#include <render_backend/gpu/context.hpp>
#include <array>

namespace render_backend::gpu::upload
{
class Queue;
}

namespace render_backend::gpu::ctx
{
// Player that resolves reads through a ResourceTransitions tracker. execute() binds the executor's tracker; for direct
// playback outside execute(), call bindResourceTransitions first.
class ResourcePlayer : public Player
{
  public:
	void bindResourceTransitions(ResourceTransitions &transitions);
	void prepareReads(ResourceUses uses = {}) override;
	void written(nvrhi::ITexture *texture, nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources) override;

  private:
	ResourceTransitions *m_transitions = nullptr;
};

// Player for an auxiliary graphics list. A "window" is a run of sorted contexts recorded in parallel on auxiliary
// lists. Override application() to support host commands inside a window. Each list needs its own player instance,
// and host overrides must be safe to call concurrently.
class WindowPlayer : public ResourcePlayer
{
  public:
	void clear(const Clear &command) override;
	void staged(const StagedUpload &) override;
	void slot(uint8_t, uint8_t) override;
	void slotsEnd() override;
};

struct FrameExecution
{
	nvrhi::IDevice &device;
	ResourcePlayer &primary;
	// The upload queue that staged uploads into the primary list, if any. Submissions then go through it.
	upload::Queue		*uploads = nullptr;
	WindowPlayer *const *windowPlayers = nullptr;
	uint32_t			 windowPlayerCount = 0;
	// Minimum pending commands before playback is split across lists. Zero always plays serially on the primary list.
	// Marker ranges, bound contexts, rank 0 and dead ranks always stay on the primary list; canRecordInWindow can keep
	// further contexts there.
	uint32_t minimumCommands = 0;
	bool (*canRecordInWindow)(const CommandContext &, void *owner) = nullptr;
	void *policyOwner = nullptr;
	// Splitting submits the primary list mid-frame. Set to false when the caller cannot submit yet; playback is then
	// serial.
	bool allowSubmission = true;
};

enum class SubmissionFailure : uint8_t
{
	None,
	Primary,
	Auxiliary,
};

struct FrameExecutionResult
{
	uint64_t		  windows = 0;
	uint64_t		  lists = 0;
	uint64_t		  splits = 0;
	uint64_t		  allocationFallbacks = 0;
	SubmissionFailure submissionFailure = SubmissionFailure::None;
};

// Owns the reusable auxiliary graphics lists and their recording workers. Join all recording producers before
// executing the contexts, and keep recording paused until execution returns. A single caller executes one frame at
// a time. The primary list is already open and remains open on success. A submission failure leaves it closed,
// abandons remaining contexts and returns a failure instead of recording work that depends on the failed submit.
class FrameExecutor
{
  public:
	static constexpr uint32_t C_LISTS = PlayThreads::C_THREADS + 1;

	FrameExecutor() = default;
	FrameExecutor(const FrameExecutor &) = delete;
	FrameExecutor		&operator=(const FrameExecutor &) = delete;
	FrameExecutionResult execute(FrameContexts &contexts, const FrameExecution &frame);
	// Queues a closed graphics list to run before the next submit(). Fails if a prelude is already queued.
	bool setPrelude(nvrhi::ICommandList *list);
	// Submits `list`, preceded by the pending prelude, through `uploads` when given. Split playback uses it, and the
	// host should use it for its own final or synchronous submissions so the prelude is not skipped.
	uint64_t submit(nvrhi::IDevice &device, nvrhi::ICommandList *list, upload::Queue *uploads = nullptr);
	void discardPrelude(upload::Queue *uploads = nullptr);
	ResourceTransitions &resourceTransitions()
	{
		return m_primaryTransitions;
	}
	// Clears the previous frame's read intent. Pass carryAcrossLists = true before any direct or split recording in
	// this frame to carry read intent across command lists.
	void beginFrame(bool carryAcrossLists);
	// Call when the host reopens the primary list outside the executor, for example after a synchronous readback.
	void primaryReopened();
	// The device must still be alive when its command lists are released.
	void reset();

  private:
	bool allocateLists(nvrhi::IDevice &device, uint32_t count);
	nvrhi::CommandListHandle m_prelude;

	nvrhi::IDevice								 *m_device = nullptr;
	std::array<nvrhi::CommandListHandle, C_LISTS> m_lists;
	ResourceTransitions							  m_primaryTransitions;
	std::array<ResourceTransitions, C_LISTS>	  m_windowTransitions;
	PlayThreads									  m_threads;
};
} // namespace render_backend::gpu::ctx
