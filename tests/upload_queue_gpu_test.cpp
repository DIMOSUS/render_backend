#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <render_backend/gpu/dx12_platform.hpp>
#include <render_backend/gpu/upload_queue.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace render_backend::gpu;

namespace
{
constexpr uint64_t C_CHUNK_BYTES = 1024 * 1024;
constexpr uint32_t C_EXTRA_FRAMES = 64;
constexpr DWORD	   C_WAIT_MS = 5000;
constexpr uint8_t  C_ORIGINAL = 0x31, C_COMPETING = 0xd9, C_REUSED = 0x5e;

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

class Completion
{
  public:
	~Completion()
	{
		if (m_event != nullptr)
			CloseHandle(m_event);
	}
	bool create(ID3D12Device &device)
	{
		m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return m_event != nullptr &&
			   SUCCEEDED(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
	}
	uint64_t signal(ID3D12CommandQueue &queue)
	{
		const uint64_t value = ++m_value;
		return SUCCEEDED(queue.Signal(m_fence.Get(), value)) ? value : 0;
	}
	bool pending(uint64_t value) const
	{
		return value != 0 && m_fence->GetCompletedValue() < value;
	}
	bool wait(uint64_t value)
	{
		if (value == 0)
			return false;
		if (pending(value) && (FAILED(m_fence->SetEventOnCompletion(value, m_event)) ||
							   WaitForSingleObject(m_event, C_WAIT_MS) != WAIT_OBJECT_0))
			return false;
		const uint64_t done = m_fence->GetCompletedValue();
		return done != UINT64_MAX && done >= value;
	}

  private:
	ComPtr<ID3D12Fence> m_fence;
	HANDLE				m_event = nullptr;
	uint64_t			m_value = 0;
};

class GraphicsGate
{
  public:
	~GraphicsGate()
	{
		release();
	}
	bool create(ID3D12Device &device)
	{
		return SUCCEEDED(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.GetAddressOf())));
	}
	bool block(ID3D12CommandQueue &queue)
	{
		m_active = SUCCEEDED(queue.Wait(m_fence.Get(), 1));
		return m_active;
	}
	bool release()
	{
		if (m_active && FAILED(m_fence->Signal(1)))
			return false;
		m_active = false;
		return true;
	}

  private:
	ComPtr<ID3D12Fence> m_fence;
	bool				m_active = false;
};

struct UnlockOnExit
{
	GraphicsGate &gate;
	~UnlockOnExit()
	{
		gate.release();
	}
};

bool check(bool condition, const char *message)
{
	if (!condition)
		std::fprintf(stderr, "upload queue: %s\n", message);
	return condition;
}

void advance(upload::Queue &staging, uint64_t &frame)
{
	for (uint32_t i = 0; i < C_EXTRA_FRAMES; ++i)
		staging.beginFrame(++frame);
}

