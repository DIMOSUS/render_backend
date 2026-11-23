// Pool of render targets reused across frames. Targets are keyed by (format, size). An owner (for example a viewport)
// takes a target for one frame, and all targets are returned at the end of the frame. Targets are created on demand;
// in later frames the owner that last used a target is offered it first. A target not taken for C_AGE frames is
// released. The table has a fixed size: when it is full, the target unused for longest is released to make room. If
// every target is taken, take() fails and the caller must render without a pooled target.
//
// Lifetime: targets are NVRHI handles. Recorded commands that name a target hold their own reference (for example a
// Clear's texture or a BeginPass's framebuffer) until playback or discard, so releasing a target never invalidates a
// pending command. NVRHI keeps resources alive while the GPU uses them.
#pragma once
#include <nvrhi/nvrhi.h>
#include <render_backend/gpu/frame_graph.hpp>
#include <array>
#include <stdint.h>

namespace render_backend::gpu
{
class TargetPool
{
  public:
	static constexpr uint32_t C_TARGETS = 16;
	static constexpr uint32_t C_AGE = 64; // Frames a target may go untaken before it is released.
	static constexpr uint32_t C_NONE = ~0u;
	struct Target
	{
		nvrhi::TextureHandle	 texture;
		nvrhi::FramebufferHandle framebuffer; // Owner's framebuffer for this target, created once by the caller.
		// Framebuffer combining this colour target with a depth target (`pair`) taken by the same owner. The caller
		// creates it the first time it sees the pair. Because an owner gets its previous targets back first, it only
		// needs to be recreated when the pool hands out a different depth target. `pair` is kept alive by the
		// framebuffer.
		nvrhi::FramebufferHandle pairFramebuffer;
		nvrhi::ITexture			*pair = nullptr;
		nvrhi::Format			 format = nvrhi::Format::UNKNOWN;
		uint32_t				 width = 0, height = 0;
		uint64_t				 bytes = 0;
		uint64_t				 lastFrame = 0; // Frame in which it was last taken.
		uint8_t					 owner = 0;		// Owner that last took it; preferred on its next take.
		bool					 taken = false;
	};
	// Takes a `width` x `height` target of `format` for this frame on behalf of `owner`. Prefers a free target the
	// owner had last, then any free target with the same key, then creates one. Depth targets are created with clear
	// value 1.0 in DepthWrite; colour targets in RenderTarget. The contents are undefined; the caller must clear them.
	// Returns C_NONE when every target is taken or creation failed (counted in Stats::fallbacks).
	uint32_t take(nvrhi::IDevice *device, nvrhi::Format format, uint32_t width, uint32_t height, uint8_t owner,
				  uint64_t frame, const char *name);

	// Installs the frame's compiled pass order. Call once per frame, before any take. The host must perform every
	// declared use in exactly that order on one graphics queue, and must not use a target outside its declared
	// lifetime.
	bool setExecutionOrder(uint64_t frame, const graph::Frame &declarations, const graph::Order &order);

	// Like take(), for frame graph resource `resource`. Two resources with the same key may share a texture when
	// their inclusive rank intervals do not overlap. Persistent resources, resources without a lifetime, and takes
	// outside the ordered frame are exclusive. The host must write or clear each logical target before reading it,
	// and must keep per-use framebuffers itself rather than in the shared slot. A slot with cached framebuffers or a
	// pair is never shared.
	uint32_t takeTransient(nvrhi::IDevice *device, nvrhi::Format format, uint32_t width, uint32_t height, uint8_t owner,
						   uint64_t frame, const char *name, uint32_t resource);
	Target	&at(uint32_t index)
	{
		return m_targets[index];
	}
	const Target &at(uint32_t index) const
	{
		return m_targets[index];
	}
	// Returns every target and releases those not taken for C_AGE frames.
	void endFrame(uint64_t frame);
	// Releases every target, for example on resize or shutdown.
	void release();

	struct Stats
	{
		uint32_t live = 0, taken = 0;		  // Existing targets, and targets taken this frame.
		uint32_t peakLive = 0, peakTaken = 0; // Peaks since creation.
		uint64_t made = 0, letGo = 0, fallbacks = 0;
		uint64_t sameFrameReuses = 0;
		uint64_t bytes = 0, peakBytes = 0; // Texel memory of live targets.
	};
	const Stats &stats() const
	{
		return m_stats;
	}

  private:
	static constexpr size_t C_RANK_WORDS = (graph::C_PASSES + 63) / 64;
	using RankMask = std::array<uint64_t, C_RANK_WORDS>;
	struct Reservation
	{
		RankMask occupied{};
		bool	 exclusive = false;
	};

	uint32_t	takeImpl(nvrhi::IDevice *device, nvrhi::Format format, uint32_t width, uint32_t height, uint8_t owner,
						 uint64_t frame, const char *name, const graph::Order::Life *lifetime);
	bool		canShare(uint32_t slot, uint64_t frame, const RankMask &ranks) const;
	void		letGo(Target &t);
	Target m_targets[C_TARGETS];
	Reservation m_reservations[C_TARGETS];
	graph::Order::Life m_lifetimes[graph::C_RESOURCES];
	uint64_t		   m_orderFrame = 0;
	bool			   m_orderSet = false;
	bool			   m_orderValid = false;
	Stats			   m_stats;
};
} // namespace render_backend::gpu
