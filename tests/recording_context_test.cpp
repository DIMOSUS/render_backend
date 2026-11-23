#include <array>
#include <atomic>
#include <cstdio>
#include <render_backend/gpu/context.hpp>
#include <thread>

using namespace render_backend::gpu::ctx;

namespace {
bool check(bool ok, const char *what) {
  if (!ok)
    std::fprintf(stderr, "recording context: %s\n", what);
  return ok;
}

struct FrameOwner {
  FrameContexts frame;
  ~FrameOwner() {
    discard(frame);
    frame.destroy();
  }
};

template <size_t Count = 4> class Capture final : public Player {
public:
  std::array<uint32_t, Count> values{};
  uint32_t count = 0;
  bool invalid = false;
  TextureUse textureUse{};
  BufferUse bufferUse{};
  uint32_t declarations = 0;

  void application(const ApplicationCommand &command,
                   const uint8_t *payload) override {
    if (command.header.size !=
            commandBytes<ApplicationCommand>(sizeof(uint32_t)) ||
        count == values.size()) {
      invalid = true;
      return;
    }
    memcpy(&values[count++], payload, sizeof(uint32_t));
  }
  void clear(const Clear &) override { invalid = true; }
  void prepareReads(ResourceUses uses) override {
    if (++declarations != 1 || uses.textureCount != 1 || uses.bufferCount != 1) {
      invalid = true;
      return;
    }
    textureUse = uses.textures[0];
    bufferUse = uses.buffers[0];
  }
  void written(nvrhi::ITexture *, nvrhi::TextureSubresourceSet) override {
    invalid = true;
  }
  void staged(const StagedUpload &) override { invalid = true; }
  void slot(uint8_t, uint8_t) override { invalid = true; }
  void slotsEnd() override { invalid = true; }
};

bool put(FrameContexts &frame, uint16_t rank, uint64_t order, uint32_t value,
         uint8_t view = 0, uint8_t pass = 0) {
  CommandContext *context =
      frame.make(ContextKey::make(rank, order), "test", view, pass);
  if (!context)
    return false;
  if (!context->add<ApplicationCommand>(&value, sizeof(value))) {
    context->cancel();
    return false;
  }
  context->seal();
  return true;
}

bool playbackOrder() {
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(!frame.create(0, 1) && !frame.create(1, 257) && frame.create(2, 3),
             "tag dimension bounds"))
    return false;
  if (!check(put(frame, 2, 3, 20) && put(frame, 1, 8, 12) &&
                 put(frame, 2, 3, 21, 255, 255) && put(frame, 1, 1, 10) &&
                 put(frame, ContextKey::C_DEAD_RANK, 1, 99),
             "record application payloads"))
    return false;
  if (!check(frame.pendingCommands() == 5,
             "pending command count before playback"))
    return false;
  Capture<> capture;
  play(frame, capture);
  frame.endFrame();
  const auto &stats = frame.lastFrame();
  const auto *tags = frame.lastTags();
  return check(!capture.invalid && capture.count == 4 &&
                   capture.values == std::array<uint32_t, 4>{10, 12, 20, 21},
               "playback ignored key order, equal-key insertion order or dead "
               "ranks") &&
         check(stats.contexts == 5 && stats.ties == 1 && stats.dead == 1 &&
                   stats.commands[uint32_t(Command::Application)] == 4 &&
                   !frame.pending(),
               "playback statistics") &&
         check(tags != nullptr && tags->at(0, 0).contexts == 3 &&
                   tags->at(0, 0).commands == 3 &&
                   tags->at(1, 2).contexts == 1 && tags->at(1, 2).commands == 1,
               "out-of-range tag clamp");
}

