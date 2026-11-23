#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include <render_backend/gpu/dx12_platform.hpp>
#include <render_backend/gpu/frame_executor.hpp>
#include <render_backend/gpu/scatter_upload.hpp>
#include <render_backend/gpu/upload_queue.hpp>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace render_backend::gpu;

namespace
{
constexpr uint32_t C_TARGETS = 8, C_WORDS = 64;

class Messages final : public nvrhi::IMessageCallback
{
public:
	void message(nvrhi::MessageSeverity severity, const char *text) override
	{
		if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
		{
			std::fprintf(stderr, "NVRHI: %s\n", text);
			failed = true;
		}
	}
	bool failed = false;
};

struct Read
{
	nvrhi::IBuffer *source, *destination;
};

class Reader : public ctx::WindowPlayer
{
public:
	void application(const ctx::ApplicationCommand &, const uint8_t *payload) override
	{
		Read data;
		std::memcpy(&data, payload, sizeof data);
		list->copyBuffer(data.destination, 0, data.source, 0, sizeof(uint32_t) * C_WORDS);
	}
};

bool exercise(nvrhi::IDevice &device)
{
	uint32_t clear[C_WORDS], expected[C_TARGETS][C_WORDS];
	for (auto &word : clear)
		word = 0x12345678u;
	upload::ScatterBatch uploader;
	upload::ScatterBatch::Target targets[C_TARGETS];
	nvrhi::BufferHandle readbacks[C_TARGETS];
	for (uint32_t i = 0; i < C_TARGETS; ++i)
	{
		targets[i].buffer = device.createBuffer(nvrhi::BufferDesc().setByteSize(sizeof clear)
			.setCanHaveRawViews(true).setCanHaveUAVs(true).enableAutomaticStateTracking(nvrhi::ResourceStates::CopySource));
		targets[i].after = nvrhi::ResourceStates::CopySource;
		readbacks[i] = device.createBuffer(nvrhi::BufferDesc().setByteSize(sizeof clear)
			.setCpuAccess(nvrhi::CpuAccessMode::Read).enableAutomaticStateTracking(nvrhi::ResourceStates::CopyDest));
		if (!targets[i].buffer || !readbacks[i])
			return false;
	}
	if (!uploader.create(&device, targets, C_TARGETS, 2))
		return false;
	upload::Queue staging;
	if (!staging.create(&device, upload::Config{}))
		return false;
	const auto primary = device.createCommandList();
	const auto initialPrelude = device.createCommandList();
	if (!primary || !initialPrelude)
		return false;
	// Both lists have queue-owned staging. Their overlapping writes are intentionally ordered by submission.
	const uint32_t first = staging.begin(true), second = staging.begin(true);
	uint32_t overwritten[C_WORDS] = {};
	if (!staging.writeBuffer(first, targets[0].buffer, 0, overwritten, sizeof overwritten, targets[0].after) ||
		!staging.writeBuffer(second, targets[0].buffer, 0, clear, sizeof clear, targets[0].after))
		return false;
	staging.end(first);
	staging.end(second);
	initialPrelude->open();
	staging.issueTicket(initialPrelude, first);
	initialPrelude->close();
	primary->open();
	staging.issueTicket(primary, second);
	for (uint32_t i = 1; i < C_TARGETS; ++i)
		primary->writeBuffer(targets[i].buffer, clear, sizeof clear);
	primary->close();

	ctx::FrameExecutor executor;
	if (!executor.setPrelude(initialPrelude) || executor.submit(device, primary, &staging) == 0 || !device.waitForIdle())
		return false;
	staging.beginFrame(1);
	if (staging.stats().used != 0)
		return false;
	nvrhi::CommandListHandle invalid;
	if (!uploader.append(0, 0, clear, 4) || uploader.append(0, 1, clear, 4) ||
		uploader.record(0, nullptr, invalid) || invalid)
		return false;
	ctx::FrameContexts contexts;
	if (!contexts.create())
		return false;
	Reader root, windows[2];
	ctx::WindowPlayer *players[] = {&windows[0], &windows[1]};
	bool valid = true;
	for (uint32_t frame = 0; frame < 2 && valid; ++frame)
	{
		executor.beginFrame(true);
		for (uint32_t target = 0; target < C_TARGETS; ++target)
		{
			std::memcpy(expected[target], clear, sizeof clear);
			for (uint32_t count = 1; count <= 4; ++count)
			{
				const uint32_t index = count * 9;
				for (uint32_t i = 0; i < count; ++i)
					expected[target][index + i] = frame * 1000 + target * C_WORDS + index + i;
				valid &= uploader.append(target, index * 4, &expected[target][index], count * 4);
			}
		}
		nvrhi::CommandListHandle prelude;
		valid = valid && uploader.record(frame, nullptr, prelude) && prelude && executor.setPrelude(prelude);
		if (!valid)
			break;

		primary->open();
		root.list = primary;
		ctx::FrameExecution execution{device, root};
		execution.uploads = &staging;
		execution.minimumCommands = 1;
		execution.windowPlayers = players;
		execution.windowPlayerCount = 2;
		// Frame 0 only records the two consumers; frame 1 also submits them, which exercises the split path.
		execution.allowSubmission = frame != 0;
		for (uint32_t i = 0; i < 2 && valid; ++i)
		{
			auto *context = contexts.make(ctx::ContextKey::make(i + 1, i + 1), "scatter consumer");
			const Read read{targets[i].buffer, readbacks[i]};
			if (!context)
				valid = false;
			else if (context->add<ctx::ApplicationCommand>(&read, sizeof read))
				context->seal();
			else
			{
				context->cancel();
				valid = false;
			}
		}
		if (valid)
		{
			const auto result = executor.execute(contexts, execution);
			valid = result.submissionFailure == ctx::SubmissionFailure::None &&
					result.splits == frame && result.lists == 2 * frame;
		}
		if (valid)
		{
			for (uint32_t i = 2; i < C_TARGETS; ++i)
				primary->copyBuffer(readbacks[i], 0, targets[i].buffer, 0, sizeof clear);
			primary->close();
			valid = executor.submit(device, primary, &staging) != 0 && device.waitForIdle();
		}
		for (uint32_t i = 0; valid && i < C_TARGETS; ++i)
		{
			const void *data = device.mapBuffer(readbacks[i], nvrhi::CpuAccessMode::Read);
			valid = data && std::memcmp(data, expected[i], sizeof clear) == 0;
			if (data)
				device.unmapBuffer(readbacks[i]);
		}
		contexts.endFrame();
	}
	executor.discardPrelude(&staging);
	executor.reset();
	contexts.destroy();
	staging.destroy();
	return valid;
}
}

int main()
{
	ComPtr<ID3D12Device> native;
	if (FAILED(dx12::createDevice(nullptr, D3D_FEATURE_LEVEL_12_0, native.GetAddressOf())))
		return 77;
	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
	if (FAILED(native->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof shaderModel)) ||
		shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6)
		return 77;
	ComPtr<ID3D12CommandQueue> queue;
	if (FAILED(dx12::createDirectQueue(native.Get(), queue.GetAddressOf())))
		return 1;
	Messages messages;
	nvrhi::d3d12::DeviceDesc desc;
	desc.errorCB = &messages;
	desc.pDevice = native.Get();
	desc.pGraphicsCommandQueue = queue.Get();
	desc.enableHeapDirectlyIndexed = true;
	const auto device = nvrhi::validation::createValidationLayer(nvrhi::d3d12::createDevice(desc));
	if (!device || !exercise(*device) || messages.failed)
		return 1;
	std::puts("scatter upload: ordered serial and split consumers, exact partial writes and untouched gaps");
	return 0;
}
