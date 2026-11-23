#include <cstdio>
#include <render_backend/gpu/dx12_platform.hpp>
#include <render_backend/gpu/target_pool.hpp>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace render_backend::gpu;

namespace
{
constexpr uint32_t C_EARLY = 0, C_LATE = 1, C_OVERLAP = 2, C_OUTPUT = 3, C_HISTORY = 4;
constexpr uint64_t C_FRAME = 10;

struct PoolOwner
{
	TargetPool pool;
	~PoolOwner()
	{
		pool.release();
	}
};

class Messages final : public nvrhi::IMessageCallback
{
  public:
	void message(nvrhi::MessageSeverity severity, const char *text) override
	{
		std::fprintf(stderr, "NVRHI %d: %s\n", static_cast<int>(severity), text);
		failed |= severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal;
	}
	bool failed = false;
};

bool check(bool result, const char *message)
{
	if (!result)
		std::fprintf(stderr, "target pool: %s\n", message);
	return result;
}

graph::Frame declarations()
{
	using graph::bit;
	graph::Frame frame{};
	frame.persistent = bit(C_OUTPUT) | bit(C_HISTORY);
	frame.pass(1, 0, bit(C_EARLY));
	frame.pass(2, bit(C_EARLY), bit(C_OVERLAP) | bit(C_OUTPUT));
	frame.pass(3, bit(C_OVERLAP), bit(C_LATE) | bit(C_OUTPUT));
	frame.pass(4, bit(C_LATE), bit(C_OUTPUT));
	frame.pass(5, 0, bit(C_HISTORY));
	return frame;
}

uint32_t reserve(TargetPool &pool, nvrhi::IDevice *device, uint32_t resource, uint8_t owner, uint64_t frame = C_FRAME)
{
	return pool.takeTransient(device, nvrhi::Format::BGRA8_UNORM, 16, 16, owner, frame, "transient test", resource);
}

bool ordinaryReservations(nvrhi::IDevice *device)
{
	PoolOwner		   owner;
	TargetPool		  &pool = owner.pool;
	const graph::Frame frame = declarations();
	graph::Order	   order;
	graph::compile(frame, order);
	// A stale metadata copy must not let the visibly overlapping resource reuse a slot.
	order.life[C_OVERLAP] = {5, 5};
	if (!check(pool.setExecutionOrder(C_FRAME, frame, order), "could not install a compiled order"))
		return false;
	if (!check(!pool.setExecutionOrder(C_FRAME, frame, order), "the order could be rebound before taking"))
		return false;
	const auto later = reserve(pool, device, C_LATE, 1);
	if (!check(later != TargetPool::C_NONE, "could not allocate the later target"))
		return false;
	const auto original = pool.at(later).texture;
	const auto earlier = reserve(pool, device, C_EARLY, 2);
	if (!check(earlier == later && pool.at(earlier).texture.Get() == original.Get() && pool.stats().taken == 1 &&
				   pool.stats().sameFrameReuses == 1,
			   "out-of-order disjoint lifetimes did not reuse one whole texture"))
		return false;
	if (!check(!pool.setExecutionOrder(C_FRAME + 1, frame, order),
			   "the order could be rebound while targets were taken"))
		return false;

	const auto overlap = reserve(pool, device, C_OVERLAP, 3);
	const auto persistent = reserve(pool, device, C_HISTORY, 4);
	const auto unspecified = reserve(pool, device, 17, 5);
	const auto unknownEpoch = reserve(pool, device, C_EARLY, 6, C_FRAME + 1);
	if (!check(overlap != TargetPool::C_NONE && overlap != earlier && persistent != TargetPool::C_NONE &&
				   persistent != earlier && persistent != overlap && unspecified != TargetPool::C_NONE &&
				   unknownEpoch != TargetPool::C_NONE && pool.stats().taken == 5 && pool.stats().sameFrameReuses == 1,
			   "overlap, persistence, unspecified lifetime or another frame "
			   "reused an active target"))
		return false;
	const auto ordinary = pool.take(device, nvrhi::Format::BGRA8_UNORM, 16, 16, 7, C_FRAME, "exclusive test");
	if (!check(ordinary != TargetPool::C_NONE && pool.stats().taken == 6 && pool.stats().sameFrameReuses == 1,
			   "ordinary take did not reserve an exclusive texture"))
		return false;

	pool.endFrame(C_FRAME + 1);
	const auto noPlanA = reserve(pool, device, C_EARLY, 0, C_FRAME + 2);
	const auto noPlanB = reserve(pool, device, C_LATE, 1, C_FRAME + 2);
	return check(noPlanA != TargetPool::C_NONE && noPlanB != TargetPool::C_NONE && noPlanA != noPlanB &&
					 pool.stats().taken == 2 && pool.stats().sameFrameReuses == 1,
				 "endFrame did not clear the previous order");
}

bool cachedFramebuffer(nvrhi::IDevice *device)
{
	PoolOwner		   owner;
	TargetPool		  &pool = owner.pool;
	const graph::Frame frame = declarations();
	graph::Order	   order;
	graph::compile(frame, order);
	if (!check(pool.setExecutionOrder(C_FRAME, frame, order), "could not install order for the framebuffer test"))
		return false;
	const auto first = reserve(pool, device, C_EARLY, 1);
	if (!check(first != TargetPool::C_NONE, "could not allocate the framebuffer's target"))
		return false;
	const auto *target = pool.at(first).texture.Get();
	const auto	framebuffer =
		device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(pool.at(first).texture));
	if (!check(bool(framebuffer), "could not make a framebuffer holding the first target"))
		return false;
	pool.at(first).framebuffer = framebuffer;
	const auto second = reserve(pool, device, C_LATE, 2);
	if (!check(second != TargetPool::C_NONE && second != first && pool.stats().sameFrameReuses == 0 &&
				   pool.at(first).framebuffer.Get() == framebuffer.Get(),
			   "a cached framebuffer slot was shared"))
		return false;
	pool.release();
	const auto *retained = framebuffer->getDesc().colorAttachments[0].texture;
	return check(retained != nullptr && retained == target && retained->getDesc().width == 16,
				 "the external framebuffer lost the held target when the pool "
				 "released its slot");
}

