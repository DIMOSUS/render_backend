// Render packet ring and render worker thread.
#include <render_backend/gpu/render_packets.hpp>
#include "platform.hpp"
#include "checked.hpp"
#include <render_backend/gpu/memory.hpp>
#include <atomic>
#include <intrin.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace render_backend::gpu::packets
{
namespace
{

// Fixed ring size. When the ring is full, the producer syncs until the worker has drained it.
constexpr uint32_t C_RING = 8u << 20;
constexpr uint32_t C_WRAP = 0xffffffffu; // Packet kind of the filler that skips to the start of the ring.
// The producer publishes packets to the worker in batches of at least C_PUBLISH bytes, or at a sync, so the worker
// wakes once per batch rather than per packet. C_WORKER_SPIN is how many pauses the worker spins for new work before
// sleeping; C_SYNC_SPIN is how many pauses a sync spins before waiting on the done event.
constexpr uint32_t C_PUBLISH = 32768;
constexpr uint32_t C_WORKER_SPIN = 256;
constexpr uint32_t C_SYNC_SPIN = 4000;

struct Header
{
	uint32_t kind;
	uint32_t bytes; // Payload size.
	uint32_t total; // Header plus payload, rounded up to 16; the distance to the next packet.
	uint32_t pad;	// Non-zero for a marked packet (commit(true)).
};
static_assert(sizeof(Header) == 16, "packet header");

int64_t now()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart;
}

uint64_t callerIdentity()
{
	// A native thread ID can be reused after a producer exits.
	static std::atomic<uint64_t> next{0};
	thread_local const uint64_t	 identity = next.fetch_add(1, std::memory_order_relaxed);
	return identity;
}
} // namespace

struct Worker::State
{
	uint8_t *bytes = nullptr;
	Execute	 execute = nullptr;
	PostSync postSync = nullptr;
	void	*owner = nullptr;
	// Monotonic byte positions; the ring offset is position % C_RING. `write` is the producer's position, `published`
	// is how far the worker may execute, and `read` is how far it has executed. Positions written by different threads
	// are on separate cache lines.
	uint64_t write = 0;
	uint64_t reserved = 0; // End of the packet between reserve() and commit().
	alignas(64) std::atomic<uint64_t> published{0};
	alignas(64) std::atomic<uint64_t> read{0};
	// Worker thread state.
	alignas(64) std::atomic<bool> sleeping{false};
	std::atomic<bool> waiting{false}, stopping{false};
	HANDLE			  thread = nullptr;
	HANDLE			  work = nullptr, done = nullptr; // Auto-reset events.
	// Statistics.
	Stats stats;
	alignas(64) std::atomic<int64_t> workerTicks{0}; // Written by the worker; on its own cache line.
	uint8_t				  workerPad[56] = {};
	uint64_t			  syncCalls = 0;
	std::atomic<uint32_t> producer{0};
	std::atomic<uint32_t> handingThread{0};
	std::atomic<uint32_t> flight{0};
	std::atomic<uint64_t> handOverEnd{0};

	static thread_local const State *executing;
	struct ThreadWait
	{
		uint64_t	caller;
		int64_t		ticks;
		ThreadWait *next;
	};
	mutable SRWLOCK threadWaitLock = SRWLOCK_INIT;
	ThreadWait	   *threadWaits = nullptr;

	~State()
	{
		while (threadWaits != nullptr)
		{
			ThreadWait *next = threadWaits->next;
			heapFree(threadWaits);
			threadWaits = next;
		}
	}

	bool onWorker() const
	{
		return executing == this;
	}
	void enterProducer() const
	{
#if RENDER_BACKEND_RUNTIME_CHECKS
		const uint32_t current = producer.load(std::memory_order_acquire);
		if (current != 0 && !onWorker() && current != GetCurrentThreadId())
		{
			fprintf(stderr, "render packets entered by thread %lu while thread %u writes them\n", GetCurrentThreadId(),
					current);
			abort();
		}
#endif
	}

	// Keep the calling thread's count separate without allocating through the host.
	void countThreadWait(int64_t waited)
	{
		const uint64_t caller = callerIdentity();
		AcquireSRWLockExclusive(&threadWaitLock);
		ThreadWait *entry = threadWaits;
		while (entry != nullptr && entry->caller != caller)
			entry = entry->next;
		if (entry == nullptr)
		{
			if (void *storage = heapAlloc(sizeof(ThreadWait)))
			{
				entry = ::new (storage) ThreadWait{caller, 0, threadWaits};
				threadWaits = entry;
			}
		}
		if (entry != nullptr)
			entry->ticks += waited;
		ReleaseSRWLockExclusive(&threadWaitLock);
	}

