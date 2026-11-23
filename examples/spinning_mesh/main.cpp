// Spinning mesh: a minimal Win32 program that draws with render_backend.
//
// It creates a window, starts a Renderer on it, uploads a mesh built in bunny.hpp, and draws it every frame
// with the effect in shaders/effects/spinning_mesh.hlsl until the window is closed.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <render_backend/gpu/renderer.hpp>

#include "bunny.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#ifdef RENDER_BACKEND_AGILITY_SDK_VERSION
// CMake defines RENDER_BACKEND_AGILITY_SDK_VERSION when it copies an Agility SDK runtime to D3D12\ next to the
// executable. These exports tell the D3D12 loader to use that runtime instead of the one installed with Windows.
extern "C"
{
	__declspec(dllexport) extern const UINT D3D12SDKVersion = RENDER_BACKEND_AGILITY_SDK_VERSION;
	__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
#endif

namespace
{
using render_backend::gpu::demo::Mesh;
using render_backend::gpu::demo::Renderer;

constexpr wchar_t C_WINDOW_TITLE[] = L"Spinning Mesh";
constexpr int	  C_WIDTH = 960;
constexpr int	  C_HEIGHT = 720;
constexpr float	  C_START_ANGLE = 0.52f; // radians
constexpr float	  C_SPIN_SPEED = 0.63f;	 // radians per second

// Layout of the Spin constant buffer in spinning_mesh.hlsl, padded to the 16-byte constant buffer granularity.
struct SpinConstants
{
	float angle;
	float aspect;
	float padding[2] = {};
};

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	// Closing the window only ends the message loop. The renderer still uses the window and must be destroyed
	// first, so wWinMain destroys the window after run() has returned.
	if (message == WM_CLOSE)
	{
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

int fail(HWND window, const std::string &message)
{
	MessageBoxA(window, message.c_str(), "Spinning Mesh", MB_OK | MB_ICONERROR);
	return 1;
}

// Joins a short description with the renderer's error text, when there is one.
std::string describe(const char *what, const std::string &detail)
{
	return detail.empty() ? std::string(what) : std::string(what) + "\n\n" + detail;
}

// Renders until the window is closed. Returns the process exit code. All GPU objects are released on return.
int run(HWND window, int showCommand)
{
	wchar_t		module[MAX_PATH];
	const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return fail(window, "Cannot locate the example executable.");

	// The renderer takes narrow paths. spinning_mesh.manifest makes UTF-8 the process code page, so the
	// conversion below is lossless for any file name.
	const std::string shaders = (std::filesystem::path(module).parent_path() / "shaders").string();

	std::string					error;
	std::unique_ptr<Renderer>	renderer = Renderer::create(window, shaders.c_str(), error);
	if (!renderer)
		return fail(window, describe("Cannot start the renderer.", error));

	const auto vertices = demo::bunny();
	const Mesh mesh = renderer->createMesh(vertices.data(), static_cast<uint32_t>(vertices.size()));
	if (!mesh)
		return fail(window, describe("Cannot upload the mesh.", renderer->lastError()));

	ShowWindow(window, showCommand);

	using Clock = std::chrono::steady_clock;
	const auto	start = Clock::now();
	const float clearColor[] = {0.092f, 0.105f, 0.137f, 1.0f};
	while (true)
	{
		MSG message{};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
				return static_cast<int>(message.wParam);
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		// Nothing is visible while minimized, so sleep until the next message.
		if (IsIconic(window))
		{
			WaitMessage();
			continue;
		}

		RECT client{};
		GetClientRect(window, &client);
		const float seconds = std::chrono::duration<float>(Clock::now() - start).count();
		const float aspect =
			client.bottom > 0 ? static_cast<float>(client.right) / static_cast<float>(client.bottom) : 1.0f;
		const SpinConstants spin{C_START_ANGLE + C_SPIN_SPEED * seconds, aspect};

		// Effect file name (without .hlsl), technique and pass as declared in spinning_mesh.hlsl.
		if (!renderer->frame(mesh, "spinning_mesh", "Default", "Main", &spin, sizeof spin, clearColor))
			return fail(window, describe("Cannot draw the mesh.", renderer->lastError()));
	}
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
	SetProcessDPIAware();

	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc = windowProcedure;
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpszClassName = C_WINDOW_TITLE;
	if (!RegisterClassW(&windowClass))
		return fail(nullptr, "Cannot register the window class.");

	// Size the window so that its client area is C_WIDTH x C_HEIGHT.
	constexpr DWORD style = WS_OVERLAPPEDWINDOW;
	RECT			bounds{0, 0, C_WIDTH, C_HEIGHT};
	AdjustWindowRect(&bounds, style, FALSE);
	HWND window = CreateWindowExW(0, C_WINDOW_TITLE, C_WINDOW_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
								  bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance,
								  nullptr);
	if (!window)
		return fail(nullptr, "Cannot create the window.");

	const int exitCode = run(window, showCommand);
	DestroyWindow(window);
	return exitCode;
}
