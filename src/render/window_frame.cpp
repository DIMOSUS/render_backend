// Window helpers for borderless full-screen presentation.
#include "platform.hpp"
#include <render_backend/gpu/window_frame.hpp>

void render_backend::gpu::window::makeBorderless(HWND window)
{
	const LONG_PTR visible = GetWindowLongPtrA(window, GWL_STYLE) & WS_VISIBLE;
	SetWindowLongPtrA(window, GWL_STYLE, WS_POPUP | visible);
	SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
	coverMonitor(window);
}

bool render_backend::gpu::window::coverMonitor(HWND window)
{
	if (IsIconic(window))
		return false; // Minimised; placed again on the next call after it is restored.
	MONITORINFO info{};
	info.cbSize = sizeof info;
	if (!GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info))
		return false;
	RECT placed{};
	GetWindowRect(window, &placed);
	if (EqualRect(&placed, &info.rcMonitor))
		return false;
	SetWindowPos(window, nullptr, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
				 info.rcMonitor.bottom - info.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
	return true;
}

bool render_backend::gpu::window::clientSize(HWND window, uint32_t &width, uint32_t &height)
{
	RECT client{};
	if (window == nullptr || IsIconic(window) || !GetClientRect(window, &client) || client.right <= 0 ||
		client.bottom <= 0)
		return false;
	width = uint32_t(client.right);
	height = uint32_t(client.bottom);
	return true;
}
