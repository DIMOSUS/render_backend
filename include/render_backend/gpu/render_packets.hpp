// A bounded packet ring with one producer at a time and a dedicated render worker.
// Each Worker owns its queue, hand-over state and diagnostics. Packets carry copies of any data the
// producer may change after commit. Synchronize before accessing state also used by the execute callback.
#pragma once
#include <memory>
#include <stddef.h>
#include <stdint.h>

namespace render_backend::gpu::packets
{
using Execute = void (*)(void *owner, uint32_t kind, const uint8_t *data, uint32_t bytes);
// Called by sync on the producer once the worker has executed everything written so far.
using PostSync = void (*)(void *owner);

constexpr uint32_t C_MAX_PACKET = 8192;
constexpr uint32_t C_KINDS = 32; // packet kinds past this range count in the last slot

enum Site : uint32_t
{
	S_API,
	S_RECORD,
	S_TEXTURE,
	S_FILL,
	S_SAMPLER,
	S_CUBE,
	S_FULL,
	S_OBJECT,
	S_PRESENT,
	S_SITES
};

struct Stats
{
	uint64_t packets = 0;
	uint64_t bytes = 0;
	uint64_t kindPackets[C_KINDS] = {};
	uint64_t kindBytes[C_KINDS] = {};
	uint64_t syncs = 0;
	uint64_t sites[S_SITES] = {};
	uint32_t ringBytes = 0;
	uint32_t highWater = 0;
	int64_t	 workerTicks = 0, waitTicks = 0; // QueryPerformanceCounter ticks
	bool	 worker = false;				 // remains true after stop if the worker was running
};

class Worker
{
  public:
	Worker();
	~Worker();
	Worker(const Worker &) = delete;
	Worker &operator=(const Worker &) = delete;
	Worker(Worker &&) = delete;
	Worker &operator=(Worker &&) = delete;

	// Start and stop on the producer. Stop finishes pending work before releasing the thread and ring.
	// Ring positions remain monotonic across stop/start on the same Worker.
	void start(Execute execute, PostSync postSync, void *owner, const wchar_t *threadName = L"render worker");
	void stop();

	// Reserve a single packet of up to C_MAX_PACKET bytes and fill its 16-aligned data before committing.
	// Mark makes the packet's end visible to waitExecuted as soon as it runs.
	uint8_t *reserve(uint32_t kind, uint32_t bytes);
	void	 commit(bool mark = false);
	void	 publish();
	uint64_t written() const;
	void	 waitExecuted(uint64_t end);
	bool	 executed(uint64_t end) const;

	// Wait for all written packets, then call PostSync. No wait on this Worker's own execute thread.
	void	 sync(Site site = S_API);
	bool	 pending() const;
	uint64_t syncCalls() const;
	bool	 running() const;
	bool	 onWorker() const;
	Stats	 stats() const;
	void	 report(char *text, size_t size) const;
	int64_t	 waitTicks() const;
	int64_t	 waitTicksOfThread() const;

	// Hand the producer role to threadId (0 disables thread validation). The previous producer may
	// track that hand-over with besideFlight, but must keep out of the ring until the producer is joined.
	void	 setProducer(uint32_t threadId);
	bool	 producerElsewhere() const;
	bool	 onHandingThread() const;
	uint32_t besideFlight() const;
	void	 awaitHandOver();
	void	 publishPending();

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace render_backend::gpu::packets
