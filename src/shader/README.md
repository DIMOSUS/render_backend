# Shader library

`render_backend::shader` turns effect files into D3D12 shader bytecode and describes what the renderer needs
to build pipelines for them. It contains:

- a parser for effect files: HLSL plus `technique` and `pass` blocks in the style of D3DX effects,
- DXC compilation with an on-disk cache that rebuilds only what changed,
- effect descriptions (`EffectDesc`) with parameters, selections, pipeline states and target sets,
- generated C++ headers that describe the material and instance records shaders read,
- automatic constant bindings (`AutoConstants`),
- a file watcher that rebuilds shaders while the application runs (`Watcher`).

## Effects

An effect is a file `<root>/effects/<name>.hlsl`. Beside ordinary HLSL, it declares techniques, and each
technique declares passes:

```hlsl
technique Default
{
    pass Main
    {
        VertexShader = VS_Main();
        PixelShader = PS_Main();
        CullMode = NONE;
        DepthTest = true;
    }
}
```

[`examples/spinning_mesh/shaders/effects/spinning_mesh.hlsl`](../../examples/spinning_mesh/shaders/effects/spinning_mesh.hlsl)
is a complete effect. All stages are compiled for shader model 6.6.

Two naming conventions apply to every effect:

- A draw pass without a `Targets = ...;` state renders into the target set named `TG_Scene`, if the effect
  declares one (usually in a shared include). Otherwise its pipeline has no fixed attachment formats and takes
  them from the framebuffer.
- A `SamplerState` declared in a file under `<root>/common/` is a shared template. It is added only to the
  effects whose code samples with it.

## Opening a cache

```cpp
render_backend::shader::ShaderCache cache;
eastl::string error;
if (!cache.open(shaderRoot, render_backend::shader::ShaderCache::Mode::Develop, error))
    return false;
```

`shaderRoot` is the directory that contains `effects/`. There are two modes:

- **Develop** compiles from source. It loads `dxcompiler.dll` at run time, from the path passed to `open`, or
  else from the executable's directory or the Windows SDK. Stale descriptions and variants are rebuilt when
  they are requested.
- **Prebuilt** reads only compiled files and never loads DXC. The folder must contain
  `effects/deployed_effects.txt`, and only the effects it lists are served. `ShaderCache::deploy` writes such a
  folder from a Develop tree. Always deploy descriptions and bytecode together.

`getShader` returns the bytecode of one stage of a pass for a set of selection values, together with its entry
point. `pipelines` lists every pipeline an effect can need, each with a stable hash, so they can be created at
startup and stored in a disk pipeline cache keyed by `shaderSetHash`.

The cache writes `<name>.desc` and, per variant, a `.dxil` and a `.dep` manifest next to the sources. A
manifest records the hashes of the source and of every file it includes, and the compiler version, entry,
selection values, profile, options and source policy. A variant is rebuilt only when one of these changes.

## Binding models

By default (`EntryModel::Authored`) the cache compiles the effect's own entry points and the caller binds
resources itself.

A renderer that fills bindless material and instance records instead passes a `ShaderPolicy` with
`EntryModel::RendererRecords`, a prelude (`sourcePrefix`), the path of the shared declarations, and a
`ShaderRecordAbi` that describes the records. Register the host's vocabulary through `vocabulary.hpp` before
the first effect is parsed. The source policy is part of every manifest, so changing it rebuilds the affected
variants. Variant file names of the authored model include the policy hash.

`ShaderCache::layoutHeaders` generates the C++ record definitions for the host. Call
`ShaderCache::expectLayouts` with the generated `C_EFFECT_LAYOUTS` table, and the cache refuses any description
whose layout differs from the one the host was compiled with. The source root, the vocabulary, effect remaps and
the generated headers belong to the host.

## Hot reload

`Watcher::start` watches the root of a Develop cache on a worker thread. After a source or include changes, it
rebuilds the affected descriptions and the variants that were already compiled. The renderer calls
`Watcher::poll` once per frame, at a point where it can replace shaders and drop the pipelines built from
them. When a compile fails, the error is reported and the previous shader stays in use.

## Allocator

The public API uses EASTL containers. The library links the EASTL allocator functions from
[`src/eastl_allocator.cpp`](../eastl_allocator.cpp), so an application must not define them again.