bool uncertainOrder(nvrhi::IDevice *device)
{
	PoolOwner	   owner;
	TargetPool	  &pool = owner.pool;
	graph::Frame   frame = declarations();
	constexpr auto A = graph::bit(5), B = graph::bit(6);
	frame.persistent |= A | B;
	frame.pass(6, A, B);
	frame.pass(7, B, A);
	graph::Order order;
	graph::compile(frame, order);
	if (!check(!pool.setExecutionOrder(C_FRAME, frame, order) && order.cycles != 0,
			   "a cycle enabled sharing under a fallback order"))
		return false;
	const auto first = reserve(pool, device, C_EARLY, 1);
	const auto second = reserve(pool, device, C_LATE, 2);
	return check(first != TargetPool::C_NONE && second != TargetPool::C_NONE && first != second,
				 "a fallback order did not make disjoint-looking lifetimes exclusive");
}
} // namespace

int main()
{
	ComPtr<ID3D12Device> native;
	if (FAILED(dx12::createDevice(nullptr, D3D_FEATURE_LEVEL_12_0, native.GetAddressOf())))
		return 77;
	ComPtr<ID3D12CommandQueue> queue;
	if (FAILED(dx12::createDirectQueue(native.Get(), queue.GetAddressOf())))
		return 1;
	Messages				 messages;
	nvrhi::d3d12::DeviceDesc desc;
	desc.errorCB = &messages;
	desc.pDevice = native.Get();
	desc.pGraphicsCommandQueue = queue.Get();
	const auto device = nvrhi::d3d12::createDevice(desc);
	if (!device)
		return 1;
	const bool reserved = ordinaryReservations(device);
	const bool cached = cachedFramebuffer(device);
	const bool uncertain = uncertainOrder(device);
	return reserved && cached && uncertain && !messages.failed ? 0 : 1;
}
