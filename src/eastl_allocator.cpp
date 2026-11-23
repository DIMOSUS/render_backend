// The allocation operators EASTL's default allocator requires the application to define. Both forward to the global
// operator new[].
#include <cstddef>
#include <cstdlib>
#include <new>

void *operator new[](std::size_t size, const char *, int, unsigned, const char *, int)
{
	return ::operator new[](size);
}

void *operator new[](std::size_t size, std::size_t alignment, std::size_t offset, const char *, int, unsigned,
					 const char *, int)
{
	// EASTL frees these blocks with plain delete[], so over-aligned or offset requests cannot be honoured.
	if (alignment == 0 || alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__ || offset % alignment != 0)
		std::abort();
	return ::operator new[](size);
}
