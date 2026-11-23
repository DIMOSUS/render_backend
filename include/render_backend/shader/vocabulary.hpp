// Names whose meaning belongs to the host: the semantics it fills per draw into an effect's instance record, the
// semantics it provides itself (material slots, render targets, annotations), and the built-in textures a texture
// parameter may name as its default. The host registers its tables once, before any effect is parsed. The front-end
// rejects an effect that uses a semantic or texture default that no table (and no prelude or derived binding) knows.
//
// The tables are process-wide globals and are not copied: they must stay alive for as long as effects are parsed.
#pragma once
#include <stdint.h>

namespace render_backend::shader
{
struct InstanceSemantic
{
	const char *semantic = nullptr; // as an effect parameter declares it (case ignored)
	uint8_t		tag = 0;			// the host's own value, returned by instanceSemantic()
};
// Semantics the host fills per draw. A parameter with one of them must be declared in the instance block. null, 0:
// none.
void setInstanceSemantics(const InstanceSemantic *table, uint32_t count);
// The registered entry for the semantic (case ignored), or null.
const InstanceSemantic *instanceSemantic(const char *semantic);

// Semantics the host provides besides the prelude's frame block and the derived ones; usable in any block. null, 0:
// none.
void setProvidedSemantics(const char *const *names, uint32_t count);
// Whether the semantic is a registered provided one (case ignored).
bool providedSemantic(const char *semantic);

// The host's built-in textures by name (Texture2D t = NAME;), bound where a material sets none. null, 0: none, and a
// texture parameter with a default is rejected.
void setTextureDefaults(const char *const *names, uint32_t count);
// The registered table (count: its size).
const char *const *textureDefaults(uint32_t &count);
} // namespace render_backend::shader