bool exercise(nvrhi::IDevice &device, ID3D12CommandQueue &nativeQueue, upload::Queue &staging, GraphicsGate &gate,
			  Completion &done)
{
	const auto dest = device.createBuffer(
		nvrhi::BufferDesc().setByteSize(C_CHUNK_BYTES).enableAutomaticStateTracking(nvrhi::ResourceStates::CopySource));
	const auto readback = device.createBuffer(nvrhi::BufferDesc()
												  .setByteSize(C_CHUNK_BYTES)
												  .setCpuAccess(nvrhi::CpuAccessMode::Read)
												  .enableAutomaticStateTracking(nvrhi::ResourceStates::CopyDest));
	const auto commands = device.createCommandList();
	const auto unrelated = device.createCommandList();
	const UnlockOnExit unlock{gate};
	if (!check(dest && readback && commands && unrelated, "failed to allocate test resources"))
		return false;

	uint64_t frame = 1;
	staging.beginFrame(frame);
	const uint32_t ticket = staging.begin();
	uint8_t *const first = staging.buffer(ticket, dest, 0, C_CHUNK_BYTES, nvrhi::ResourceStates::CopySource);
	if (!check(first != nullptr, "failed to stage the original request"))
		return false;
	std::memset(first, C_ORIGINAL, C_CHUNK_BYTES);
	staging.end(ticket);
	advance(staging, frame);
	if (!check(staging.pending() && staging.stats().used == C_CHUNK_BYTES, "queued bytes were retired"))
		return false;

	commands->open();
	const bool issued = staging.issue(commands);
	if (issued)
		commands->copyBuffer(readback, 0, dest, 0, C_CHUNK_BYTES);
	commands->close();
	if (!check(issued, "queued request was not recorded"))
		return false;
	advance(staging, frame);
	if (!check(!staging.pending() && staging.stats().used == C_CHUNK_BYTES, "unsubmitted recording was retired"))
		return false;

	unrelated->open();
	unrelated->close();
	if (!check(staging.submit(unrelated) != 0 && done.wait(done.signal(nativeQueue)),
			   "unrelated command list did not complete"))
		return false;
	advance(staging, frame);
	if (!check(staging.stats().used == C_CHUNK_BYTES, "unrelated submission retired another list's bytes"))
		return false;

	if (!check(gate.block(nativeQueue) && staging.submit(commands) != 0, "cannot submit behind the graphics gate"))
		return false;
	const uint64_t submitted = done.signal(nativeQueue);
	advance(staging, frame);
	if (!check(done.pending(submitted) && staging.stats().used == C_CHUNK_BYTES,
			   "submitted bytes retired while their GPU work was blocked"))
		return false;

	const uint32_t competing = staging.begin(true);
	uint8_t *const second = staging.buffer(competing, dest, 0, C_CHUNK_BYTES, nvrhi::ResourceStates::CopySource);
	if (!check(second != nullptr && second != first && staging.stats().used == 2 * C_CHUNK_BYTES,
			   "competing upload reused a pinned chunk"))
		return false;
	std::memset(second, C_COMPETING, C_CHUNK_BYTES);
	if (!check(gate.release() && done.wait(submitted), "blocked upload/readback did not finish"))
		return false;

	staging.beginFrame(++frame);
	if (!check(staging.stats().used == C_CHUNK_BYTES, "completed work did not retire its chunk"))
		return false;
	const uint32_t reuse = staging.begin();
	uint8_t *const recycled = staging.buffer(reuse, dest, 0, C_CHUNK_BYTES, nvrhi::ResourceStates::CopySource);
	if (!check(recycled == first && staging.stats().chunksMade == 2, "retired memory was not reused"))
		return false;
	std::memset(recycled, C_REUSED, C_CHUNK_BYTES);
	staging.cancel(reuse);
	staging.cancel(competing);
	staging.beginFrame(++frame);
	if (!check(!staging.pending() && staging.stats().used == 0, "canceled requests kept unsubmitted staging pinned"))
		return false;

	const auto *bytes = static_cast<const uint8_t *>(device.mapBuffer(readback, nvrhi::CpuAccessMode::Read));
	if (!check(bytes != nullptr, "cannot read back the uploaded contents"))
		return false;
	bool matches = true;
	for (uint64_t i = 0; matches && i < C_CHUNK_BYTES; ++i)
		matches = bytes[i] == C_ORIGINAL;
	device.unmapBuffer(readback);
	if (!check(matches, "GPU copy contains bytes from a later allocation"))
		return false;

	const auto	   abandoned = device.createCommandList();
	const uint32_t discard = staging.begin();
	if (!check(abandoned && staging.buffer(discard, dest, 0, C_CHUNK_BYTES, nvrhi::ResourceStates::CopySource),
			   "cannot stage a recording to abandon"))
		return false;
	staging.end(discard);
	abandoned->open();
	const bool recorded = staging.issue(abandoned);
	abandoned->close();
	if (!check(recorded, "request to abandon was not recorded"))
		return false;
	staging.discard(abandoned);
	staging.beginFrame(++frame);
	return check(staging.stats().used == 0, "discarded recording kept staging pinned");
}
} // namespace

int main()
{
	ComPtr<ID3D12Device> native;
	if (FAILED(dx12::createDevice(nullptr, D3D_FEATURE_LEVEL_12_0, native.GetAddressOf())))
	{
		std::fputs("No D3D12 feature-level 12.0 adapter available\n", stderr);
		return 77;
	}
	ComPtr<ID3D12CommandQueue> nativeQueue;
	if (FAILED(dx12::createDirectQueue(native.Get(), nativeQueue.GetAddressOf())))
		return 1;
	Messages				 messages;
	nvrhi::d3d12::DeviceDesc desc;
	desc.errorCB = &messages;
	desc.pDevice = native.Get();
	desc.pGraphicsCommandQueue = nativeQueue.Get();
	const auto device = nvrhi::d3d12::createDevice(desc);
	if (!device)
		return 1;
	Completion	 done;
	GraphicsGate gate;
	if (!done.create(*native.Get()) || !gate.create(*native.Get()))
		return 1;
	upload::Config config;
	config.chunkBytes = C_CHUNK_BYTES;
	config.idleFrames = C_EXTRA_FRAMES * 4;
	upload::Queue staging;
	if (!staging.create(device, config))
		return 1;
	const bool passed = exercise(*device, *nativeQueue.Get(), staging, gate, done);
	// Unlock even after an early check fails. Avoid a blocking queue destructor on a failed bounded wait.
	if (!gate.release() || !done.wait(done.signal(*nativeQueue.Get())))
	{
		std::fputs("upload queue: GPU cleanup did not finish\n", stderr);
		std::_Exit(1);
	}
	staging.destroy();
	device->runGarbageCollection();
	if (passed && !messages.failed)
		std::puts("upload queue: staging stayed pinned until submission completion, then was reused");
	return passed && !messages.failed ? 0 : 1;
}
