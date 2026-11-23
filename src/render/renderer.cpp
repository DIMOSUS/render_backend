#include <render_backend/gpu/renderer.hpp>
#include <render_backend/gpu/frame_executor.hpp>
#include <render_backend/gpu/dx12_platform.hpp>
#include <render_backend/gpu/frame_graph.hpp>
#include <render_backend/gpu/render_packets.hpp>
#include <render_backend/gpu/upload_queue.hpp>
#include <render_backend/gpu/window_frame.hpp>
#include <render_backend/shader/shader_cache.hpp>
#include <wrl/client.h>
#include <stdio.h>

namespace render_backend::gpu::demo
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr uint32_t		C_BUFFERS = 3;
constexpr uint32_t		C_CONSTANT_BYTES = 256;
constexpr nvrhi::Format C_COLOR_FORMAT = nvrhi::Format::BGRA8_UNORM;

struct Messages final : nvrhi::IMessageCallback
{
	void message(nvrhi::MessageSeverity, const char *text) override
	{
		OutputDebugStringA(text);
		OutputDebugStringA("\n");
	}
};

struct VertexPlayer final : ctx::ResourcePlayer
{
	void clear(const ctx::Clear &) override
	{
	}
	void staged(const ctx::StagedUpload &) override
	{
	}
	void slot(uint8_t, uint8_t) override
	{
	}
	void slotsEnd() override
	{
	}
};

void recordClear(ctx::CommandContext &context, nvrhi::ITexture *texture, const float color[4], bool depth)
{
	auto &command = *context.add<ctx::ClearTexture>();
	command.texture = texture;
	command.texture->AddRef();
	command.subresources = nvrhi::AllSubresources;
	command.depth = depth ? 1u : 0u;
	memcpy(command.value, color, sizeof command.value);
}
} // namespace

struct Mesh::Storage
{
	nvrhi::BufferHandle buffer;
	uint32_t			vertices = 0;
};

Mesh::Mesh() = default;
Mesh::~Mesh() = default;
Mesh::Mesh(Mesh &&) noexcept = default;
Mesh &Mesh::operator=(Mesh &&) noexcept = default;
Mesh::operator bool() const
{
	return storage != nullptr;
}

struct Renderer::State
{
	Messages					  messages;
	HWND						  window = nullptr;
	ComPtr<ID3D12Device>		  native;
	ComPtr<ID3D12CommandQueue>	  queue;
	ComPtr<IDXGISwapChain3>		  swap;
	nvrhi::DeviceHandle			  device;
	nvrhi::CommandListHandle	  list;
	nvrhi::TextureHandle		  color[C_BUFFERS];
	nvrhi::TextureHandle		  depth;
	nvrhi::FramebufferHandle	  framebuffers[C_BUFFERS];
	nvrhi::BufferHandle			  constants;
	nvrhi::BindingSetHandle		  constantSet;
	nvrhi::GraphicsPipelineHandle pipeline;
	shader::ShaderCache			  shaders;
	ctx::FrameExecutor executor;
	ctx::FrameContexts			  contexts;
	upload::Queue				  uploads;
	std::string					  error, pipelineKey;
	uint32_t					  width = 0, height = 0;
	uint32_t					  frameNumber = 0;
	bool						  tearing = false;
	packets::Worker			  worker;
	bool						  presented = false;

	~State()
	{
		worker.stop();
		ctx::discard(contexts);
		contexts.destroy();
		if (device)
		{
			device->waitForIdle();
			device->runGarbageCollection();
		}
		uploads.discard(list);
		uploads.destroy();
	}

	bool fail(const char *reason)
	{
		error = reason;
		return false;
	}

	bool fail(const char *reason, HRESULT hr)
	{
		char message[256];
		snprintf(message, sizeof message, "%s (0x%08lx)", reason, static_cast<unsigned long>(hr));
		return fail(message);
	}

