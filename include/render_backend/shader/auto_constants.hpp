// Auto-constants: effect parameters the renderer fills by itself (AMBIENT, TIME, VIEW, a texture of a given semantic
// ...) without the material or the draw naming them. The host registers each one once, by name, with its type, its
// rate and a provider: a function that computes the value from an AutoContext (the frame, view, instance and
// material the renderer is filling a record for).
//
// Records. The renderer's frame header and instance header (declared by the prelude) are not effect records; the
// renderer fills them itself. Each effect has its own records (effect_desc.hpp): a frame record (the frame
// parameters it uses, filled once per frame per effect), its instance data (its own instance parameters, after the
// instance header) and its material record. When the renderer fills one of them (never per draw), fillRecord writes
// every field: the matching auto-constant's value if there is one, else the declared default, else zeros.
//
// What is written. Only record fields (ParamDesc::use has C_USE_RECORD). Never written: unused parameters,
// state-only parameters, parameters the shader computes from the headers (C_USE_DERIVED: WORLD, ALPHA ...), aliases
// (ParamDesc::alias: another name of a prelude parameter, with no field of its own), and the material header at the
// start of a material record.
//
// Matching. A field matches the registration named like the parameter (case-sensitive), else the one named like its
// semantic (case-insensitive). A semantic decides only when exactly one field of the record that is not registered by
// its own name has it. A shared semantic fills none of its fields (check() reports each declaration; they keep their
// defaults) until all but one are registered by name. Textures, samplers and values are separate name spaces: the
// texture DIFFUSE (a heap index) and a float3 with semantic Diffuse do not match each other.
//
// Type check. A registration records its type (float4x4, float3, int, bool, a texture of a dimension, a sampler, or a
// named raw type: a struct or byte array) and its rate. check() compares them with the declarations and reports each
// mismatch at its declaration ("<file>:<line>: g_mView (VIEW): declared float4x4, the auto-constant VIEW is
// float4x3"). A registration that disagrees with a declaration is never used for it and the field keeps its default
// (writing a float4x4 into a float4x3 field would overwrite the next field). Registering an alias's own name is also
// an error. In Develop mode the shader cache calls check() on every description it hands out and refuses it on
// errors. A Prebuilt cache does not, so the renderer should call check() after loadEffect and log the notes: fields
// with a semantic nobody registered (a typo like ": WORDL" shows up there), shared semantics, and registrations of
// computed parameters.
//
// Layout. Records are tightly packed in 4-byte units with no 16-byte rows (the generated entries read them with
// ByteAddressBuffer.Load<T>). Matrices are row-major as declared: a float4x3 is 4 rows of 3 floats, 48 bytes. bool is
// a 32-bit 0/1; textures and samplers are 32-bit ResourceDescriptorHeap / SamplerDescriptorHeap indices.
//
// Plans. For each effect AutoConstants keeps a plan: per rate, the record's default image (defaults and zeros, built
// once) and the list of (offset, registration) pairs, so filling a record is one memcpy and one call per
// auto-constant. Plans are keyed by the effect's name and the description's address, with at most two per effect
// (a third description takes the slot the last fill did not use). fillRecord builds the plan on first use and
// rebuilds it when the description is another object, its layout hash or parameter count changed, or the
// registrations changed. A reload assigned into the same object with the same layout but other defaults or semantics
// is not detected; forget() drops the effect's plans by name, and the shader cache calls it for every rebuilt
// description. Two live descriptions of one effect (an effect and its alias loaded separately) keep a plan each.
// check() never creates a plan.
//
// Threading. One SRW lock guards the registrations and the plans. register / unregister / forget / fillRecord take
// it exclusively; check and the counts take it shared, so the shader cache can check a description on the watcher
// thread while the render thread fills records. Providers run under the lock on the thread that calls fillRecord
// and must not call back into the same AutoConstants (the lock is not recursive).
#pragma once
#include <render_backend/shader/effect_desc.hpp>
#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace render_backend::shader
{
// What a provider computes its value from: opaque pointers to the renderer's objects for the record being filled
// (any may be null when the rate has none; a frame record has no instance), user data of the fill call, and, set by
// fillRecord, the effect and the parameter (or sampler) being written.
struct AutoContext
{
	const void *frame = nullptr;	// the frame's state (time, sun, fog ...)
	const void *view = nullptr;		// the camera the frame is drawn from
	const void *instance = nullptr; // an instance record: the object drawn
	const void *material = nullptr; // a material record: the material
	void	   *data = nullptr;		// anything else the caller of fillRecord passes through
	// This fill's own fallback (null: the rate's registered one, if any). It is called, like an AutoRawProvider with
	// fallbackUser, for every record field and material sampler that no registration names, so the caller can supply
	// values for this one record without registering a fallback for the whole rate.
	void (*fallback)(const AutoContext &context, void *out, uint32_t size, void *user) = nullptr;
	void *fallbackUser = nullptr;

	const EffectDesc  *effect = nullptr;  // set by fillRecord
	const ParamDesc	  *param = nullptr;	  // set by fillRecord: the parameter written (null for a sampler)
	const SamplerDesc *sampler = nullptr; // set by fillRecord: the sampler written (null for a parameter)
};

// The value types an auto-constant can have. The HLSL type of a declaration maps to one of them (half -> float,
// dword -> uint); a type outside the list (bool3, a struct, an array) is registered as Raw with its HLSL name.
enum class AutoType : uint8_t
{
	Float,
	Float2,
	Float3,
	Float4,
	Float3x2,
	Float3x3,
	Float3x4,
	Float4x3,
	Float4x4,
	Int,
	Int2,
	Int3,
	Int4,
	UInt,
	UInt2,
	UInt3,
	UInt4,
	Bool,	 // a 32-bit 0/1 in the record
	Texture, // a ResourceDescriptorHeap index (a parameter of C_USE_TEXTURE)
	Sampler, // a SamplerDescriptorHeap index (a SamplerDesc of the material record)
	Raw,	 // bytes of a named HLSL type: a struct (LightPoint), an array, a shape the list does not have
};
// "float4x4", "texture", "sampler", "raw" ...
const char *autoTypeName(AutoType type);
// bytes of one value in a record (Raw, Texture, Sampler: 4 - Raw's real size is the registration's)
uint32_t autoTypeSize(AutoType type);
// the type of an HLSL type as declared (float4x3 -> Float4x3, half2 -> Float2, Texture2D -> Texture); false when it
// has none of the value types (a struct, a string, bool3)
bool autoTypeOf(const eastl::string &hlslType, AutoType &out);

// The value structs a typed provider writes: tightly packed, row-major like the records (a row-major 4x4 C++ matrix
// is a Float4x4 as it is).
struct Float2
{
	float x, y;
};
struct Float3
{
	float x, y, z;
};
struct Float4
{
	float x, y, z, w;
};
struct Float3x2
{
	float m[3][2];
};
struct Float3x3
{
	float m[3][3];
};
struct Float3x4
{
	float m[3][4];
};
struct Float4x3
{
	float m[4][3];
};
struct Float4x4
{
	float m[4][4];
};
struct Int2
{
	int32_t x, y;
};
struct Int3
{
	int32_t x, y, z;
};
struct Int4
{
	int32_t x, y, z, w;
};
struct UInt2
{
	uint32_t x, y;
};
struct UInt3
{
	uint32_t x, y, z;
};
struct UInt4
{
	uint32_t x, y, z, w;
};

// the AutoType of a provider's value type
template <typename T> struct AutoTypeOf;
#define RB_AUTO_TYPE(T, E)                                                                                             \
	template <> struct AutoTypeOf<T>                                                                                   \
	{                                                                                                                  \
		static constexpr AutoType C_TYPE = AutoType::E;                                                                \
	};
RB_AUTO_TYPE(float, Float)
RB_AUTO_TYPE(Float2, Float2)
RB_AUTO_TYPE(Float3, Float3)
RB_AUTO_TYPE(Float4, Float4)
RB_AUTO_TYPE(Float3x2, Float3x2)
RB_AUTO_TYPE(Float3x3, Float3x3)
RB_AUTO_TYPE(Float3x4, Float3x4)
RB_AUTO_TYPE(Float4x3, Float4x3)
RB_AUTO_TYPE(Float4x4, Float4x4)
RB_AUTO_TYPE(int32_t, Int)
RB_AUTO_TYPE(Int2, Int2)
RB_AUTO_TYPE(Int3, Int3)
RB_AUTO_TYPE(Int4, Int4)
RB_AUTO_TYPE(uint32_t, UInt)
RB_AUTO_TYPE(UInt2, UInt2)
RB_AUTO_TYPE(UInt3, UInt3)
RB_AUTO_TYPE(UInt4, UInt4)
RB_AUTO_TYPE(bool, Bool)
#undef RB_AUTO_TYPE

// A typed provider: writes the value into out (which starts zeroed). user is the pointer given at registration,
// typically the owning object.
template <typename T> using AutoProvider = void (*)(const AutoContext &context, T &out, void *user);
// a texture or sampler provider: the heap index (0 when there is nothing to bind - the renderer's null descriptor)
using AutoIndexProvider = void (*)(const AutoContext &context, uint32_t &index, void *user);
// a raw provider: size bytes at out, the parameter's field in the record (it holds the default when the provider runs)
using AutoRawProvider = void (*)(const AutoContext &context, void *out, uint32_t size, void *user);

// what one fillRecord did
struct FillReport
{
	uint32_t provided = 0;	  // parameters written by an auto-constant
	uint32_t defaulted = 0;	  // parameters left with their declared default
	uint32_t zeroed = 0;	  // parameters left zero: no auto-constant, no default (samplers and textures included)
	bool	 rebuilt = false; // the effect's plan was (re)built for this fill
};

class AutoConstants
{
  public:
	AutoConstants() = default;
	AutoConstants(const AutoConstants &) = delete;
	AutoConstants &operator=(const AutoConstants &) = delete;

	// a value auto-constant: name is the semantic (or a parameter's name, see the top of the file); false when the name
	// is already registered as a value or the provider is null. The value starts zeroed for every call.
	template <typename T> bool registerConstant(const char *name, Rate rate, AutoProvider<T> provider, void *user)
	{
		return add(name, rate, AutoTypeOf<T>::C_TYPE, TextureDim::None, nullptr, 0,
				   reinterpret_cast<Function>(provider), &callTyped<T>, user);
	}
	// a texture: the heap index of its SRV. dim: the dimension the declaration must have (None: any)
	bool registerTexture(const char *name, Rate rate, AutoIndexProvider provider, void *user,
						 TextureDim dim = TextureDim::None);
	// a sampler of the material record, by the sampler's name: the heap index of its sampler descriptor
	bool registerSampler(const char *name, AutoIndexProvider provider, void *user);
	// a parameter of a type the value list does not have: hlslType as declared (LightPoint, bool3 ...), count its
	// array length (0: not an array) - both must match the declaration, and so must the record size
	bool registerRaw(const char *name, Rate rate, const char *hlslType, uint32_t count, AutoRawProvider provider,
					 void *user);

	// The fallback of a rate: called for every record field of that rate (value or texture; for the material rate
	// also every sampler) that no registration names. It lets the host supply values by parameter name (for example
	// from a material system) without registering every parameter of every effect. The provider gets the parameter
	// (context.param, or context.sampler) and its field, which holds the default when it runs, and writes the bytes
	// (a uint32_t heap index for a texture or sampler). Derived and alias parameters are never written. One per rate;
	// false when the rate has one already. Removed with unregisterFallback or unregisterUser. A fill with its own
	// fallback (AutoContext::fallback) uses that one instead.
	bool registerFallback(Rate rate, AutoRawProvider provider, void *user);
	bool unregisterFallback(Rate rate);
	// every registration of that name (values, textures, samplers); false if there was none
	bool unregisterConstant(const char *name);
	// every registration made with that user pointer (an owner going away); how many
	uint32_t unregisterUser(const void *user);
	uint32_t count() const; // the live registrations

	// Checks the registrations against a description. errors: a record field whose registration has another type
	// (count, texture dimension) or rate ("<file>:<line>: g_mView (VIEW): declared float4x4, the auto-constant VIEW is
	// float4x3"), or a registration by an alias's own name. notes (optional), each at its declaration: a record field
	// with a semantic nobody registered ("...: no auto-constant SPECULAR (t_SpecularTexture): it stays zero"), a
	// registered semantic that several fields share, or a registration of a parameter the shader computes. Creates no
	// plan, so desc may be a temporary. false when there are errors (fillRecord leaves those fields at their default).
	bool check(const EffectDesc &desc, eastl::vector<eastl::string> &errors,
			   eastl::vector<eastl::string> *notes = nullptr);
	// one record of a rate of the description: the defaults (zeros where there are none), then every auto-constant of
	// that rate that matches. Leaves the material header (the first 32 bytes of a material record) as it is. false when
	// the effect has no record of that rate or size is smaller than desc.recordSize[rate] (nothing written).
	bool fillRecord(const EffectDesc &desc, Rate rate, const AutoContext &context, uint8_t *record, size_t size,
					FillReport *report = nullptr);
	// drops the plans of the description's effect (by name; none: nothing happens) - the next fill rebuilds it. For a
	// description going away or changed in place; the shader cache's reload path calls it with the rebuilt
	// description, which also refreshes a plan whose description was reloaded in place.
	void forget(const EffectDesc &desc);
	// the plans kept: at most two per effect name
	uint32_t planCount() const;

	// the bytes of a declared default as fillRecord writes them (the numbers of defaultValue, tight, converted to
	// 32-bit integers for int / uint / bool): at most size bytes at out, how many written (0: none, a quoted string)
	static uint32_t writeDefault(const ParamDesc &param, uint8_t *out, uint32_t size);

  private:
	using Function = void (*)();
	using Call = void (*)(Function fn, const AutoContext &context, uint8_t *out, uint32_t size, void *user);

	struct Entry
	{
		eastl::string name;
		eastl::string hlslType; // Raw only
		AutoType	  type = AutoType::Float;
		Rate		  rate = Rate::Frame;
		TextureDim	  dim = TextureDim::None;
		uint32_t	  count = 0;	// Raw only
		Function	  fn = nullptr; // null: a free slot (taken by the next registration; plans rebuild)
		Call		  call = nullptr;
		void		 *user = nullptr;
	};
	struct Write
	{
		uint32_t offset;
		uint32_t size;
		uint32_t entry;		 // m_entries index; C_NO_ENTRY: a field for a fill's own fallback only
		int32_t	 param;		 // desc.params index, -1 for a sampler
		int32_t	 sampler;	 // desc.samplers index, -1 for a parameter
		bool	 byFallback; // nothing names the field: a fill's own fallback (AutoContext::fallback) writes it
	};
	static constexpr uint32_t C_NO_ENTRY = 0xffffffffu;
	struct Plan
	{
		eastl::string		   name;		   // the effect's: the key
		const EffectDesc	  *desc = nullptr; // the description it was built from (compared, never read after)
		uint64_t			   layoutHash = 0;
		size_t				   params = 0;
		uint32_t			   generation = 0;
		eastl::vector<uint8_t> image[C_RATES]; // the record with the defaults
		eastl::vector<Write>   writes[C_RATES];
		uint32_t			   defaulted[C_RATES] = {};
		uint32_t			   zeroed[C_RATES] = {};
	};

	template <typename T>
	static void callTyped(Function fn, const AutoContext &context, uint8_t *out, uint32_t, void *user)
	{
		T value{};
		reinterpret_cast<AutoProvider<T>>(fn)(context, value, user);
		if constexpr (sizeof(T) == 1) // bool: a 32-bit 0/1
		{
			const uint32_t v = value ? 1u : 0u;
			memcpy(out, &v, 4);
		}
		else
			memcpy(out, &value, sizeof value);
	}

	bool add(const char *name, Rate rate, AutoType type, TextureDim dim, const char *hlslType, uint32_t count,
			 Function fn, Call call, void *user);
	// the registration a parameter / sampler uses (-1: none); kind: 0 value, 1 texture, 2 sampler
	int32_t find(uint32_t kind, const char *name, bool caseless) const;
	// the registration of a record field: by its name, else by its semantic when it alone has it (see sharers)
	int32_t match(const EffectDesc &desc, const ParamDesc &param, bool &bySemantic) const;
	// the record fields of param's kind with param's semantic that are not registered by their own names
	void sharers(const EffectDesc &desc, const ParamDesc &param, eastl::vector<const ParamDesc *> &out) const;
	// the error of a registration against a declaration, empty when it fits
	eastl::string mismatch(const EffectDesc &desc, const ParamDesc &param, const Entry &entry) const;
	void		  build(const EffectDesc &desc, Plan &plan, eastl::vector<eastl::string> *errors,
						eastl::vector<eastl::string> *notes) const;
	Plan		 &slotOf(const EffectDesc &desc);				 // the description's plan, possibly stale or new
	Plan		 &planOf(const EffectDesc &desc, bool &rebuilt); // the same, rebuilt when it is stale

	eastl::vector<Entry> m_entries;
	eastl::vector<Plan>	 m_plans;
	uint32_t			 m_generation = 1;
	uint32_t			 m_lastPlan = 0;
	void				*m_lock = nullptr; // SRWLOCK, kept out of the header (windows.h)
};

} // namespace render_backend::shader
