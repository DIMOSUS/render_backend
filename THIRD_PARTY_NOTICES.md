# Third-party notices

The build downloads the following projects at the revisions pinned in
[`cmake/DependencyVersions.cmake`](cmake/DependencyVersions.cmake). None of their sources are stored in this
repository, except the NVRHI patches in [`patches/nvrhi`](patches/nvrhi).

| Dependency | Upstream | License | License files in the upstream checkout |
| --- | --- | --- | --- |
| NVRHI | [NVIDIA-RTX/NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | MIT | `LICENSE.txt`. A copy is included in `patches/nvrhi/`. |
| DirectX-Headers | [microsoft/DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT | `LICENSE` |
| EASTL | [electronicarts/EASTL](https://github.com/electronicarts/EASTL) | BSD-3-Clause | `LICENSE`, `3RDPARTYLICENSES.TXT` |
| EABase | [electronicarts/EABase](https://github.com/electronicarts/EABase) | BSD-3-Clause | `LICENSE` |
| stb_image | [nothings/stb](https://github.com/nothings/stb) | MIT or public domain | `LICENSE`, and the notice in `stb_image.h` |

The optional Agility SDK runtime comes from Microsoft's `Microsoft.Direct3D.D3D12` NuGet package. If you
redistribute it, include the package's `LICENSE.txt` and follow its `distributable files.txt`.

DXC is not downloaded. It comes from the Windows SDK installed on the developer's machine.