	int64_t threadWait() const
	{
		const uint64_t caller = callerIdentity();
		AcquireSRWLockShared(&threadWaitLock);
		const ThreadWait *entry = threadWaits;
		while (entry != nullptr && entry->caller != caller)
			entry = entry->next;
		const int64_t ticks = entry != nullptr ? entry->ticks : 0;
		ReleaseSRWLockShared(&threadWaitLock);
		return ticks;
	}

	void				run(uint64_t end);
	void				waitRead(uint64_t end);
	static DWORD WINAPI worker(void *state);
};

thread_local const Worker::State *Worker::State::executing = nullptr;

Worker::Worker() : m_state(std::make_unique<State>())
{
}

Worker::~Worker()
{
	stop();
}

// Executes packets in order from `read` up to `end`.
void Worker::State::run(uint64_t end)
{
	State	&r = *this;
	uint64_t pos = r.read.load(std::memory_order_relaxed);
	while (pos < end)
	{
		const Header *h = reinterpret_cast<const Header *>(r.bytes + pos % C_RING);
		if (h->kind != C_WRAP)
			r.execute(r.owner, h->kind, reinterpret_cast<const uint8_t *>(h + 1), h->bytes);
		pos += h->total;
		if (h->pad != 0 && pos != end)
		{
			// Marked packet: publish its end now so waitExecuted can return early.
			r.read.store(pos, std::memory_order_seq_cst);
			if (r.waiting.load(std::memory_order_seq_cst))
				SetEvent(r.done);
		}
	}
	r.read.store(pos, std::memory_order_seq_cst);
	if (r.waiting.load(std::memory_order_seq_cst))
		SetEvent(r.done);
}

// Producer side: makes everything written so far available to the worker.
void Worker::publish()
{
	State &r = *m_state;
	r.published.store(r.write, std::memory_order_seq_cst);
	if (r.sleeping.load(std::memory_order_seq_cst))
		SetEvent(r.work);
}

DWORD WINAPI Worker::State::worker(void *state)
{
	State &r = *static_cast<State *>(state);
	executing = &r;
	for (;;)
	{
		uint64_t end = r.published.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < C_WORKER_SPIN && r.read.load(std::memory_order_relaxed) == end && !r.stopping.load();
			 ++i)
		{
			_mm_pause();
			end = r.published.load(std::memory_order_acquire);
		}
		if (r.read.load(std::memory_order_relaxed) == end)
		{
			if (r.stopping.load())
				return 0;
			r.sleeping.store(true);
			if (r.read.load() == r.published.load() && !r.stopping.load())
				WaitForSingleObject(r.work, INFINITE);
			r.sleeping.store(false);
			continue;
		}
		const int64_t t0 = now();
		r.run(end);
		r.workerTicks.fetch_add(now() - t0, std::memory_order_relaxed);
	}
}

// Producer side: waits until the worker has executed everything before `end`, which must already be published. Spins
// briefly, then waits on the done event.
void Worker::State::waitRead(uint64_t end)
{
	State &r = *this;
	for (uint32_t i = 0; i < C_SYNC_SPIN && r.read.load(std::memory_order_acquire) < end; ++i)
		_mm_pause();
	if (r.read.load(std::memory_order_acquire) >= end)
		return;
	r.waiting.store(true, std::memory_order_seq_cst);
	while (r.read.load(std::memory_order_seq_cst) < end)
	{
		if (r.sleeping.load())
			SetEvent(r.work);
		WaitForSingleObject(r.done, 1);
	}
	r.waiting.store(false);
}

