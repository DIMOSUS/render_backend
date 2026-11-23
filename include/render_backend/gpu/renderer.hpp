// A small native vertex renderer over the shared frame graph and command playback.
#pragma once
#include <stdint.h>
#include <memory>
#include <string>

namespace render_backend::gpu::demo
{
struct Vertex
{
	float position[3];
	float normal[3];
	float color[3];
};

class Mesh
{
  public:
	Mesh();
	~Mesh();
	Mesh(Mesh &&) noexcept;
	Mesh &operator=(Mesh &&) noexcept;
	Mesh(const Mesh &) = delete;
	Mesh	&operator=(const Mesh &) = delete;
	explicit operator bool() const;

  private:
	friend class Renderer;
	struct Storage;
	std::unique_ptr<Storage> storage;
};

// The caller owns the window, which must outlive the renderer. Each renderer runs its own render worker thread
// (packets::Worker) from create() until destruction.
class Renderer
{
  public:
	static std::unique_ptr<Renderer> create(void *hwnd, const char *shaderRoot, std::string &error);
	~Renderer();
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	Mesh	  createMesh(const Vertex *vertices, uint32_t vertexCount);
	// The effect declares one constant buffer with an automatic register. `constants` supplies up to 256 bytes.
	// Minimized windows skip rendering.
	bool frame(const Mesh &mesh, const char *effect, const char *technique, const char *pass, const void *constants,
			   uint32_t bytes, const float clear[4]);
	const std::string &lastError() const;

  private:
	Renderer();
	struct State;
	std::unique_ptr<State> state;
};
} // namespace render_backend::gpu::demo