	bool resize(uint32_t w, uint32_t h)
	{
		if (width == w && height == h && framebuffers[C_BUFFERS - 1])
			return true;
		if (!swap)
		{
			const HRESULT hr =
				dx12::createFlipSwapChain(window, queue.Get(), w, h, C_BUFFERS, tearing, swap.GetAddressOf());
			if (FAILED(hr))
				return fail("CreateSwapChainForHwnd failed", hr);
		}
		else
		{
			device->waitForIdle();
			for (uint32_t i = 0; i < C_BUFFERS; ++i)
			{
				framebuffers[i] = nullptr;
				color[i] = nullptr;
			}
			device->runGarbageCollection();
			const HRESULT hr = swap->ResizeBuffers(C_BUFFERS, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
												   tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
			if (FAILED(hr))
				return fail("ResizeBuffers failed", hr);
		}
		depth = device->createTexture(nvrhi::TextureDesc()
										  .setWidth(w)
										  .setHeight(h)
										  .setFormat(nvrhi::Format::D32)
										  .setIsRenderTarget(true)
										  .setClearValue(nvrhi::Color(1.0f))
										  .setKeepInitialState(true)
										  .setInitialState(nvrhi::ResourceStates::DepthWrite));
		if (!depth)
			return fail("Cannot allocate a depth attachment");
		for (uint32_t i = 0; i < C_BUFFERS; ++i)
		{
			ComPtr<ID3D12Resource> image;
			const HRESULT		   hr = swap->GetBuffer(i, IID_PPV_ARGS(image.GetAddressOf()));
			if (FAILED(hr))
				return fail("GetBuffer failed", hr);
			color[i] =
				device->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(image.Get()),
													 nvrhi::TextureDesc()
														 .setWidth(w)
														 .setHeight(h)
														 .setFormat(C_COLOR_FORMAT)
														 .setIsRenderTarget(true)
														 .setKeepInitialState(true)
														 .setInitialState(nvrhi::ResourceStates::Present));
			if (!color[i])
				return fail("Cannot wrap a swap-chain image");
			framebuffers[i] = device->createFramebuffer(
				nvrhi::FramebufferDesc().addColorAttachment(color[i]).setDepthAttachment(depth));
			if (!framebuffers[i])
				return fail("Cannot create a swap-chain framebuffer");
		}
		width = w;
		height = h;
		return true;
	}

