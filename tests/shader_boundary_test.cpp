#include <render_backend/shader/auto_constants.hpp>
#include <render_backend/shader/shader_cache.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
using namespace render_backend::shader;

bool nativeTechnique(const char *root)
{
	namespace fs = std::filesystem;
	constexpr const char source[] = R"(
cbuffer Spin
{
    float2 offset;
};

float4 Raster(float2 position : POSITION) : SV_Position { return float4(position + offset, 0.0, 1.0); }
float4 Tint() : SV_Target { return float4(0.2, 0.3, 0.4, 1.0); }

technique Native
{
    pass Main
    {
        VertexShader = Raster();
        PixelShader = Tint();
    }
}
)";
	std::error_code		 errorCode;
	const fs::path		 effects = fs::path(root) / "effects";
	fs::create_directories(effects, errorCode);
	if (errorCode)
		return false;
	FILE *file = nullptr;
	if (_wfopen_s(&file, (effects / "native.hlsl").c_str(), L"wb") != 0)
		return false;
	const size_t written = fwrite(source, 1, sizeof source - 1, file);
	const int	 closed = fclose(file);
	if (written != sizeof source - 1 || closed != 0)
		return false;

	ShaderCache					cache;
	eastl::string				error;
	eastl::vector<PipelineDesc> pipelines;
	if (!cache.open(root, ShaderCache::Mode::Develop, error) || !cache.pipelines("native", pipelines, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return false;
	}
	if (pipelines.size() != 1 || pipelines[0].technique != "Native" || pipelines[0].pass != "Main")
	{
		std::fprintf(stderr, "expected one pipeline, Native/Main; got %zu\n", pipelines.size());
		return false;
	}
	const PipelineDesc &pipeline = pipelines[0];
	if (pipeline.path != Path::Vertices || pipeline.shaders[uint32_t(Stage::Vertex)].empty() ||
		pipeline.shaders[uint32_t(Stage::Pixel)].empty() || pipeline.state.blend != BlendMode::None ||
		!pipeline.targets.name.empty() || !pipeline.targets.colors.empty() || !pipeline.targets.depth.empty())
	{
		std::fprintf(stderr, "expected a vertex/pixel pipeline without declared framebuffer formats or blending\n");
		return false;
	}
	// Compile both stages in the source tree first: deploy copies only what has been compiled.
	eastl::vector<uint8_t> code;
	BuildEntry			   report;
	if (!cache.getShader("native", "Native", "Main", Stage::Vertex, {}, code, report) || code.empty() ||
		report.entry != "Raster")
	{
		std::fprintf(stderr, "vertex shader: %s\n", report.messages.c_str());
		return false;
	}
	if (!cache.getShader("native", "Native", "Main", Stage::Pixel, {}, code, report) || code.empty() ||
		report.entry != "Tint")
	{
		std::fprintf(stderr, "pixel shader: %s\n", report.messages.c_str());
		return false;
	}
	// Prebuilt mode reads a deployed folder (described by its deployed_effects.txt), not the source tree.
	ShaderCache			prebuilt;
	const eastl::string deployed = eastl::string(root) + "\\deployed";
	uint32_t			files = 0;
	if (!cache.deploy(deployed.c_str(), files, error) ||
		!prebuilt.open(deployed.c_str(), ShaderCache::Mode::Prebuilt, error))
	{
		std::fprintf(stderr, "deploy: %s\n", error.c_str());
		return false;
	}
	EffectDesc desc;
	if (!prebuilt.loadEffect("native", desc, error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return false;
	}
	const TechniqueDesc *technique = desc.findTechnique("Native");
	if (!technique || technique->passes.size() != 1 || !technique->passes[0].vertices ||
		desc.registers.size() != 1)
	{
		std::fprintf(stderr, "expected an inferred vertex path and one automatic register\n");
		return false;
	}
	const RegisterDesc &spin = desc.registers[0];
	if (spin.name != "Spin" || spin.kind != 'b' || spin.index != 0 || spin.space != 0)
	{
		std::fprintf(stderr, "expected Spin at constant-buffer register b0, space 0\n");
		return false;
	}
	if (!prebuilt.getShader("native", "Native", "Main", Stage::Pixel, {}, code, report) || code.empty() ||
		report.entry != "Tint")
	{
		std::fprintf(stderr, "prebuilt pixel shader: %s\n", report.messages.c_str());
		return false;
	}
	return true;
}

bool namedAutomaticBinding()
{
	EffectDesc desc;
	desc.name = "standalone_binding";
	desc.recordSize[uint32_t(Rate::Material)] = sizeof(float);
	ParamDesc &value = desc.params.push_back();
	value.name = "parameter";
	value.type = "float";
	value.semantic = "USER_VALUE";
	value.rate = Rate::Material;
	value.use = C_USE_RECORD;
	value.offset = 0;
	value.size = sizeof(float);

	AutoConstants bindings;
	bindings.registerConstant<float>(
		"USER_VALUE", Rate::Material, [](const AutoContext &, float &out, void *) { out = 0.625f; }, nullptr);
	float result = 0.0f;
	return bindings.fillRecord(desc, Rate::Material, {}, reinterpret_cast<uint8_t *>(&result), sizeof result) && result == 0.625f;
}
} // namespace

int main(int argc, char **argv)
{
	namespace fs = std::filesystem;
	if (argc != 2)
		return 2;
	// Work in a new directory so that output left by an earlier run cannot satisfy this one.
	std::error_code error;
	const fs::path	run =
		fs::path(argv[1]) / ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(run, error);
	if (error)
	{
		std::fprintf(stderr, "cannot create %s\n", run.string().c_str());
		return 1;
	}
	const bool technique = nativeTechnique(run.string().c_str());
	const bool binding = namedAutomaticBinding();
	fs::remove_all(run, error);
	std::printf("authored technique: %s; automatic binding: %s\n", technique ? "PASS" : "FAIL",
				binding ? "PASS" : "FAIL");
	return technique && binding ? 0 : 1;
}
