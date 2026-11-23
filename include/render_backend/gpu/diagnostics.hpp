// Render diagnostics: marker-scoped messages, timestamp queries and resource-creation counters.
#pragma once
#include <nvrhi/nvrhi.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <stdint.h>

struct ID3D12Device;

namespace render_backend::gpu::diag
{
struct Config
{
	const char *infoLogName = "render.log";
	const char *errorLogName = "render_errors.log";
	const char *prefix = "renderer";
	const char *timingsPath = nullptr;
	bool		detailed = false;
};

// Supply host diagnostics policy before the first log write or renderer initialization. The values are copied.
void		configure(const Config &config);
const char *prefix();

// Name of the pass being recorded, included in reports. Empty between passes. The string must outlive its use.
void		setMarker(const char *name);
const char *marker();

// Truncates both logs. Call once at startup, before any message, so a run without messages does not leave the previous
// run's logs behind.
void startLogs();

// Sends a warning or error, tagged with the current marker, to the debugger and to the error log in %TEMP%. Errors
// increment errorCount(). Both logs are recreated each run and stay open. A repeated line is written once, then again
// with its count at 10, 100, ... repeats. Each log stops at 1 MiB.
void report(bool error, const char *source, const char *text);
// Writes an information line to the debugger and to the info log in %TEMP%.
void	 info(const char *text);
void	 presented(); // Call after each present; logs the pending startupMarkAtPresent mark once.
uint32_t errorCount();

// True when detailed diagnostics were requested, for example timing a second pipeline creation to measure the warm
// cache.
bool detailed();

// Forwards D3D12 debug layer messages through ID3D12InfoQueue1. Does nothing when the debug layer is not present.
void installInfoQueue(ID3D12Device *device);
// Call when a device is released, so a new device at the same address gets the callback installed again.
void deviceReleased(ID3D12Device *device);

// Pipelines and shaders created during frames. All of them are expected to be created during warm-up, before
// frameStarted(). Each later creation is reported as an error with its key, except while setReloading(true) is in
// effect (shader hot reload).
void	 frameStarted();
void	 warmUpAgain(); // Call when the renderer is recreated (for example after device loss) to re-enter warm-up.
void	 countPipeline(const char *key);
void	 countShader(const char *key);
void	 setReloading(bool reloading);
uint32_t pipelinesInFrames();
uint32_t shadersInFrames();

// GPU time per marker range, enabled by Config::timingsPath. Each timed range of the marker stack (markers.hpp)
// gets a pair of timestamp queries; nested ranges each have their own. Results are read once the frame has completed,
// without waiting. Queries come from a free list, up to C_TIMER_QUERIES in total; a range that finds none is not timed
// and counts as skipped. The table has one row per range path ("frame/view/pass/stage"), up to C_ROWS; further paths
// share the last row. The table is written as a tree every 300 frames.
class Timers
{
  public:
	// Keep below the device's timer query heap size (nvrhi::d3d12::DeviceDesc::maxTimerQueries).
	static constexpr uint32_t C_TIMER_QUERIES = 1000;
	static constexpr uint32_t C_ROWS = 1024;
	static constexpr uint32_t C_OPEN = 24; // Maximum nesting depth timed; deeper ranges are skipped.
	bool					  enabled(); // A timing output path was configured.
	// Opens a range for `path`. `counted` adds a call; pass false when resuming a range in a new command list so only
	// its time is added.
	void	 begin(nvrhi::IDevice *device, nvrhi::ICommandList *commands, const char *path, bool counted);
	void	 end(nvrhi::ICommandList *commands);				   // Closes the innermost open range.
	// Accumulates completed queries and periodically writes the table.
	void	 collect(nvrhi::IDevice *device, uint32_t frameIndex);
	void	 clear(); // Releases all queries. Call before the device is destroyed.
	uint32_t queries() const
	{
		return m_made;
	}

  private:
	struct Pending
	{
		nvrhi::TimerQueryHandle query;
		uint32_t				row = 0;
		bool					counted = false;
	};
	struct Row
	{
		eastl::string path;
		double		  ms;
		uint32_t	  calls;
	};
	uint32_t							   rowOf(const char *path);
	eastl::vector<nvrhi::TimerQueryHandle> m_free;
	eastl::vector<Pending>				   m_pending;
	eastl::vector<Row>					   m_rows;
	Pending								   m_open[C_OPEN];
	uint32_t							   m_openCount = 0; // May exceed C_OPEN; those ranges are not timed.
	uint32_t							   m_made = 0, m_frames = 0, m_skipped = 0;
	int									   m_state = 0; // 0: not checked yet, 1: enabled, 2: disabled.
	char								   m_path[260] = {};
};
} // namespace render_backend::gpu::diag

namespace render_backend::gpu
{
void	 startupMark(const char *what);
void	 startupMarkAtPresent(const char *what);
void	 logLine(const char *text);
uint32_t errorCount();
} // namespace render_backend::gpu
