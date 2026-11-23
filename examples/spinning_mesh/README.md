# Spinning Mesh

A minimal Win32 program that draws a rotating, flat-shaded bunny with a single render pass. The mesh is built
from ellipsoids in [`bunny.hpp`](bunny.hpp), so the example uses no asset files.

The example is built by default when the repository is the top-level CMake project
(`RENDER_BACKEND_BUILD_EXAMPLE`). The executable is written to `bin/<config>/spinning_mesh.exe` in the build
directory, and the shader directory is copied next to it.

## Files

- [`main.cpp`](main.cpp) creates the window and a `Renderer`, uploads the mesh once and draws it every frame.
  Each frame passes the `Spin` constants: the rotation angle and the aspect ratio.
- [`shaders/effects/spinning_mesh.hlsl`](shaders/effects/spinning_mesh.hlsl) holds the vertex and pixel
  shaders and declares the technique and pass that `main.cpp` draws with. It is compiled at startup with DXC.
- [`spinning_mesh.manifest`](spinning_mesh.manifest) sets UTF-8 as the process code page, so the narrow path
  strings passed to the backend can represent any file name.

## What the backend infers

The example declares no bindings or pipeline formats in C++:

- The effect compiler assigns a register to the `Spin` constant buffer, and the renderer uploads the constants
  to that binding.
- The pixel shader writes `SV_Target`, and the framebuffer provides the attachment formats when the pipeline is
  created.

The demo `Renderer` uses a fixed vertex format of three `float3` attributes, which the shader reads as
`POSITION`, `NORMAL` and `COLOR`.

To run with the Agility SDK runtime instead of the one installed with Windows, configure with
`RENDER_BACKEND_FETCH_AGILITY=ON`. See [Agility runtime](../../cmake/README.md#agility-runtime).
