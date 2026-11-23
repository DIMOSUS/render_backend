// Window helpers for borderless full-screen presentation and for tracking the client size the swap chain follows.
#pragma once
#include <stdint.h>

// Same declaration as windows.h with STRICT (the default), so this header does not need to include windows.h.
struct HWND__;
typedef HWND__ *HWND;

namespace render_backend::gpu::window
{
// Switches the window to borderless full screen without exclusive mode: it becomes a popup window (keeping
// WS_VISIBLE) that covers the whole monitor.
void makeBorderless(HWND window);
// Keeps a borderless window covering its monitor. Moves it when the monitor rectangle has changed, for example after a
// resolution change or when a monitor is disconnected. Returns true if the window moved.
bool coverMonitor(HWND window);
// Client area size. Returns false when there is none, for example when the window is minimised or has zero size.
bool clientSize(HWND window, uint32_t &width, uint32_t &height);
} // namespace render_backend::gpu::window