void Worker::start(Execute execute, PostSync postSync, void *owner, const wchar_t *threadName)
{
	State &r = *m_state;
	if (r.bytes != nullptr)
		stop();
	r.bytes = static_cast<uint8_t *>(heapAlloc(C_RING));
	if (r.bytes == nullptr)
		abort();
	r.execute = execute;
	r.postSync = postSync;
	r.owner = owner;
	// Positions continue from a previous start/stop cycle, so end positions recorded earlier (for example a hand-over
	// end) stay behind the new read position and count as executed.
	r.reserved = r.write;
	r.published.store(r.write);
	r.read.store(r.write);
	r.stats = Stats{};
	r.stats.ringBytes = C_RING;
	r.workerTicks.store(0);
	r.stopping.store(false);
	r.work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	r.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	r.thread = CreateThread(nullptr, 0, State::worker, &r, 0, nullptr);
	if (r.work == nullptr || r.done == nullptr || r.thread == nullptr)
		abort(); // The worker is the only executor of packets; nothing can render without it.
	if (threadName != nullptr)
		SetThreadDescription(r.thread, threadName);
	r.stats.worker = true;
}

void Worker::stop()
{
	State &r = *m_state;
	if (r.bytes == nullptr)
		return;
	sync();
	if (r.thread != nullptr)
	{
		r.stopping.store(true);
		SetEvent(r.work);
		WaitForSingleObject(r.thread, INFINITE);
		CloseHandle(r.thread);
		CloseHandle(r.work);
		CloseHandle(r.done);
		r.thread = r.work = r.done = nullptr;
	}
	heapFree(r.bytes);
	r.bytes = nullptr;
	r.execute = nullptr;
	r.postSync = nullptr;
	r.owner = nullptr;
}

uint8_t *Worker::reserve(uint32_t kind, uint32_t bytes)
{
	State &r = *m_state;
	r.enterProducer();
	if (bytes > C_MAX_PACKET)
		abort(); // Contract violation: packets are at most C_MAX_PACKET bytes.
	const uint32_t total = (uint32_t(sizeof(Header)) + bytes + 15u) & ~15u;
	// Packets are contiguous. If one does not fit before the end of the ring, a wrap packet skips the remainder.
	uint64_t	   at = r.write;
	const uint32_t offset = uint32_t(at % C_RING);
	const uint32_t skip = offset + total > C_RING ? C_RING - offset : 0u;
	if (at + skip + total - r.read.load(std::memory_order_acquire) > C_RING)
	{
		sync(S_FULL); // Ring full: drain it first.
		at = r.write;
	}
	if (skip != 0)
	{
		Header *w = reinterpret_cast<Header *>(r.bytes + offset);
		w->kind = C_WRAP;
		w->bytes = 0;
		w->total = skip;
		w->pad = 0;
		at += skip;
	}
	Header *h = reinterpret_cast<Header *>(r.bytes + at % C_RING);
	h->kind = kind;
	h->bytes = bytes;
	h->total = total;
	h->pad = 0;
	r.reserved = at + total;
	r.write = at; // The wrap packet is published together with this packet.
	return reinterpret_cast<uint8_t *>(h + 1);
}

void Worker::commit(bool mark)
{
	State  &r = *m_state;
	Header &h = *reinterpret_cast<Header *>(r.bytes + r.write % C_RING);
	if (mark)
		h.pad = 1;
	const uint64_t total = r.reserved - r.write;
	const uint32_t kind = h.kind < C_KINDS ? h.kind : C_KINDS - 1;
	r.stats.bytes += total;
	r.stats.kindBytes[kind] += total;
	++r.stats.kindPackets[kind];
	r.write = r.reserved;
	++r.stats.packets;
	const uint64_t waitingBytes = r.write - r.read.load(std::memory_order_relaxed);
	if (waitingBytes > r.stats.highWater)
		r.stats.highWater = uint32_t(waitingBytes);
	if (r.write - r.published.load(std::memory_order_relaxed) >= C_PUBLISH)
		publish();
}

void Worker::sync(Site site)
{
	State &r = *m_state;
	if (r.onWorker())
		return;
	r.enterProducer(); // Checked even when the worker is not running, since the producer still owns the state.
	if (r.bytes == nullptr)
		return;
	++r.syncCalls;
	const uint64_t end = r.write;
	if (r.read.load(std::memory_order_acquire) == end)
	{
		if (r.postSync != nullptr)
			r.postSync(r.owner);
		return;
	}
	publish();
	++r.stats.syncs;
	++r.stats.sites[site];
	const int64_t t0 = now();
	r.waitRead(end);
	const int64_t waited = now() - t0;
	r.stats.waitTicks += waited;
	r.countThreadWait(waited);
	if (r.postSync != nullptr)
		r.postSync(r.owner);
}

uint64_t Worker::written() const
{
	return m_state->write;
}