bool parallelRecording() {
  constexpr uint32_t C_WORKERS = 4;
  constexpr uint32_t C_PER_WORKER = 32;
  constexpr uint32_t C_COMMANDS = C_WORKERS * C_PER_WORKER;
  constexpr uint32_t C_PAYLOAD_BASE = 0x5a5a0000;
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(frame.create(), "allocate context pool for parallel producers"))
    return false;
  std::array<std::thread, C_WORKERS> workers;
  std::array<bool, C_WORKERS> recorded{};
  std::atomic<uint32_t> ready{0};
  std::atomic<bool> start{false};
  for (uint32_t worker = 0; worker < C_WORKERS; ++worker)
    workers[worker] = std::thread([&, worker] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      bool ok = true;
      // Use interleaved, predetermined keys that are independent of scheduling.
      for (uint32_t item = C_PER_WORKER; item-- > 0;) {
        const uint32_t index = item * C_WORKERS + worker;
        ok &= put(frame, 1, index + 1, C_PAYLOAD_BASE + index);
      }
      recorded[worker] = ok;
    });
  while (ready.load(std::memory_order_acquire) != C_WORKERS)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);

  // Publish every context in this intended batch before sorting it.
  for (auto &worker : workers)
    worker.join();
  for (const bool ok : recorded)
    if (!check(ok, "parallel producer could not record a command"))
      return false;
  if (!check(frame.pendingCommands() == C_COMMANDS,
             "parallel producers lost a command before playback"))
    return false;

  Capture<C_COMMANDS> capture;
  play(frame, capture);
  frame.endFrame();
  const auto &stats = frame.lastFrame();
  if (!check(!capture.invalid && capture.count == C_COMMANDS &&
                 stats.contexts == C_COMMANDS &&
                 stats.commands[uint32_t(Command::Application)] == C_COMMANDS &&
                 stats.ties == 0 && !frame.pending(),
             "parallel playback did not visit each assigned key exactly once"))
    return false;
  for (uint32_t index = 0; index < C_COMMANDS; ++index)
    if (!check(capture.values[index] == C_PAYLOAD_BASE + index,
               "parallel playback changed a key or payload"))
      return false;
  return true;
}

bool publication() {
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(frame.create(), "allocate context pool for publication"))
    return false;

  std::atomic<bool> recording{false}, finish{false}, stopped{false};
  constexpr uint32_t C_EARLY_VALUE = 37;
  constexpr uint32_t C_LATE_VALUE = 93;
  std::thread writer([&] {
    auto *context = frame.make(ContextKey::make(1, 1), "paused producer");
    auto *command = context ? context->add<ApplicationCommand>(sizeof(uint32_t))
                            : nullptr;
    recording.store(true, std::memory_order_release);
    while (!finish.load(std::memory_order_acquire))
      std::this_thread::yield();
    if (command) {
      memcpy(CommandContext::data(command), &C_LATE_VALUE, sizeof(C_LATE_VALUE));
      context->seal();
    }
    stopped.store(true, std::memory_order_release);
  });
  while (!recording.load(std::memory_order_acquire))
    std::this_thread::yield();

  const bool hidden =
      !frame.pending() && frame.pendingCommands() == 0 && frame.sort() == 0;
  const bool submitted = put(frame, 2, 1, C_EARLY_VALUE);
  const bool earlyPending = frame.pending() && frame.pendingCommands() == 1;
  Capture<2> capture;
  play(frame, capture);
  const bool earlyPlayed = capture.count == 1 && !frame.pending();

  finish.store(true, std::memory_order_release);
  while (!frame.pending() && !stopped.load(std::memory_order_acquire))
    std::this_thread::yield();
  play(frame, capture);
  writer.join();

  return check(hidden, "playback observed an open context") &&
         check(submitted && earlyPending && earlyPlayed,
               "an open context blocked an already published batch") &&
         check(!capture.invalid && capture.count == 2 &&
                   capture.values ==
                       std::array<uint32_t, 2>{C_EARLY_VALUE, C_LATE_VALUE} &&
                   !frame.pending(),
               "later publication lost its payload or replayed twice");
}

