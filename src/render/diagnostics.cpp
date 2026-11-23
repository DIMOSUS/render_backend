// Diagnostics: logs, debug layer messages, startup timeline and GPU timers.
#include <render_backend/gpu/diagnostics.hpp>
#include "platform.hpp"
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h> // ID3D12InfoQueue1
#include <EASTL/sort.h>
#include <EASTL/string.h>
#include <stdio.h>
#include <string.h>

namespace
{
const char *g_marker = "";
uint32_t	g_errors = 0;
bool		g_inFrames = false;
bool		g_reloading = false; // Shader hot reload in progress; creations are expected.
uint32_t	g_pipelines = 0, g_shaders = 0;
DWORD		g_infoQueueCookie = 0;
char		g_errorLogName[128] = "render_errors.log";
char		g_infoLogName[128] = "render.log";
char		g_prefix[64] = "renderer";
char		g_timingsPath[MAX_PATH] = {};
bool		g_detailed = false;

// A log file in %TEMP%, truncated once per run. It is opened on first use, stays open, and is flushed after every line
// so a crash does not lose output. Output stops at C_LOG_CAP bytes after a final note. A line already seen is not
// written again; its repeat count is logged at 10, 100, 1000, ... Only the first C_SEEN distinct lines are remembered;
// after that every new line is written. Constant-initialized, so it is usable before static constructors run.
struct LogFile
{
	static constexpr uint32_t C_LOG_CAP = 1u << 20;
	static constexpr uint32_t C_SEEN = 512;

	const char *name;
	FILE	   *file = nullptr;
	bool		tried = false, capped = false;
	uint32_t	bytes = 0, seenCount = 0;
	uint64_t	seenHash[C_SEEN] = {};
	uint32_t	seenRepeats[C_SEEN] = {};

	SRWLOCK lock = SRWLOCK_INIT; // Reports can come from several threads, for example pipeline warm-up.

	explicit constexpr LogFile(const char *n) : name(n)
	{
	}
	void write(const char *line)
	{
		AcquireSRWLockExclusive(&lock);
		writeLocked(line);
		ReleaseSRWLockExclusive(&lock);
	}
	void writeLocked(const char *line)
	{
		uint64_t hash = 14695981039346656037ull; // FNV-1a
		for (const char *c = line; *c != 0; ++c)
			hash = (hash ^ uint8_t(*c)) * 1099511628211ull;
		for (uint32_t i = 0; i < seenCount; ++i)
			if (seenHash[i] == hash)
			{
				const uint32_t r = ++seenRepeats[i];
				if (r == 10 || r == 100 || r == 1000 || r == 10000 || r == 100000 || r == 1000000)
				{
					char text[1100];
					_snprintf_s(text, sizeof text, _TRUNCATE, "(repeated %u times) %s", r, line);
					put(text);
				}
				return;
			}
		if (seenCount < C_SEEN)
		{
			seenHash[seenCount] = hash;
			seenRepeats[seenCount++] = 1;
		}
		put(line);
	}
	void open()
	{
		AcquireSRWLockExclusive(&lock);
		openLocked();
		ReleaseSRWLockExclusive(&lock);
	}
	void openLocked()
	{
		if (!tried)
		{
			tried = true;
			char		path[MAX_PATH + sizeof g_infoLogName];
			const DWORD n = GetTempPathA(MAX_PATH, path);
			if (n != 0 && n < MAX_PATH)
			{
				strcpy_s(path + n, sizeof path - n, name);
				if (fopen_s(&file, path, "w") != 0)
					file = nullptr;
			}
		}
	}
	void put(const char *line)
	{
		openLocked();
		if (file == nullptr || capped)
			return;
		const uint32_t length = uint32_t(strlen(line)) + 1;
		if (bytes + length > C_LOG_CAP)
		{
			capped = true;
			fprintf(file, "(log capped at %u bytes: the rest of this run is not written)\n", C_LOG_CAP);
			fflush(file);
			return;
		}
		bytes += length;
		fprintf(file, "%s\n", line);
		fflush(file);
	}
};
LogFile g_errorLog(g_errorLogName);
LogFile g_infoLog(g_infoLogName);

void __stdcall infoQueueMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID,
								LPCSTR description, void *)
{
	if (severity == D3D12_MESSAGE_SEVERITY_INFO || severity == D3D12_MESSAGE_SEVERITY_MESSAGE)
		return;
	// The Agility SDK warns that CheckFeatureSupport is superseded. The calls come from NVRHI and D3D12MA, which this
	// project does not modify, so the warning is filtered to keep the log limited to actionable messages.
	static const char C_SUPERSEDED[] = "ID3D12Device::CheckFeatureSupport is superseded";
	if (severity == D3D12_MESSAGE_SEVERITY_WARNING && strncmp(description, C_SUPERSEDED, sizeof C_SUPERSEDED - 1) == 0)
		return;
	render_backend::gpu::diag::report(severity != D3D12_MESSAGE_SEVERITY_WARNING, "d3d12", description);
}
} // namespace