	bool selectPipeline(const char *effect, const char *techniqueName, const char *passName)
	{
		const std::string key = std::string(effect) + '/' + techniqueName + '/' + passName;
		if (pipeline && pipelineKey == key)
			return true;
		shader::EffectDesc effectDesc;
		eastl::string	   why;
		if (!shaders.loadEffect(effect, effectDesc, why))
			return fail(why.c_str());
		const shader::RegisterDesc *payloadRegister = nullptr;
		for (const shader::RegisterDesc &binding : effectDesc.registers)
		{
			if (binding.kind != 'b')
				continue;
			if (payloadRegister != nullptr)
				return fail("The vertex renderer requires exactly one automatically registered constant buffer");
			payloadRegister = &binding;
		}
		if (payloadRegister == nullptr)
			return fail("Declare one constant buffer without an explicit register to bind the frame payload");
		const auto			   *technique = effectDesc.findTechnique(techniqueName);
		const shader::PassDesc *pass = nullptr;
		if (technique)
			for (const auto &candidate : technique->passes)
				if (candidate.name == passName)
					pass = &candidate;
		if (!pass)
			return fail("No such effect pass");
		shader::PassState options;
		if (!effectDesc.resolve(*pass, shader::SelectionValues(), options, why))
			return fail(why.c_str());
		if (options.topology != shader::Topology::Triangles || options.blend != shader::BlendMode::None)
			return fail("The vertex renderer requires an unblended triangle-list pass");
		eastl::vector<uint8_t> vs, ps;
		shader::BuildEntry	   vsInfo, psInfo;
		if (!shaders.getShader(effect, techniqueName, passName, shader::Stage::Vertex, {}, vs, vsInfo,
							   shader::Path::Vertices))
			return fail(vsInfo.messages.c_str());
		if (!shaders.getShader(effect, techniqueName, passName, shader::Stage::Pixel, {}, ps, psInfo,
							   shader::Path::Vertices))
			return fail(psInfo.messages.c_str());
		const auto vertex = device->createShader(
			nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsInfo.entry.c_str()), vs.data(),
			vs.size());
		const auto pixel = device->createShader(
			nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psInfo.entry.c_str()), ps.data(),
			ps.size());
		if (!vertex || !pixel)
			return fail("Cannot create the effect's shaders");
		const nvrhi::VertexAttributeDesc attrs[] = {
			nvrhi::VertexAttributeDesc()
				.setName("POSITION")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(Vertex, position))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("NORMAL")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(Vertex, normal))
				.setElementStride(sizeof(Vertex)),
			nvrhi::VertexAttributeDesc()
				.setName("COLOR")
				.setFormat(nvrhi::Format::RGB32_FLOAT)
				.setOffset(offsetof(Vertex, color))
				.setElementStride(sizeof(Vertex)),
		};
		const auto input = device->createInputLayout(attrs, 3, vertex);
		if (!input)
			return fail("Cannot create the effect's vertex layout");
		nvrhi::RenderState renderState;
		const auto		   cull = options.cullMode == shader::CullMode::None	? nvrhi::RasterCullMode::None
								  : options.cullMode == shader::CullMode::Front ? nvrhi::RasterCullMode::Front
																				: nvrhi::RasterCullMode::Back;
		renderState.rasterState.setCullMode(cull);
		renderState.depthStencilState.setDepthTestEnable(options.depthTest)
			.setDepthWriteEnable(options.depthWrite)
			.setDepthFunc(nvrhi::ComparisonFunc(uint8_t(options.depthFunc) + 1));
		const auto newBindings = device->createBindingLayout(
			nvrhi::BindingLayoutDesc()
				.setVisibility(nvrhi::ShaderType::All)
				.setRegisterSpace(payloadRegister->space)
				.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(payloadRegister->index)));
		if (!newBindings)
			return fail("Cannot create the effect's constant buffer layout");
		const auto newConstantSet = device->createBindingSet(
			nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(payloadRegister->index, constants)),
			newBindings);
		if (!newConstantSet)
			return fail("Cannot bind the frame payload to the effect's constant buffer");
		const auto pending = device->createGraphicsPipeline(nvrhi::GraphicsPipelineDesc()
																.setVertexShader(vertex)
																.setPixelShader(pixel)
																.setInputLayout(input)
																.addBindingLayout(newBindings)
																.setRenderState(renderState),
															framebuffers[0]->getFramebufferInfo());
		if (!pending)
			return fail("Cannot create the effect's graphics pipeline");
		pipeline = pending;
		constantSet = newConstantSet;
		pipelineKey = key;
		return true;
	}

	bool record(const Mesh::Storage &mesh, const void *data, uint32_t bytes, const float clear[4])
	{
		const auto	 back = graph::bit(0);
		const auto	 z = graph::bit(1);
		graph::Frame graphFrame{};
		graphFrame.persistent = back;
		const uint32_t clearPass = graphFrame.pass(0, 0, back | z);
		const uint32_t drawPass = graphFrame.pass(1, back | z, back | z);
		graph::Order   order{};
		graph::compile(graphFrame, order);
		auto *clears = contexts.make(ctx::ContextKey::make(order.rank[clearPass], 0), "clear");
		auto *draw = contexts.make(ctx::ContextKey::make(order.rank[drawPass], 1), "draw");
		if (!clears || !draw || !clears->reserve(2 * ctx::commandBytes<ctx::ClearTexture>(0)) ||
			!draw->reserve(ctx::commandBytes<ctx::WriteBuffer>(C_CONSTANT_BYTES) +
						   ctx::commandBytes<ctx::BeginPass>(0) + ctx::commandBytes<ctx::SetPipeline>(0) +
						   ctx::commandBytes<ctx::Draw>(0) + ctx::commandBytes<ctx::EndPass>(0)))
		{
			contexts.endFrame();
			return fail("Frame command storage exhausted");
		}
		const UINT	image = swap->GetCurrentBackBufferIndex();
		const float depthClear[4] = {1.0f};
		recordClear(*clears, color[image], clear, false);
		recordClear(*clears, depth, depthClear, true);
		uint8_t constantBytes[C_CONSTANT_BYTES]{};
		memcpy(constantBytes, data, bytes);
		auto &write = *draw->add<ctx::WriteBuffer>(constantBytes, C_CONSTANT_BYTES);
		write.bytes = C_CONSTANT_BYTES;
		write.buffer = constants;
		auto &begin = *draw->add<ctx::BeginPass>();
		begin.framebuffer = framebuffers[image];
		begin.framebuffer->AddRef();
		begin.viewport[1] = float(width);
		begin.viewport[3] = float(height);
		begin.viewport[5] = 1.0f;
		auto &state = *draw->add<ctx::SetPipeline>();
		state.kind = ctx::PipelineKind::Graphics;
		state.pipeline = pipeline;
		state.sets[0] = constantSet;
		state.vertices = mesh.buffer;
		state.vertices->AddRef();
		draw->add<ctx::Draw>()->vertexCount = mesh.vertices;
		draw->add<ctx::EndPass>();
		clears->seal();
		draw->seal();
		return true;
	}

	void execute()
	{
		VertexPlayer player;
		player.list = list;
		list->open();
		executor.beginFrame(false);
		executor.execute(contexts, ctx::FrameExecution{*device, player});
		list->close();
		const uint64_t submission = uploads.submit(list);
		contexts.endFrame();
		if (submission == 0)
		{
			presented = false;
			fail("Cannot submit the frame");
			return;
		}
		const HRESULT hr = swap->Present(1, 0);
		presented = SUCCEEDED(hr);
		if (!presented)
			fail("Present failed", hr);
		device->runGarbageCollection();
	}
};