bool abandonedResources() {
  nvrhi::RefCounter<nvrhi::IResource> resource;
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(frame.create(), "allocate context pool for discard tests"))
    return false;
  const auto withResource = [&]() -> CommandContext * {
    auto *context = frame.make(ContextKey::make(1, 1), "held resource");
    auto *command = context ? context->add<ApplicationCommand>() : nullptr;
    if (!command)
      return nullptr;
    resource.AddRef();
    command->resource = &resource;
    return context;
  };

  auto *open = withResource();
  auto *sealed = withResource();
  if (!check(open && sealed && resource.GetRefCount() == 3,
             "record two retained resources"))
    return false;
  sealed->seal();
  sealed->seal();
  sealed->cancel();
  const bool closed = !sealed->reserve(32) &&
                      sealed->add<ApplicationCommand>() == nullptr;
  discard(frame);
  if (!check(closed && !frame.pending() && resource.GetRefCount() == 2,
             "discard must release the published batch only"))
    return false;

  open->cancel();
  open->cancel();
  open->seal();
  if (!check(!frame.pending() && resource.GetRefCount() == 1,
             "cancellation must release without publishing"))
    return false;

  auto *detached = withResource();
  if (!check(detached != nullptr, "record resource for an aborted batch"))
    return false;
  detached->seal();
  const uint32_t sorted = frame.sort();
  frame.discardSorted(0, sorted);
  if (!check(sorted == 1 && resource.GetRefCount() == 1,
             "discarding a detached batch must release its resources"))
    return false;

  auto *leftOpen = withResource();
  auto *leftSealed = withResource();
  if (!check(leftOpen && leftSealed, "record abandoned contexts"))
    return false;
  leftSealed->seal();
  frame.endFrame();
  return check(!frame.pending() && resource.GetRefCount() == 1,
               "frame reset leaked an open or published recording");
}

class TestTexture final : public nvrhi::RefCounter<nvrhi::ITexture> {
public:
  const nvrhi::TextureDesc &getDesc() const override { return desc; }
  nvrhi::Object getNativeView(nvrhi::ObjectType, nvrhi::Format,
                             nvrhi::TextureSubresourceSet, nvrhi::TextureDimension,
                             bool, std::optional<nvrhi::ComponentMapping>) override {
    return nullptr;
  }

private:
  nvrhi::TextureDesc desc;
};

class TestBuffer final : public nvrhi::RefCounter<nvrhi::IBuffer> {
public:
  const nvrhi::BufferDesc &getDesc() const override { return desc; }
  nvrhi::GpuVirtualAddress getGpuVirtualAddress() const override { return 0; }

private:
  nvrhi::BufferDesc desc;
};

bool resourceDeclaration() {
  TestTexture texture;
  TestBuffer buffer;
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(frame.create(), "allocate context pool for resource declarations"))
    return false;
  const nvrhi::TextureSubresourceSet range{2, 3, 4, 5};
  const auto record = [&]() -> CommandContext * {
    auto *context = frame.make(ContextKey::make(1, 1), "resource declaration");
    const TextureUse textures[] = {{&texture, range, nvrhi::ResourceStates::CopyDest}};
    const BufferUse buffers[] = {{&buffer, nvrhi::ResourceStates::IndirectArgument}};
    return context && recordResourceUses(*context, {textures, 1, buffers, 1})
               ? context
               : nullptr;
  };
  const auto hasReferences = [&](unsigned long count) {
    return texture.GetRefCount() == count && buffer.GetRefCount() == count;
  };

  auto *context = record();
  if (!check(context && hasReferences(2), "recording did not retain both resources"))
    return false;
  context->seal();
  Capture<> capture;
  play(frame, capture);
  if (!check(!capture.invalid && capture.declarations == 1 &&
                 capture.textureUse.texture == &texture &&
                 capture.textureUse.subresources == range &&
                 capture.textureUse.state == nvrhi::ResourceStates::CopyDest &&
                 capture.bufferUse.buffer == &buffer &&
                 capture.bufferUse.state == nvrhi::ResourceStates::IndirectArgument &&
                 hasReferences(1),
             "resource declaration changed on playback or leaked a reference"))
    return false;

  context = record();
  if (!check(context && hasReferences(2), "record resources for discard"))
    return false;
  context->seal();
  discard(frame);
  return check(hasReferences(1), "discard leaked a resource declaration reference");
}

