#include <render_backend/shader/shader_cache.hpp>
#include <render_backend/shader/watcher.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{
namespace fs = std::filesystem;
using namespace render_backend::shader;
using Clock = std::chrono::steady_clock;

constexpr char C_EFFECT[] = R"(
#include "palette.hlsli"

selection bool Alternate = false : [false, true];

float4 Position(float2 position : POSITION) : SV_Position { return float4(position, 0, 1); }
float4 Shade() : SV_Target { return float4(PaletteColor(), Alternate ? 1.0 : 0.0, 0, 1); }

technique Display
{
    pass Draw
    {
        VertexShader = Position();
        PixelShader = Shade();
    }
}
)";
constexpr char C_PALETTE[] = "#include \"colors.hlsli\"\n"
							 "float PaletteColor() { return BaseColor(); }\n";

class TemporaryTree
{
  public:
	explicit TemporaryTree(const fs::path &base)
	{
		std::error_code error;
		fs::create_directories(base, error);
		if (error)
			return;
		const fs::path candidate = base / ("case-" + std::to_string(Clock::now().time_since_epoch().count()));
		if (fs::create_directory(candidate, error))
			path = candidate;
	}

	~TemporaryTree()
	{
		if (!path.empty())
		{
			std::error_code error;
			fs::remove_all(path, error);
		}
	}

	fs::path path;
};

bool writeText(const fs::path &path, const char *text)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << text;
	file.close();
	return bool(file);
}

bool expect(bool ok, const char *message)
{
	if (!ok)
		std::fprintf(stderr, "%s\n", message);
	return ok;
}

bool expectReport(const eastl::vector<BuildEntry> &report, BuildResult description, BuildResult variants,
		  BuildReason reason)
{
	if (!expect(report.size() == 4, "expected one description and three unique shader variants"))
		return false;
	for (const BuildEntry &entry : report)
		if (entry.result != (entry.description ? description : variants) || entry.reason != reason)
		{
			std::fprintf(stderr, "%s.%s: unexpected result %u (%s): %s\n", entry.effect.c_str(),
						 entry.variant.c_str(), unsigned(entry.result), reasonName(entry.reason), entry.messages.c_str());
			return false;
		}
	return true;
}

template <class Accept> bool waitFor(Watcher &watcher, Accept accept, const char *message)
{
	const auto deadline = Clock::now() + std::chrono::seconds(15);
	eastl::vector<ShaderChange> changes;
	do
	{
		watcher.poll(changes);
		for (const ShaderChange &change : changes)
			if (accept(change))
				return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	} while (Clock::now() < deadline);
	return expect(false, message);
}

