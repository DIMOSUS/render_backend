# Render Backend

A Direct3D 12 rendering backend for Windows x64, written in C++17 on top of [NVRHI](https://github.com/NVIDIA-RTX/NVRHI).
It consists of two static libraries:

- **`render_backend::render`**: device and swap chain setup, texture loading and upload, command contexts
  recorded on several threads, a frame graph, and ordered frame submission.
- **`render_backend::shader`**: an effect language with techniques and passes, DXC compilation with an
  on-disk cache, generated record layouts and automatic constant bindings, and shader hot reload.

The backend has no scene or material model. A frontend declares its passes, resource dependencies and
bindings, and the backend records and submits the work.

## Requirements

- Windows 10 or later and a GPU with a D3D12 driver. Effects target shader model 6.6.
- Visual Studio 2026 with the "Desktop development with C++" workload.
- The Windows SDK. DXC from the SDK compiles effects at run time.
- CMake 3.24 or newer. The `Visual Studio 18 2026` generator below needs CMake 4.2 or newer; the version that
  ships with Visual Studio 2026 is enough.
- Git. The first configure downloads the pinned dependencies listed in
  [`cmake/DependencyVersions.cmake`](cmake/DependencyVersions.cmake).

## Build

```powershell
cmake -S . -B build/vs2026 -G "Visual Studio 18 2026" -A x64
cmake --build build/vs2026 --config Release
ctest --test-dir build/vs2026 -C Release --output-on-failure
```

`build/vs2026/RenderBackend.slnx` opens the same targets in Visual Studio.

## Example

`build/vs2026/bin/Release/spinning_mesh.exe` opens a window and draws a rotating mesh with a single pass. The
mesh is generated in code, so the example needs no assets. Its effect is compiled at startup.
See [`examples/spinning_mesh`](examples/spinning_mesh/README.md).

## Using the backend in a project

Add the repository as a subdirectory and link the libraries you need:

```cmake
add_subdirectory(external/render_backend)
target_link_libraries(my_app PRIVATE render_backend::render render_backend::shader)
```

There are no install rules, so `find_package` is not supported.

The public headers expose EASTL containers. `render_backend::shader` links the backend's EASTL allocator
functions (`src/eastl_allocator.cpp`), so a consumer must not define its own. Build settings that affect the ABI
must match across the application and the backend. See
[MSVC runtime and STL configuration](cmake/README.md#msvc-runtime-and-stl-configuration).

### CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `RENDER_BACKEND_BUILD_EXAMPLE` | ON when top level | Build `spinning_mesh`. |
| `RENDER_BACKEND_BUILD_TESTS` | ON when top level | Build the unit tests. |
| `RENDER_BACKEND_BUILD_GPU_TESTS` | OFF | Also build the tests that need a D3D12 hardware adapter. |
| `RENDER_BACKEND_CHECKS` | OFF | Extra validation, including NVRHI assertions in Release. |
| `RENDER_BACKEND_DISABLE_MSVC_STL_EXCEPTIONS` | OFF | Build with `_HAS_EXCEPTIONS=0` and propagate it to consumers. |
| `RENDER_BACKEND_EXPORT_SDK` | OFF | At configure time, copy the patched NVRHI and DirectX-Headers include trees to `<build>/consumer/include` for build systems that do not use CMake. |
| `RENDER_BACKEND_AGILITY_SDK_ROOT` | unset | Path to an unpacked Agility SDK package. See [Agility runtime](cmake/README.md#agility-runtime). |
| `RENDER_BACKEND_FETCH_AGILITY` | OFF | Download the pinned Agility SDK package instead. |

## Render library

### Textures

The texture loader reads DDS, PNG, JPEG, TGA and BMP from memory. BC6H (UF16 and SF16) and BC7 (UNORM and sRGB)
DDS files are uploaded as compressed blocks with their mip chains.

### Uploads

`upload::Queue` stages texture and buffer data in upload-heap chunks that it owns.

1. Record the copies with `Queue::issue`, or with `Queue::issueTicket` for a copy that must happen at a
   specific point in the command list.
2. Close the command list and submit it with `Queue::submit`. The queue then knows which graphics-queue fence
   value releases each staging range.
3. `Queue::beginFrame` reclaims staging memory whose copies have completed and resets the per-frame budget.

If a recording is abandoned, or its command list is reused without being submitted, call `Queue::discard`.

`upload::ScatterBatch` applies many small, disjoint CPU writes to raw UAV buffers. It packs them into a mapped
upload buffer and records a single compute dispatch, followed by transitions to the requested states. A frame
slot can be reused once the GPU has finished reading its upload buffer.

### Frame execution

The frontend declares the pass order and resource dependencies in a `graph::Frame`.

- Each producer records into its own command context and publishes it with `seal()`. Contexts can be
  recorded on different threads.
- `FrameExecutor` takes the published contexts, sorts them by key and submits their command lists in that
  order. Publish every context that must be ordered with the others before you execute the batch, and finish
  recording on all threads before the frame ends.
- On success the primary command list stays open, so the caller can append commands or submit the rest of the
  frame.
- `FrameExecutor::setPrelude` queues a closed command list that is submitted ahead of the first ordered batch,
  even if playback is split. `FrameExecutor::submit` sends the final primary list.
- While `FrameExecution::allowSubmission` is false, nothing is submitted. Use this while the frontend is still
  collecting data for the frame.

NVRHI infers resource states from bound views. For accesses it cannot see, such as a texture read through a
bindless index, declare them with `ctx::recordResourceUses`. A texture use can be limited to a range of mip
levels and array slices.

### Render targets

`TargetPool::takeTransient` lets targets with matching descriptions share memory when their lifetimes do not
overlap. Rules:

- Call `TargetPool::setExecutionOrder` with the compiled graph order before reserving targets.
- Use the targets in that order on a single graphics queue.
- Each logical target starts with a write or a clear.
- Do not attach per-use framebuffers to a shared slot.

### Packet workers

A `packets::Worker` belongs to the code that creates it. Each instance has its own queue and wait counters.

## Shader library

See [`src/shader/README.md`](src/shader/README.md).

## Tests

`ctest` runs the unit tests: frame graph, recording contexts, DDS parsing, packet worker, shader cache and the
shader library's public interface. With `-DRENDER_BACKEND_BUILD_GPU_TESTS=ON` it also runs four hardware
tests:

- `backend_bc_texture_gpu`: uploads BC6H and BC7 textures, including a cube map and the smallest mips, and
  compares every block read back with the source. It runs with the D3D12 debug layer.
- `backend_upload_queue_gpu`: stalls the graphics queue, keeps advancing CPU frames while uploads are
  recorded or in flight, and checks that staging memory is not reused early.
- `backend_scatter_upload_gpu`: applies scatter writes over two frames, through a prelude and a split
  playback, and compares the buffers read back with the expected contents.
- `backend_target_pool_gpu`: checks when transient targets may share a slot, including stale lifetime data,
  cached framebuffers and an order that is not known yet.

GPU tests report themselves as skipped when no suitable adapter, or no debug layer, is available.

## Dependencies

NVRHI, DirectX-Headers, EASTL, EABase and stb_image are fetched at pinned revisions. NVRHI is built with four
small patches from [`patches/nvrhi`](patches/nvrhi). [`cmake/README.md`](cmake/README.md) covers offline builds,
runtime library settings, the Agility SDK and how to update NVRHI.

## License

The project is licensed under the [MIT License](LICENSE). Licenses of the dependencies are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