void render_backend::gpu::diag::configure(const Config &config)
{
	const auto copy = [](char *out, size_t capacity, const char *text)
	{ strncpy_s(out, capacity, text != nullptr ? text : "", _TRUNCATE); };
	copy(g_infoLogName, sizeof g_infoLogName, config.infoLogName);
	copy(g_errorLogName, sizeof g_errorLogName, config.errorLogName);
	copy(g_prefix, sizeof g_prefix, config.prefix);
	copy(g_timingsPath, sizeof g_timingsPath, config.timingsPath);
	g_detailed = config.detailed || g_timingsPath[0] != 0;
}

const char *render_backend::gpu::diag::prefix()
{
	return g_prefix;
}

void render_backend::gpu::diag::startLogs()
{
	g_errorLog.open();
	g_infoLog.open();
}

void render_backend::gpu::diag::setMarker(const char *name)
{
	g_marker = name != nullptr ? name : "";
}

const char *render_backend::gpu::diag::marker()
{
	return g_marker;
}

void render_backend::gpu::diag::report(bool error, const char *source, const char *text)
{
	char line[1024];
	_snprintf_s(line, sizeof line, _TRUNCATE, "%s %s [%s] %s: %s", prefix(), error ? "error" : "warning",
				g_marker[0] != 0 ? g_marker : "-", source, text);
	OutputDebugStringA(line);
	OutputDebugStringA("\n");
	g_errorLog.write(line);
	if (error)
		++g_errors;
}

void render_backend::gpu::diag::info(const char *text)
{
	OutputDebugStringA(text);
	OutputDebugStringA("\n");
	g_infoLog.write(text);
}

