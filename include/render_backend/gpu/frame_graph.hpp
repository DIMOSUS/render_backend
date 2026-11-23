// Frame graph. A frame is a list of passes, each declaring the resources it reads and writes. From these declarations
// the graph derives the execution order, the lifetime of each resource, and which passes can be culled. It does not
// know what a pass does or what a resource is: a pass is a caller-chosen name (a number) and a resource is a bit in a
// 64-bit mask.
//
// Ordering rules. A pass sees a resource as the passes declared before it left it:
// - Writers of a resource execute in declaration order.
// - A reader executes after the writers declared before it and before the writers declared after it.
// - A reader with no earlier writer reads the frame's final result, so it waits for every writer of the resource.
// Passes these rules leave unordered keep declaration order. Declaring a name again extends the earlier pass (masks are
// merged) when every pass declared in between is independent of it; otherwise it creates a new pass with that name.
//
// Uses fixed arrays and never allocates. Frame and Order are trivially copyable.
#pragma once
#include <stdint.h>

namespace render_backend::gpu::graph
{
constexpr uint32_t C_PASSES = 96;	 // Maximum passes, and maximum declarations, per frame.
constexpr uint32_t C_RESOURCES = 64; // Bits in a Mask.
constexpr uint32_t C_NONE = ~0u;
constexpr uint16_t C_DEAD = 0xffff; // Rank of a culled pass.
using Mask = uint64_t;
constexpr Mask bit(uint32_t resource)
{
	return Mask(1) << resource;
}

struct Pass
{
	uint32_t name = 0;
	Mask	 reads = 0, writes = 0;
};

// Pass declarations for one frame.
struct Frame
{
	uint32_t passCount = 0, declarationCount = 0;
	uint32_t overflow = 0;	 // Declarations dropped because C_PASSES was reached.
	Mask	 persistent = 0; // Resources that outlive the frame, such as the back buffer or history buffers.
	Pass	 passes[C_PASSES];
	uint8_t	 declared[C_PASSES]; // Pass index of each declaration, in declaration order.

	// Declares a pass and returns its index in `passes`: either an extended earlier pass with the same name or a new
	// one. Returns C_NONE when the declaration limit is reached.
	uint32_t pass(uint32_t name, Mask reads, Mask writes);
	// Returns the pass of the first declaration of `name` at or after declaration index `cursor`, and moves `cursor`
	// to it. Returns C_NONE if there is none.
	uint32_t find(uint32_t name, uint32_t &cursor) const;
};

// Result of compile().
struct Order
{
	uint32_t count = 0; // Passes that execute.
	uint32_t dead = 0;	// Culled passes: their output reaches no persistent resource.
	// Passes placed in declaration order because the rules formed a cycle. The resulting order may run a pass before a
	// writer it depends on; callers should fall back to declaration order for that frame.
	uint32_t cycles = 0;
	uint16_t rank[C_PASSES];	 // Position of each pass in execution order, 1..count, or C_DEAD.
	uint8_t	 sequence[C_PASSES]; // The first `count` entries are pass indices in execution order.
	// Ranks of the first and last executed pass that use a resource. {0, 0} if none does.
	struct Life
	{
		uint16_t first = 0, last = 0;
	};
	Life life[C_RESOURCES];
};
void compile(const Frame &frame, Order &order);
} // namespace render_backend::gpu::graph