bool exercise(const fs::path &root)
{
	const fs::path effects = root / "effects";
	std::error_code fileError;
	if (!expect(fs::create_directory(effects, fileError), "could not create synthetic shader directory"))
		return false;
	const fs::path colors = effects / "colors.hlsli";
	if (!expect(writeText(effects / "palette.hlsl", C_EFFECT) && writeText(effects / "palette.hlsli", C_PALETTE) &&
					writeText(colors, "float BaseColor() { return 0.125; }\n"),
				"could not write synthetic shaders"))
		return false;

	ShaderCache cache;
	eastl::string error;
	const std::string directory = root.string();
	if (!cache.open(directory.c_str(), ShaderCache::Mode::Develop, error))
		return expect(false, error.c_str());

	eastl::vector<BuildEntry> report;
	cache.updateAll(false, report);
	if (!expectReport(report, BuildResult::Compiled, BuildResult::Compiled, BuildReason::NoManifest))
		return false;
	eastl::vector<PipelineDesc> pipelines;
	if (!cache.pipelines("palette", pipelines, error))
		return expect(false, error.c_str());
	constexpr auto vs = uint32_t(Stage::Vertex);
	constexpr auto ps = uint32_t(Stage::Pixel);
	if (!expect(pipelines.size() == 2 && pipelines[0].shaders[vs] == pipelines[1].shaders[vs] &&
					pipelines[0].shaders[ps] != pipelines[1].shaders[ps],
				"the selection must vary only the pixel shader"))
		return false;
	const auto loadPixel = [&](const SelectionValues &values, eastl::vector<uint8_t> &bytes, BuildEntry &entry)
	{
		if (!cache.getShader("palette", "Display", "Draw", Stage::Pixel, values, bytes, entry))
			return expect(false, entry.messages.c_str());
		return true;
	};
	eastl::vector<uint8_t> original, alternate;
	BuildEntry entry;
	if (!loadPixel({0}, original, entry) || !loadPixel({1}, alternate, entry) ||
		!expect(!original.empty() && !alternate.empty() && original != alternate,
				"each selection must compile into distinct pixel bytecode"))
		return false;
	cache.updateAll(true, report);
	if (!expectReport(report, BuildResult::UpToDate, BuildResult::UpToDate, BuildReason::UpToDate))
		return false;

	// An include-only edit renews the description manifest without rewriting identical metadata.
	if (!expect(writeText(colors, "float BaseColor() { return 0.375; }\n"), "could not edit transitive include"))
		return false;
	cache.updateAll(true, report);
	if (!expectReport(report, BuildResult::Stale, BuildResult::Stale, BuildReason::IncludeChanged))
		return false;
	cache.updateAll(false, report);
	if (!expectReport(report, BuildResult::UpToDate, BuildResult::Compiled, BuildReason::IncludeChanged))
		return false;
	cache.updateAll(true, report);
	if (!expectReport(report, BuildResult::UpToDate, BuildResult::UpToDate, BuildReason::UpToDate))
		return false;
	eastl::vector<uint8_t> valid;
	if (!loadPixel({0}, valid, entry) || !expect(valid != original, "updated include did not change pixel bytecode"))
		return false;
	const eastl::string variant = entry.variant;

	eastl::vector<uint8_t> recovered;
	Watcher watcher;
	if (!watcher.start(cache, error))
		return expect(false, error.c_str());
	if (!expect(writeText(colors, "float BaseColor() { return not_declared; }\n"), "could not write invalid source"))
		return false;
	const auto selected = [&](const ShaderChange &change)
	{ return change.effect == "palette" && change.variant == variant; };
	if (!waitFor(watcher, [&](const ShaderChange &change)
					{ return selected(change) && change.kind == ShaderChange::Kind::Error && !change.message.empty(); },
				 "watcher did not report the compilation error"))
		return false;
	if (!loadPixel({0}, recovered, entry) ||
		!expect(entry.result == BuildResult::Failed && recovered == valid,
				"a failed edit did not retain the last good shader"))
		return false;

	if (!expect(writeText(colors, "float BaseColor() { return 0.625; }\n"), "could not repair invalid source"))
		return false;
	bool cleared = false;
	eastl::vector<uint8_t> published;
	if (!waitFor(watcher, [&](const ShaderChange &change)
				 {
					 if (selected(change))
					 {
						 cleared |= change.kind == ShaderChange::Kind::Cleared;
						 if (change.kind == ShaderChange::Kind::Shader)
							 published = change.bytecode;
					 }
					 return cleared && !published.empty();
				 },
				 "watcher did not clear the error and publish the repaired shader"))
		return false;
	if (!loadPixel({0}, recovered, entry) ||
		!expect(entry.result == BuildResult::UpToDate && recovered == published && recovered != valid,
				"watcher and cache did not agree on repaired bytecode"))
		return false;
	watcher.stop();

	// With the watcher stopped, an output whose bytes no longer match its manifest must be rebuilt.
	const auto bytecodePath = compiledPath(cache.root(), "palette", variant.c_str(), ".dxil");
	if (!expect(writeText(fs::path(bytecodePath.c_str()), "corrupt"), "could not replace cached bytecode"))
		return false;
	return loadPixel({0}, recovered, entry) &&
		   expect(entry.result == BuildResult::Compiled && entry.reason == BuildReason::NoOutput &&
					  recovered == published,
				  "cached bytecode failed to recover from an invalid output hash");
}
} // namespace

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	TemporaryTree tree(argv[1]);
	if (!expect(!tree.path.empty(), "could not create an isolated shader cache test directory"))
		return 1;
	const bool ok = exercise(tree.path);
	std::printf("shader permutations, cache invalidation and reload recovery: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