bool arenaLimits() {
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(frame.create(), "allocate arena and contexts"))
    return false;
  CommandContext *context = frame.make(ContextKey::make(1, 1), "arena");
  if (!check(context != nullptr &&
                 context->add<ApplicationCommand>() != nullptr,
             "make initial recording run"))
    return false;
  if (!check(!context->reserve(UINT64_MAX) && !frame.couldTake(UINT64_MAX) &&
                 frame.directUnits() == 1,
             "oversized reserve wrapped in an existing run"))
    return false;
  if (!check(commandBytes<ApplicationCommand>(1u << 24) == 0 &&
                 commandBytes<ApplicationCommand>(UINT32_MAX) == 0 &&
                 context->add<ApplicationCommand>(UINT32_MAX) == nullptr &&
                 frame.droppedCommands() == 1 && context->commands() == 1,
             "encoded command-size field overflow"))
    return false;
  {
    Capture<> capture;
    eastl::vector<uint8_t> scratch;
    Recorder direct(capture, scratch);
    const uint32_t value = 7;
    if (!check(direct.add<ApplicationCommand>(&value, sizeof value) !=
                       nullptr &&
                   direct.add<ApplicationCommand>(UINT32_MAX) == nullptr,
               "direct recorder size overflow"))
      return false;
    direct.finish();
    if (!check(!capture.invalid && capture.count == 1 &&
                   capture.values[0] == value,
               "invalid direct command damaged the pending command"))
      return false;
  }

  constexpr uint32_t C_PAYLOAD = 1u << 20;
  constexpr uint32_t C_TRIES = FrameContexts::C_ARENA / C_PAYLOAD + 1;
  const uint32_t before = frame.droppedCommands();
  uint32_t accepted = 0;
  while (accepted < C_TRIES &&
         context->add<ApplicationCommand>(C_PAYLOAD) != nullptr)
    ++accepted;
  if (!check(accepted > 0 && accepted < C_TRIES &&
                 frame.droppedCommands() - before == 1 &&
                 context->commands() == accepted + 1,
             "exhausted arena corrupted an earlier recording"))
    return false;
  discard(frame);
  frame.endFrame();
  if (!check(!frame.pending() && frame.lastFrame().arena > 0 &&
                 frame.lastFrame().arena <= FrameContexts::C_ARENA,
             "arena accounting after discard"))
    return false;
  return check(put(frame, 1, 1, 0),
               "arena did not become usable for the next frame");
}

bool contextLimit() {
  FrameOwner owner;
  FrameContexts &frame = owner.frame;
  if (!check(frame.create(), "allocate context pool"))
    return false;
  const auto key = ContextKey::make(1, 1);
  for (uint32_t i = 0; i < FrameContexts::C_CONTEXTS; ++i)
    if (!check(frame.make(key, "pool") != nullptr,
               "the pool rejected a legal context index"))
      return false;
  if (!check(frame.make(key, "overflow") == nullptr &&
                 frame.make(key, "overflow") == nullptr &&
                 frame.droppedContexts() == 2,
             "the pool accepted or miscounted an overflowing allocation"))
    return false;
  discard(frame);
  frame.endFrame();
  return check(frame.lastFrame().contexts == FrameContexts::C_CONTEXTS &&
                   frame.lastFrame().arena == 0 &&
                   frame.make(key, "next frame") != nullptr,
               "the pool did not recover after a frame reset");
}
} // namespace

int main() {
  const bool orderOk = playbackOrder();
  const bool parallelOk = parallelRecording();
  const bool arenaOk = arenaLimits();
  const bool publicationOk = publication();
  const bool resourcesOk = abandonedResources();
  const bool declarationOk = resourceDeclaration();
  return contextLimit() && orderOk && parallelOk && arenaOk && publicationOk &&
                 resourcesOk && declarationOk
             ? 0
             : 1;
}
