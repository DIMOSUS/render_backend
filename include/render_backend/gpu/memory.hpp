// Process-heap storage for renderer internals and native mip extent calculations.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace render_backend::gpu
{
void	*heapAlloc(size_t size);
void	 heapFree(void *p);
uint32_t levelExtent(uint32_t size, uint32_t level);
uint32_t textureLevelCount(uint32_t width, uint32_t height, uint32_t levels);
} // namespace render_backend::gpu