void Worker::waitExecuted(uint64_t end)
{
	State &r = *m_state;
	if (r.onWorker())
		return;
	r.enterProducer();
	if (r.bytes == nullptr || r.read.load(std::memory_order_acquire) >= end)
		return;
	publish();
	++r.stats.syncs;
	++r.stats.sites[S_PRESENT];
	const int64_t t0 = now();
	r.waitRead(end);
	const int64_t waited = now() - t0;
	r.stats.waitTicks += waited;
	r.countThreadWait(waited);
}

bool Worker::pending() const
{
	const State &r = *m_state;
	return r.bytes != nullptr && r.read.load(std::memory_order_acquire) != r.write;
}

uint64_t Worker::syncCalls() const
{
	return m_state->syncCalls;
}

bool Worker::running() const
{
	return m_state->thread != nullptr;
}

bool Worker::onWorker() const
{
	return m_state->onWorker();
}

Stats Worker::stats() const
{
	Stats s = m_state->stats;
	s.workerTicks = m_state->workerTicks.load();
	return s;
}

void Worker::report(char *text, size_t size) const
{
	using ull = unsigned long long;
	const Stats	  s = stats();
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	sprintf_s(
		text, size,
		"packets %llu bytes %llu syncs %llu (api %llu record %llu texture %llu fill %llu sampler %llu cube %llu full "
		"%llu object %llu present %llu) ring %u peak %u worker %d busyMs %.1f waitMs %.1f",
		ull(s.packets), ull(s.bytes), ull(s.syncs), ull(s.sites[0]), ull(s.sites[1]), ull(s.sites[2]), ull(s.sites[3]),
		ull(s.sites[4]), ull(s.sites[5]), ull(s.sites[6]), ull(s.sites[7]), ull(s.sites[8]), unsigned(s.ringBytes),
		unsigned(s.highWater), s.worker ? 1 : 0, double(s.workerTicks) * 1000.0 / double(f.QuadPart),
		double(s.waitTicks) * 1000.0 / double(f.QuadPart));
	// Per kind, as kind:packets/bytes.
	size_t		used = strlen(text);
	const char *lead = " kinds ";
	for (uint32_t k = 0; k < C_KINDS && used + 48 < size; ++k)
		if (s.kindPackets[k] != 0)
		{
			const int n = sprintf_s(text + used, size - used, "%s%u:%llu/%llu", lead, k, ull(s.kindPackets[k]),
									ull(s.kindBytes[k]));
			used += n > 0 ? size_t(n) : 0;
			lead = " ";
		}
}

int64_t Worker::waitTicks() const
{
	return m_state->stats.waitTicks;
}

int64_t Worker::waitTicksOfThread() const
{
	return m_state->threadWait();
}

void Worker::setProducer(uint32_t threadId)
{
	State &r = *m_state;
	if (threadId != 0)
	{
		r.handingThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		r.flight.fetch_add(1, std::memory_order_relaxed);
		r.handOverEnd.store(r.write, std::memory_order_release);
		if (r.bytes != nullptr)
			publish();
	}
	r.producer.store(threadId, std::memory_order_release);
}

uint32_t Worker::besideFlight() const
{
	const State	  &r = *m_state;
	const uint32_t producer = r.producer.load(std::memory_order_acquire);
	const uint32_t handing = r.handingThread.load(std::memory_order_relaxed);
	if (producer == 0 || r.onWorker() || handing != GetCurrentThreadId() || producer == handing)
		return 0;
	return r.flight.load(std::memory_order_relaxed);
}

bool Worker::onHandingThread() const
{
	return m_state->handingThread.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

void Worker::awaitHandOver()
{
	State		  &r = *m_state;
	const uint64_t end = r.handOverEnd.load(std::memory_order_acquire);
	if (r.bytes != nullptr && !r.onWorker() && r.read.load(std::memory_order_acquire) < end)
	{
		publish();
		r.waitRead(end); // Not counted in the sync statistics.
	}
}

bool Worker::executed(uint64_t end) const
{
	return m_state->bytes == nullptr || m_state->read.load(std::memory_order_acquire) >= end;
}

bool Worker::producerElsewhere() const
{
	const uint32_t producer = m_state->producer.load(std::memory_order_acquire);
	return producer != 0 && !onWorker() && producer != GetCurrentThreadId();
}

void Worker::publishPending()
{
	if (m_state->bytes != nullptr && !onWorker())
		publish();
}
} // namespace render_backend::gpu::packets