Renderer::Renderer() : state(std::make_unique<State>())
{
}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::create(void *hwnd, const char *shaderRoot, std::string &error)
{
	if (!hwnd || !shaderRoot)
	{
		error = "A window and shader path are required";
		return {};
	}
	auto   renderer = std::unique_ptr<Renderer>(new Renderer);
	State &state = *renderer->state;
	state.window = static_cast<HWND>(hwnd);
	HRESULT hr = dx12::createDevice(nullptr, D3D_FEATURE_LEVEL_12_0, state.native.GetAddressOf());
	if (SUCCEEDED(hr))
		hr = dx12::createDirectQueue(state.native.Get(), state.queue.GetAddressOf());
	if (FAILED(hr))
	{
		state.fail("Cannot create the D3D12 device or direct queue", hr);
		error = state.error;
		return {};
	}
	nvrhi::d3d12::DeviceDesc deviceDesc;
	deviceDesc.errorCB = &state.messages;
	deviceDesc.pDevice = state.native.Get();
	deviceDesc.pGraphicsCommandQueue = state.queue.Get();
	deviceDesc.enableHeapDirectlyIndexed = true;
	state.device = nvrhi::d3d12::createDevice(deviceDesc);
	if (!state.device || !state.contexts.create())
	{
		error = "Cannot allocate the NVRHI device or command contexts";
		return {};
	}
	state.list = state.device->createCommandList();
	state.constants = state.device->createBuffer(nvrhi::BufferDesc()
													 .setByteSize(C_CONSTANT_BYTES)
													 .setIsConstantBuffer(true)
													 .setIsVolatile(true)
													 .setMaxVersions(C_BUFFERS + 1));
	if (!state.list || !state.constants || !state.uploads.create(state.device, upload::Config{}))
	{
		error = "Cannot allocate the command list, constants, or staging queue";
		return {};
	}
	eastl::string why;
	if (!state.shaders.open(shaderRoot, shader::ShaderCache::Mode::Develop, why))
	{
		error = why.c_str();
		return {};
	}
	state.worker.start([](void *owner, uint32_t, const uint8_t *, uint32_t) { static_cast<State *>(owner)->execute(); },
				   nullptr, &state);
	return renderer;
}

Mesh Renderer::createMesh(const Vertex *vertices, uint32_t vertexCount)
{
	Mesh mesh;
	if (!vertices || vertexCount == 0)
		return mesh;
	const uint64_t size = uint64_t(vertexCount) * sizeof(Vertex);
	auto		   buffer = state->device->createBuffer(nvrhi::BufferDesc()
															.setByteSize(size)
															.setIsVertexBuffer(true)
															.setInitialState(nvrhi::ResourceStates::VertexBuffer)
															.setKeepInitialState(true));
	if (!buffer)
		return mesh;
	const uint32_t ticket = state->uploads.begin();
	if (ticket == 0)
		return mesh;
	const bool staged =
		state->uploads.writeBuffer(ticket, buffer, 0, vertices, size, nvrhi::ResourceStates::VertexBuffer);
	state->uploads.end(ticket);
	if (!staged)
		return mesh;
	state->list->open();
	state->uploads.issue(state->list);
	state->list->close();
	if (state->uploads.submit(state->list) == 0 || !state->device->waitForIdle() || FAILED(state->native->GetDeviceRemovedReason()))
		return mesh;
	state->device->runGarbageCollection();
	mesh.storage = std::make_unique<Mesh::Storage>();
	mesh.storage->buffer = buffer;
	mesh.storage->vertices = vertexCount;
	return mesh;
}

bool Renderer::frame(const Mesh &mesh, const char *effect, const char *technique, const char *pass,
					 const void *constants, uint32_t bytes, const float clear[4])
{
	if (!mesh.storage || !effect || !technique || !pass || !constants || bytes > C_CONSTANT_BYTES || !clear)
		return state->fail("Invalid frame parameters");
	uint32_t width, height;
	if (!window::clientSize(state->window, width, height))
		return true;
	if (!state->resize(width, height) || !state->selectPipeline(effect, technique, pass) ||
		!state->record(*mesh.storage, constants, bytes, clear))
		return false;
	state->uploads.beginFrame(++state->frameNumber);
	state->worker.reserve(0, 0);
	state->worker.commit();
	state->worker.sync();
	return state->presented;
}

const std::string &Renderer::lastError() const
{
	return state->error;
}
} // namespace render_backend::gpu::demo
