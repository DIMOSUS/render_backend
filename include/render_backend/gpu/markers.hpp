// GPU debug markers: a stack of named ranges over the frame's command list, as shown by RenderDoc, PIX and GPU
// profilers. The hierarchy is
//
//   frame N > view > pass > stage > item
//
// where a stage is part of a pass (for example opaque or transparent geometry, or a post-processing technique) and an
// item is a single draw or upload.
//
// Ranges are opened during playback, in execution order: view and pass ranges by the Player through Player::slot
// (context.hpp), stage and item ranges by MarkerPush/MarkerPop commands and by callbacks. Recording never touches the
// stack, so markers never force a playback.
//
// Each range is always emitted as a PIX event through NVRHI, which RenderDoc and PIX display, and also through an
// optional vendor Backend for tools that do not read PIX events. The stack outlives a command list: if the list is
// closed mid-frame (for example for a readback), closing() ends every open range and opened() reopens them in the next
// list, so ranges always nest within a list.
//
// Markers are off by default and the host enables them with setup(). Callers check on(), which does not change after
// setup, before formatting names. Views, passes and stages are timed when diag::Timers is enabled; items are not.
// Names are copied into fixed-size slots.
#pragma once
#include <nvrhi/nvrhi.h>
#include <stdint.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace render_backend::gpu::diag
{
class Timers;
}

namespace render_backend::gpu::markers
{
// Optional vendor marker interface, emitted alongside PIX events on the native list NVRHI records into.
class Backend
{
  public:
	virtual ~Backend() = default;
	virtual void		begin(ID3D12GraphicsCommandList *list, const char *name) = 0;
	virtual void		end(ID3D12GraphicsCommandList *list) = 0;
	virtual const char *name() const = 0;
};
enum class Kind : uint8_t
{
	View,  // A view of the scene. Timed.
	Pass,  // A render pass. Timed.
	Stage, // Part of a pass. Timed.
	Item,  // A single draw or upload. Not timed.
};

class Stack
{
  public:
	static constexpr uint32_t C_DEPTH = 16; // Maximum open ranges. Deeper pushes are counted but not emitted.
	static constexpr uint32_t C_NAME = 96;	// Name buffer size; longer names are truncated.
	static constexpr uint32_t C_PATH = 1024;

	// Enables markers. `timers` and `backend` may be null. The stack takes ownership of `backend`.
	void setup(nvrhi::IDevice *device, diag::Timers *timers, Backend *backend);
	void release();
	bool on() const
	{
		return m_on;
	}

	// opened(): a command list was opened; begins the frame range and reopens every range on the stack.
	// closing(): the list is about to close; ends every open range in it but keeps them on the stack.
	// frameDone(): call after present; the next frame gets the next number.
	void opened(nvrhi::ICommandList *list);
	void closing();
	void frameDone();

	// Pushes a range. It begins immediately if a list is open, otherwise when the next list opens. Returns the stack
	// index to pass to popTo().
	uint32_t push(Kind kind, const char *name);
	uint32_t pushf(Kind kind, const char *format, ...);
	// Ends every range at index `level` and above.
	void	 popTo(uint32_t level);
	uint32_t depth() const
	{
		return m_depth;
	}
	// Name of the range at index i, for example to tag error reports.
	const char *name(uint32_t i) const
	{
		return m_entries[i].name;
	}

  private:
	struct Entry
	{
		char	 name[C_NAME];
		Kind	 kind;
		bool	 running = false; // Begun in the current list.
		bool	 begun = false;	  // Begun in some list this frame; a timer resumed in a new list adds no call.
		uint32_t pathLength = 0;  // Length of m_path up to and including this range (timed kinds only).
	};
	void begin(Entry &e, uint32_t index);
	void end(Entry &e);

	bool					   m_on = false;
	nvrhi::IDevice			  *m_device = nullptr;
	nvrhi::ICommandList		  *m_list = nullptr; // Open list; null between lists.
	ID3D12GraphicsCommandList *m_native = nullptr;
	diag::Timers			  *m_timers = nullptr;
	Backend					  *m_backend = nullptr;
	Entry					   m_entries[C_DEPTH];
	uint32_t				   m_depth = 0, m_overflow = 0;
	uint32_t				   m_frame = 1; // Frame being recorded, counted from 1 like RenderDoc.
	bool					   m_frameBegun = false;
	char					   m_frameName[32] = {};
	char					   m_path[C_PATH] = {};
};

// RAII marker range. Does nothing when `stack` is null; pass null when markers are off.
class Scope
{
  public:
	Scope(Stack *stack, Kind kind, const char *name) : m_stack(stack)
	{
		if (m_stack != nullptr)
			m_level = m_stack->push(kind, name);
	}
	~Scope()
	{
		if (m_stack != nullptr)
			m_stack->popTo(m_level);
	}
	Scope(const Scope &) = delete;
	Scope &operator=(const Scope &) = delete;

  private:
	Stack	*m_stack;
	uint32_t m_level = 0;
};
} // namespace render_backend::gpu::markers