void render_backend::gpu::startupMark(const char *what)
{
	// The first mark measures from process creation (GetProcessTimes), so loader and CRT startup are included. Later
	// marks add the performance counter time since the first mark.
	static LARGE_INTEGER frequency{}, first{};
	static double		 firstMs = 0.0, lastMs = 0.0;
	LARGE_INTEGER		 now;
	QueryPerformanceCounter(&now);
	if (frequency.QuadPart == 0)
	{
		QueryPerformanceFrequency(&frequency);
		first = now;
		FILETIME creation, exitTime, kernel, user, current;
		GetSystemTimePreciseAsFileTime(&current);
		if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
		{
			const auto ticks = [](const FILETIME &t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
			firstMs = double(ticks(current) - ticks(creation)) / 10000.0; // FILETIME is in 100 ns units.
		}
		lastMs = firstMs;
	}
	const double ms = firstMs + double(now.QuadPart - first.QuadPart) * 1000.0 / double(frequency.QuadPart);
	char		 text[512];
	_snprintf_s(text, sizeof text, _TRUNCATE, "start-up %7.0f ms (+%5.0f): %s", ms, ms - lastMs, what);
	lastMs = ms;
	diag::info(text);
}

namespace
{
const char *g_presentMark = "the first frame presented"; // Mark logged at the next present; null when none is pending.
} // namespace

void render_backend::gpu::startupMarkAtPresent(const char *what)
{
	g_presentMark = what;
}

void render_backend::gpu::logLine(const char *text)
{
	diag::info(text);
}

void render_backend::gpu::diag::presented()
{
	if (g_presentMark != nullptr)
		startupMark(g_presentMark);
	g_presentMark = nullptr;
}

bool render_backend::gpu::diag::detailed()
{
	return g_detailed;
}

uint32_t render_backend::gpu::diag::errorCount()
{
	return g_errors;
}

namespace
{
// Device that already has the callback, so recreating the renderer on the same device does not install it twice.
ID3D12Device *g_infoQueueDevice = nullptr;
} // namespace

void render_backend::gpu::diag::deviceReleased(ID3D12Device *device)
{
	if (device == g_infoQueueDevice)
		g_infoQueueDevice = nullptr; // A new device may reuse the address and needs its own callback.
}

void render_backend::gpu::diag::installInfoQueue(ID3D12Device *device)
{
	ID3D12InfoQueue1 *queue = nullptr;
	if (device == nullptr || device == g_infoQueueDevice || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
		return; // No debug layer: release build, or the Graphics Tools optional feature is not installed.
	g_infoQueueDevice = device;
	queue->RegisterMessageCallback(infoQueueMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &g_infoQueueCookie);
	queue->Release();
}

void render_backend::gpu::diag::frameStarted()
{
	g_inFrames = true;
}

void render_backend::gpu::diag::warmUpAgain()
{
	g_inFrames = false;
}

void render_backend::gpu::diag::setReloading(bool reloading)
{
	g_reloading = reloading;
}

void render_backend::gpu::diag::countPipeline(const char *key)
{
	if (!g_inFrames || g_reloading)
		return;
	++g_pipelines;
	report(true, "pipeline", (eastl::string("made in a frame (outside pipeline warm-up): ") + key).c_str());
}

void render_backend::gpu::diag::countShader(const char *key)
{
	if (!g_inFrames || g_reloading)
		return;
	++g_shaders;
	report(true, "shader", (eastl::string("made in a frame (outside pipeline warm-up): ") + key).c_str());
}

uint32_t render_backend::gpu::diag::pipelinesInFrames()
{
	return g_pipelines;
}

uint32_t render_backend::gpu::diag::shadersInFrames()
{
	return g_shaders;
}

uint32_t render_backend::gpu::errorCount()
{
	return diag::errorCount();
}

//----------------------------------------------------------------------------------------------------------------------
bool render_backend::gpu::diag::Timers::enabled()
{
	if (m_state == 0)
	{
		strcpy_s(m_path, g_timingsPath);
		m_state = m_path[0] != 0 ? 1 : 2;
	}
	return m_state == 1;
}

uint32_t render_backend::gpu::diag::Timers::rowOf(const char *path)
{
	// A linear search over the frame's distinct ranges, only when timing output is enabled.
	for (uint32_t i = 0; i < m_rows.size(); ++i)
		if (m_rows[i].path == path)
			return i;
	if (m_rows.size() + 1 >= C_ROWS)
	{
		if (m_rows.size() + 1 == C_ROWS)
			m_rows.push_back(Row{eastl::string("(more paths than rows)"), 0.0, 0});
		return C_ROWS - 1;
	}
	m_rows.push_back(Row{eastl::string(path), 0.0, 0});
	return uint32_t(m_rows.size() - 1);
}

void render_backend::gpu::diag::Timers::begin(nvrhi::IDevice *device, nvrhi::ICommandList *commands, const char *path, bool counted)
{
	if (!enabled())
		return;
	if (m_openCount >= C_OPEN)
	{
		++m_openCount; // Too deep to time; end() still pops it.
		++m_skipped;
		return;
	}
	Pending &open = m_open[m_openCount++];
	open.query = nullptr;
	open.row = rowOf(path);
	open.counted = counted;
	if (m_free.empty())
	{
		nvrhi::TimerQueryHandle q = m_made < C_TIMER_QUERIES ? device->createTimerQuery() : nullptr;
		if (!q)
		{
			++m_skipped; // All queries in flight or the heap is exhausted; this range is not timed.
			return;
		}
		m_free.push_back(q);
		++m_made;
	}
	open.query = m_free.back();
	m_free.pop_back();
	commands->beginTimerQuery(open.query);
}

void render_backend::gpu::diag::Timers::end(nvrhi::ICommandList *commands)
{
	if (m_openCount == 0)
		return;
	if (m_openCount-- > C_OPEN)
		return;
	Pending &open = m_open[m_openCount];
	if (!open.query)
		return;
	commands->endTimerQuery(open.query);
	m_pending.push_back(open);
	open.query = nullptr;
}

void render_backend::gpu::diag::Timers::collect(nvrhi::IDevice *device, uint32_t frameIndex)
{
	if (m_state != 1)
		return;
	++m_frames;
	for (size_t i = 0; i < m_pending.size();)
	{
		Pending &p = m_pending[i];
		if (!device->pollTimerQuery(p.query))
		{
			++i;
			continue;
		}
		Row &row = m_rows[p.row];
		row.ms += double(device->getTimerQueryTime(p.query)) * 1000.0;
		row.calls += p.counted ? 1u : 0u;
		device->resetTimerQuery(p.query);
		m_free.push_back(p.query);
		m_pending[i] = m_pending.back();
		m_pending.pop_back();
	}
	if (frameIndex % 300 != 0 || m_rows.empty())
		return;
	FILE *f = nullptr;
	if (fopen_s(&f, m_path, "w") != 0 || f == nullptr)
		return;
	fprintf(f,
			"# GPU time per marker range (timestamp queries), %u frames; queries made %u, ranges not timed %u\n"
			"# a range's time includes the ranges under it; rows in the frame's order (a range under the one it opened "
			"in), indented by depth\n",
			m_frames, m_made, m_skipped);
	fprintf(f, "%-72s %12s %10s %12s\n", "range", "ms/frame", "calls", "ms/call");
	// Print rows as a tree in frame order. Each row's sort key is the list of row indices of its path prefixes followed
	// by its own index, so parents come before children and siblings keep first-seen order. A prefix without a row of
	// its own is skipped.
	eastl::vector<eastl::vector<uint32_t>> keys(m_rows.size());
	for (uint32_t i = 0; i < m_rows.size(); ++i)
	{
		const eastl::string &path = m_rows[i].path;
		for (size_t at = path.find('/'); at != eastl::string::npos; at = path.find('/', at + 1))
			for (uint32_t k = 0; k < m_rows.size(); ++k)
				if (m_rows[k].path.size() == at && path.compare(0, at, m_rows[k].path) == 0)
				{
					keys[i].push_back(k);
					break;
				}
		keys[i].push_back(i);
	}
	eastl::vector<uint32_t> order;
	for (uint32_t i = 0; i < m_rows.size(); ++i)
		order.push_back(i);
	eastl::sort(order.begin(), order.end(), [&keys](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
	for (uint32_t i : order)
	{
		const Row  &r = m_rows[i];
		uint32_t	depth = 0;
		const char *leaf = r.path.c_str();
		for (const char *c = r.path.c_str(); *c != 0; ++c)
			if (*c == '/')
			{
				++depth;
				leaf = c + 1;
			}
		char label[256];
		_snprintf_s(label, sizeof label, _TRUNCATE, "%*s%s", int(depth * 2), "", leaf);
		fprintf(f, "%-72s %12.4f %10u %12.5f\n", label, r.ms / m_frames, r.calls, r.calls != 0 ? r.ms / r.calls : 0.0);
	}
	fclose(f);
}

void render_backend::gpu::diag::Timers::clear()
{
	m_openCount = 0;
	for (Pending &p : m_open)
		p.query = nullptr;
	m_pending.clear();
	m_free.clear();
	// All queries were released above, so the C_TIMER_QUERIES limit applies afresh on a recreated device.
	m_made = 0;
	m_skipped = 0;
}
