# Dependencies

The first configure downloads these sources into the build directory, at the revisions pinned in
[`DependencyVersions.cmake`](DependencyVersions.cmake):

| Dependency | CMake target |
| --- | --- |
| NVRHI, D3D12 backend only, with the patches in [`patches/nvrhi`](../patches/nvrhi) | `render_backend::nvrhi` (also `render_backend::nvrhi_base` and `render_backend::nvrhi_d3d12`) |
| DirectX-Headers | used by NVRHI; reused if the parent project already defines `DirectX-Headers` or `Microsoft::DirectX-Headers` |
| EASTL and EABase | `render_backend::eastl` |
| stb_image | `render_backend::stb` |

`render_backend::nvrhi` links NVRHI, its D3D12 backend, `DirectX-Guids` and the required system libraries.
NVRHI is built with RTTI, which its validation layer needs. With `RENDER_BACKEND_CHECKS=ON`, NVRHI assertions
stay enabled in Release.

The cache variables `RENDER_BACKEND_NVRHI_INCLUDE`, `RENDER_BACKEND_DIRECTX_INCLUDE`,
`RENDER_BACKEND_EASTL_INCLUDE` and `RENDER_BACKEND_EABASE_INCLUDE` hold the include directories, for build
systems that consume the libraries without CMake.

## Offline builds

To build without network access, point CMake at local checkouts of the pinned revisions:

```
FETCHCONTENT_SOURCE_DIR_RENDER_BACKEND_NVRHI
FETCHCONTENT_SOURCE_DIR_RENDER_BACKEND_DIRECTX_HEADERS
FETCHCONTENT_SOURCE_DIR_RENDER_BACKEND_EASTL
FETCHCONTENT_SOURCE_DIR_RENDER_BACKEND_EABASE
FETCHCONTENT_SOURCE_DIR_RENDER_BACKEND_STB
```

If the NVRHI directory is a Git checkout, configure fails unless it is at the pinned commit.

## MSVC runtime and STL configuration

A standalone configure uses the static CRT (`/MT`) in every configuration. To use a different runtime, set
`CMAKE_MSVC_RUNTIME_LIBRARY`, for example to `MultiThreadedDLL` for `/MD`.

When the backend is added with `add_subdirectory`, it uses the parent project's runtime setting, or CMake's
default if the parent sets none. Set `CMAKE_MSVC_RUNTIME_LIBRARY` before any targets are created, so that the
application, the backend and its dependencies use the same CRT. The parent project needs policy `CMP0091` set to
`NEW` before the compiler is enabled.

`RENDER_BACKEND_DISABLE_MSVC_STL_EXCEPTIONS=ON` builds with `_HAS_EXCEPTIONS=0`. The definition propagates to
consumers through the linked targets.

Build systems that link the static libraries directly must use the same CRT, `_ITERATOR_DEBUG_LEVEL` and
`_HAS_EXCEPTIONS` as the backend. Because the public API exposes EASTL types, they must also define
`EASTL_EXCEPTIONS_ENABLED=0`, `EASTL_ASSERT_ENABLED=0` and `EASTL_CPP11_MUTEX_ENABLED=0`.

## NVRHI patches

| Patch | Fix |
| --- | --- |
| `01-check-resource-query-hresult` | Test the `HRESULT` of `QueryInterface` on native resources instead of the returned pointer. |
| `02-read-only-root-signature-cache-lookup` | Look up an existing root signature without inserting into the cache. |
| `03-allow-bindless-acceleration-structure-srv` | Accept acceleration structure SRVs in mutable bindless descriptor tables in the validation layer. |
| `04-check-framebuffer-mip-extents` | Validate framebuffer attachment sizes at the selected mip level. |

CMake copies the pinned NVRHI sources into a build-tree directory whose name depends on the pin and the patch
contents, and applies the patches there. The fetched checkout is never modified. A patch passes if it applies
cleanly, or if a strict reverse check shows that its changes are already present. Otherwise configure stops.

## Updating NVRHI

1. Change `RENDER_BACKEND_PIN_NVRHI` in `DependencyVersions.cmake`. If the new NVRHI revision needs a different
   DirectX-Headers version, update `RENDER_BACKEND_PIN_DIRECTX_HEADERS` in the same change.
2. Configure, build and test in a new build directory with checks enabled:

   ```powershell
   cmake -S . -B build/update-check -A x64 -DRENDER_BACKEND_CHECKS=ON
   cmake --build build/update-check --config Release --parallel
   ctest --test-dir build/update-check -C Release --output-on-failure
   ```

3. If a patch fails, read the upstream change. Refresh the patch, or delete it if upstream now includes the fix,
   and configure again.
4. On a machine with a GPU, also build with `-DRENDER_BACKEND_BUILD_GPU_TESTS=ON` and run the example.

## Agility runtime

An executable can ship the D3D12 Agility SDK runtime instead of relying on the version installed with Windows.
Call `render_backend_copy_agility_runtime(<target>)` and configure with either:

- `-DRENDER_BACKEND_AGILITY_SDK_ROOT=<path>`, the root of an unpacked `Microsoft.Direct3D.D3D12` NuGet
  package, or
- `-DRENDER_BACKEND_FETCH_AGILITY=ON`, which downloads the pinned package and checks its SHA-256 hash.

The helper copies `D3D12Core.dll` to a `D3D12` folder next to the executable. With `RENDER_BACKEND_CHECKS=ON`
it also copies `d3d12SDKLayers.dll` if the package includes it. It defines
`RENDER_BACKEND_AGILITY_SDK_VERSION` for the target. The executable must export `D3D12SDKVersion` with that
value, and `D3D12SDKPath`, or Windows will not load the copied runtime.
[`examples/spinning_mesh/main.cpp`](../examples/spinning_mesh/main.cpp) shows how.

Without either option, the helper does nothing and the executable uses the installed runtime. Libraries never
depend on the Agility binaries.
