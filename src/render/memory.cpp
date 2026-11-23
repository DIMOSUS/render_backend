#include <render_backend/gpu/memory.hpp>
#include "platform.hpp"

void *render_backend::gpu::heapAlloc(size_t size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size == 0 ? 1 : size);
}

void render_backend::gpu::heapFree(void *p)
{
	if (p != nullptr)
		HeapFree(GetProcessHeap(), 0, p);
}

//----------------------------------------------------------------------------------------------------------------------
// Mip geometry
uint32_t render_backend::gpu::levelExtent(uint32_t size, uint32_t level)
{
	const uint32_t extent = size >> level;
	return extent == 0 ? 1 : extent;
}

uint32_t render_backend::gpu::textureLevelCount(uint32_t width, uint32_t height, uint32_t levels)
{
	uint32_t full = 1;
	while (width > 1 || height > 1)
	{
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
		++full;
	}
	return levels == 0 || levels > full ? full : levels;
}
