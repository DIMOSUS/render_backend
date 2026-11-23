// Auto-constant registry (auto_constants.hpp): registrations by name, their check against an effect description,
// the per-description plans (default images and write lists), and filling a record from a plan.
#include <render_backend/shader/auto_constants.hpp>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

namespace render_backend::shader
{
namespace
{

// Scoped SRWLOCK stored in a pointer-sized member (the header does not include windows.h; SRWLOCK_INIT is zero).
// Exclusive, or shared for code that only reads the registrations.
class Lock
{
  public:
	Lock(void *const &member, bool shared)
		: m_lock(reinterpret_cast<PSRWLOCK>(const_cast<void **>(&member))), m_shared(shared)
	{
		static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK is one pointer");
		if (m_shared)
			AcquireSRWLockShared(m_lock);
		else
			AcquireSRWLockExclusive(m_lock);
	}
	~Lock()
	{
		if (m_shared)
			ReleaseSRWLockShared(m_lock);
		else
			ReleaseSRWLockExclusive(m_lock);
	}
	Lock(const Lock &) = delete;
	Lock &operator=(const Lock &) = delete;

  private:
	PSRWLOCK m_lock;
	bool	 m_shared;
};

struct TypeInfo
{
	AutoType	type;
	const char *name;
	char		scalar; // 'f' float, 'i' int, 'u' uint, 'b' bool, 0 not numeric
	uint8_t		rows, columns;
};
const TypeInfo C_TYPES[] = {
	{AutoType::Float, "float", 'f', 1, 1},
	{AutoType::Float2, "float2", 'f', 1, 2},
	{AutoType::Float3, "float3", 'f', 1, 3},
	{AutoType::Float4, "float4", 'f', 1, 4},
	{AutoType::Float3x2, "float3x2", 'f', 3, 2},
	{AutoType::Float3x3, "float3x3", 'f', 3, 3},
	{AutoType::Float3x4, "float3x4", 'f', 3, 4},
	{AutoType::Float4x3, "float4x3", 'f', 4, 3},
	{AutoType::Float4x4, "float4x4", 'f', 4, 4},
	{AutoType::Int, "int", 'i', 1, 1},
	{AutoType::Int2, "int2", 'i', 1, 2},
	{AutoType::Int3, "int3", 'i', 1, 3},
	{AutoType::Int4, "int4", 'i', 1, 4},
	{AutoType::UInt, "uint", 'u', 1, 1},
	{AutoType::UInt2, "uint2", 'u', 1, 2},
	{AutoType::UInt3, "uint3", 'u', 1, 3},
	{AutoType::UInt4, "uint4", 'u', 1, 4},
	{AutoType::Bool, "bool", 'b', 1, 1},
	{AutoType::Texture, "texture", 0, 1, 1},
	{AutoType::Sampler, "sampler", 0, 1, 1},
	{AutoType::Raw, "raw", 0, 1, 1},
};

const TypeInfo &infoOf(AutoType type)
{
	for (const TypeInfo &t : C_TYPES)
		if (t.type == type)
			return t;
	return C_TYPES[0];
}

// the scalar of an HLSL type and what follows it ("float4x3" -> 'f', "4x3"); 0 if it is not numeric
char scalarOf(const char *type, const char *&rest)
{
	struct Prefix
	{
		const char *name;
		char		scalar;
	};
	static const Prefix prefixes[] = {{"float", 'f'}, {"half", 'f'},  {"int", 'i'},
									  {"uint", 'u'},  {"dword", 'u'}, {"bool", 'b'}};
	for (const Prefix &p : prefixes)
	{
		const size_t n = strlen(p.name);
		if (strncmp(type, p.name, n) == 0 && (type[n] == 0 || (type[n] >= '1' && type[n] <= '4')))
		{
			rest = type + n;
			return p.scalar;
		}
	}
	return 0;
}

const char *dimName(TextureDim dim)
{
	switch (dim)
	{
	case TextureDim::Tex2D:
		return "2D";
	case TextureDim::Cube:
		return "cube";
	case TextureDim::Tex3D:
		return "3D";
	default:
		return "any";
	}
}

void appendf(eastl::string &out, const char *format, ...)
{
	char	buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	out += buffer;
}

// where a declaration is: its file:line, else the effect's source
const char *whereOf(const EffectDesc &desc, const ParamDesc &param)
{
	return param.where.empty() ? desc.source.c_str() : param.where.c_str();
}

// "g_mWorld (WORLD)" or "g_mWorld"
eastl::string labelOf(const ParamDesc &param)
{
	return param.semantic.empty() ? param.name : param.name + " (" + param.semantic + ")";
}

// 0 value, 1 texture, 2 sampler: the three name spaces
uint32_t kindOf(AutoType type)
{
	return type == AutoType::Texture ? 1 : type == AutoType::Sampler ? 2 : 0;
}

bool inRecord(const EffectDesc &desc, const ParamDesc &param)
{
	const uint32_t r = static_cast<uint32_t>(param.rate);
	return (param.use & C_USE_RECORD) != 0 && (param.use & C_USE_DERIVED) == 0 && param.offset >= 0 && param.size &&
		   static_cast<uint32_t>(param.offset) + param.size <= desc.recordSize[r];
}

void callIndex(void (*fn)(), const AutoContext &context, uint8_t *out, uint32_t, void *user)
{
	uint32_t index = 0;
	reinterpret_cast<AutoIndexProvider>(fn)(context, index, user);
	memcpy(out, &index, 4);
}

void callRaw(void (*fn)(), const AutoContext &context, uint8_t *out, uint32_t size, void *user)
{
	reinterpret_cast<AutoRawProvider>(fn)(context, out, size, user);
}
} // namespace

const char *autoTypeName(AutoType type)
{
	return infoOf(type).name;
}

uint32_t autoTypeSize(AutoType type)
{
	const TypeInfo &t = infoOf(type);
	return 4u * t.rows * t.columns;
}

bool autoTypeOf(const eastl::string &hlslType, AutoType &out)
{
	if (hlslType.compare(0, 7, "Texture") == 0)
	{
		out = AutoType::Texture;
		return true;
	}
	const char *rest = nullptr;
	const char	scalar = scalarOf(hlslType.c_str(), rest);
	if (!scalar)
		return false;
	uint32_t rows = 1, columns = 1;
	if (*rest)
	{
		columns = static_cast<uint32_t>(*rest++ - '0');
		if (*rest == 'x' && rest[1] >= '1' && rest[1] <= '4' && rest[2] == 0)
		{
			rows = columns;
			columns = static_cast<uint32_t>(rest[1] - '0');
		}
		else if (*rest)
			return false;
	}
	for (const TypeInfo &t : C_TYPES)
	{
		if (t.scalar == scalar && t.rows == rows && t.columns == columns)
		{
			out = t.type;
			return true;
		}
	}
	return false;
}

uint32_t AutoConstants::writeDefault(const ParamDesc &param, uint8_t *out, uint32_t size)
{
	if (param.defaultValue.empty() || param.defaultValue[0] == '"')
		return 0;
	const char *rest = nullptr;
	const char	scalar = scalarOf(param.type.c_str(), rest);
	const bool	integer = scalar == 'i' || scalar == 'u' || scalar == 'b';
	uint32_t	written = 0;
	const char *c = param.defaultValue.c_str();
	while (*c != 0 && written + 4 <= size)
	{
		char		*end = nullptr;
		const double v = strtod(c, &end);
		if (end == c)
			break;
		if (integer)
		{
			const uint32_t i = static_cast<uint32_t>(static_cast<int64_t>(v));
			memcpy(out + written, &i, 4);
		}
		else
		{
			const float f = static_cast<float>(v);
			memcpy(out + written, &f, 4);
		}
		written += 4;
		c = *end == ',' ? end + 1 : end;
	}
	return written;
}

// ------------------------------------------------------------------------------------------------- registration

bool AutoConstants::add(const char *name, Rate rate, AutoType type, TextureDim dim, const char *hlslType,
						uint32_t count, Function fn, Call call, void *user)
{
	Lock lock(m_lock, false);
	if (!name || !*name || !fn || find(kindOf(type), name, true) >= 0)
		return false;
	Entry *slot = nullptr;
	for (Entry &e : m_entries)
		if (!e.fn)
		{
			slot = &e;
			break;
		}
	if (!slot)
	{
		m_entries.push_back();
		slot = &m_entries.back();
	}
	slot->name = name;
	slot->hlslType = hlslType ? hlslType : "";
	slot->type = type;
	slot->rate = rate;
	slot->dim = dim;
	slot->count = count;
	slot->fn = fn;
	slot->call = call;
	slot->user = user;
	++m_generation;
	return true;
}

bool AutoConstants::registerTexture(const char *name, Rate rate, AutoIndexProvider provider, void *user, TextureDim dim)
{
	return add(name, rate, AutoType::Texture, dim, nullptr, 0, reinterpret_cast<Function>(provider), &callIndex, user);
}

bool AutoConstants::registerSampler(const char *name, AutoIndexProvider provider, void *user)
{
	return add(name, Rate::Material, AutoType::Sampler, TextureDim::None, nullptr, 0,
			   reinterpret_cast<Function>(provider), &callIndex, user);
}

bool AutoConstants::registerRaw(const char *name, Rate rate, const char *hlslType, uint32_t count,
								AutoRawProvider provider, void *user)
{
	if (!hlslType || !*hlslType)
		return false;
	return add(name, rate, AutoType::Raw, TextureDim::None, hlslType, count, reinterpret_cast<Function>(provider),
			   &callRaw, user);
}

namespace
{
// Fallback registration names. No HLSL name or semantic starts with '*', so no lookup by name finds them.
const char *const C_FALLBACK_NAMES[C_RATES] = {"*fallback frame", "*fallback instance", "*fallback material"};
} // namespace

bool AutoConstants::registerFallback(Rate rate, AutoRawProvider provider, void *user)
{
	return add(C_FALLBACK_NAMES[static_cast<uint32_t>(rate)], rate, AutoType::Raw, TextureDim::None, "*", 0,
			   reinterpret_cast<Function>(provider), &callRaw, user);
}

bool AutoConstants::unregisterFallback(Rate rate)
{
	return unregisterConstant(C_FALLBACK_NAMES[static_cast<uint32_t>(rate)]);
}

bool AutoConstants::unregisterConstant(const char *name)
{
	Lock lock(m_lock, false);
	bool found = false;
	for (Entry &e : m_entries)
	{
		if (e.fn && _stricmp(e.name.c_str(), name) == 0)
		{
			e.fn = nullptr;
			found = true;
		}
	}
	m_generation += found;
	return found;
}

uint32_t AutoConstants::unregisterUser(const void *user)
{
	Lock	 lock(m_lock, false);
	uint32_t n = 0;
	for (Entry &e : m_entries)
	{
		if (e.fn && e.user == user)
		{
			e.fn = nullptr;
			++n;
		}
	}
	m_generation += n != 0;
	return n;
}

uint32_t AutoConstants::count() const
{
	Lock	 lock(m_lock, true);
	uint32_t n = 0;
	for (const Entry &e : m_entries)
		n += e.fn != nullptr;
	return n;
}

// ------------------------------------------------------------------------------------------------- matching

int32_t AutoConstants::find(uint32_t kind, const char *name, bool caseless) const
{
	for (size_t i = 0; i < m_entries.size(); ++i)
	{
		const Entry &e = m_entries[i];
		if (e.fn && kindOf(e.type) == kind &&
			(caseless ? _stricmp(e.name.c_str(), name) : strcmp(e.name.c_str(), name)) == 0)
			return static_cast<int32_t>(i);
	}
	return -1;
}

void AutoConstants::sharers(const EffectDesc &desc, const ParamDesc &param, eastl::vector<const ParamDesc *> &out) const
{
	out.clear();
	const uint32_t kind = (param.use & C_USE_TEXTURE) ? 1 : 0;
	for (const ParamDesc &q : desc.params)
		if (inRecord(desc, q) && (q.use & C_USE_TEXTURE) == (param.use & C_USE_TEXTURE) &&
			_stricmp(q.semantic.c_str(), param.semantic.c_str()) == 0 && find(kind, q.name.c_str(), false) < 0)
			out.push_back(&q);
}

int32_t AutoConstants::match(const EffectDesc &desc, const ParamDesc &param, bool &bySemantic) const
{
	const uint32_t kind = (param.use & C_USE_TEXTURE) ? 1 : 0;
	bySemantic = false;
	const int32_t byName = find(kind, param.name.c_str(), false);
	if (byName >= 0 || param.semantic.empty())
		return byName;
	const int32_t i = find(kind, param.semantic.c_str(), true);
	if (i < 0)
		return -1;
	// the semantic decides only when one record parameter not registered by name has it
	eastl::vector<const ParamDesc *> shared;
	sharers(desc, param, shared);
	if (shared.size() != 1)
		return -1;
	bySemantic = true;
	return i;
}

eastl::string AutoConstants::mismatch(const EffectDesc &desc, const ParamDesc &param, const Entry &entry) const
{
	eastl::string		error;
	const char		   *where = whereOf(desc, param);
	const char		   *key = entry.name.c_str();
	const eastl::string label = labelOf(param);
	if (param.rate != entry.rate)
	{
		appendf(error, "%s: %s: declared per %s, the auto-constant %s is per %s", where, label.c_str(),
				rateName(param.rate), key, rateName(entry.rate));
		return error;
	}
	if (entry.type == AutoType::Texture)
	{
		if (param.count)
			appendf(error, "%s: %s: declared %s[%u], the auto-constant %s is one texture", where, label.c_str(),
					param.type.c_str(), param.count, key);
		else if (entry.dim != TextureDim::None && entry.dim != param.dim)
			appendf(error, "%s: %s: declared %s, the auto-constant %s is a %s texture", where, label.c_str(),
					param.type.c_str(), key, dimName(entry.dim));
		return error;
	}
	if (entry.type == AutoType::Raw)
	{
		if (param.type != entry.hlslType || param.count != entry.count)
		{
			eastl::string declared = param.type, registered = entry.hlslType;
			if (param.count)
				appendf(declared, "[%u]", param.count);
			if (entry.count)
				appendf(registered, "[%u]", entry.count);
			appendf(error, "%s: %s: declared %s, the auto-constant %s is %s", where, label.c_str(), declared.c_str(),
					key, registered.c_str());
		}
		return error;
	}
	AutoType declared;
	if (param.count || !autoTypeOf(param.type, declared) || declared != entry.type)
	{
		eastl::string type = param.type;
		if (param.count)
			appendf(type, "[%u]", param.count);
		appendf(error, "%s: %s: declared %s, the auto-constant %s is %s", where, label.c_str(), type.c_str(), key,
				autoTypeName(entry.type));
		return error;
	}
	if (param.size != autoTypeSize(entry.type))
		appendf(error, "%s: %s: its record field is %u bytes, the auto-constant %s writes %u", where, label.c_str(),
				param.size, key, autoTypeSize(entry.type));
	return error;
}

// ------------------------------------------------------------------------------------------------- plans

void AutoConstants::build(const EffectDesc &desc, Plan &plan, eastl::vector<eastl::string> *errors,
						  eastl::vector<eastl::string> *notes) const
{
	plan.desc = &desc;
	plan.layoutHash = desc.layoutHash;
	plan.params = desc.params.size();
	plan.generation = m_generation;
	for (uint32_t r = 0; r < C_RATES; ++r)
	{
		plan.image[r].assign(desc.recordSize[r], 0);
		plan.writes[r].clear();
		plan.defaulted[r] = 0;
		plan.zeroed[r] = 0;
	}
	int32_t fallback[C_RATES];
	for (uint32_t r = 0; r < C_RATES; ++r)
		fallback[r] = find(0, C_FALLBACK_NAMES[r], false);
	for (size_t i = 0; i < desc.params.size(); ++i)
	{
		const ParamDesc &p = desc.params[i];
		if (!p.alias.empty())
		{
			// An alias of a prelude parameter has no field of its own; the prelude parameter's registration fills
			// it. A registration by the alias's own name would never be used.
			const int32_t e = find((p.use & C_USE_TEXTURE) ? 1 : 0, p.name.c_str(), false);
			if (e >= 0 && errors)
			{
				eastl::string error;
				appendf(error,
						"%s: %s is another name of the standard %s: the auto-constant %s is never used, "
						"register %s",
						whereOf(desc, p), labelOf(p).c_str(), p.alias.c_str(), m_entries[e].name.c_str(),
						p.semantic.c_str());
				errors->push_back(error);
			}
			continue;
		}
		const uint32_t kind = (p.use & C_USE_TEXTURE) ? 1 : 0;
		if (p.use & C_USE_DERIVED)
		{
			// Computed by the shader from the renderer's headers: nothing to write, and a registration is not used.
			int32_t e = find(kind, p.name.c_str(), false);
			if (e < 0 && !p.semantic.empty())
				e = find(kind, p.semantic.c_str(), true);
			if (e >= 0 && notes)
			{
				eastl::string note;
				appendf(note,
						"%s: %s is computed by the shader from the renderer's headers: the auto-constant %s is "
						"not used",
						whereOf(desc, p), labelOf(p).c_str(), m_entries[e].name.c_str());
				notes->push_back(note);
			}
			continue;
		}
		if (!inRecord(desc, p))
			continue;
		const uint32_t r = static_cast<uint32_t>(p.rate);
		const uint32_t defaulted =
			(p.use & C_USE_TEXTURE) ? 0 : writeDefault(p, plan.image[r].data() + p.offset, p.size);
		bool		  bySemantic = false;
		const int32_t e = match(desc, p, bySemantic);
		bool		  used = false;
		if (e >= 0)
		{
			const eastl::string error = mismatch(desc, p, m_entries[e]);
			if (error.empty())
			{
				plan.writes[r].push_back(Write{static_cast<uint32_t>(p.offset), p.size, static_cast<uint32_t>(e),
											   static_cast<int32_t>(i), -1, false});
				used = true;
			}
			else if (errors)
				errors->push_back(error);
		}
		else
		{
			// No registration names it: the fill's own fallback, else the rate's registered fallback, writes it by
			// the parameter's name. With neither it keeps its default.
			plan.writes[r].push_back(Write{static_cast<uint32_t>(p.offset), p.size,
										   fallback[r] >= 0 ? static_cast<uint32_t>(fallback[r]) : C_NO_ENTRY,
										   static_cast<int32_t>(i), -1, true});
			used = fallback[r] >= 0;
		}
		if (e < 0 && fallback[r] < 0 && notes && !p.semantic.empty())
		{
			eastl::string note;
			const int32_t registered = find(kind, p.semantic.c_str(), true);
			const char	 *keeps = defaulted ? "it gets its default" : "it stays zero";
			if (registered >= 0)
			{
				// Several record parameters share this semantic and none is registered by name. It is unknown which
				// one is meant, so none gets the auto-constant.
				eastl::vector<const ParamDesc *> shared;
				sharers(desc, p, shared);
				eastl::string names;
				for (const ParamDesc *q : shared)
					names += (names.empty() ? "" : ", ") + q->name;
				appendf(note,
						"%s: %s: the auto-constant %s is the semantic of %u parameters (%s): register each by "
						"name; %s",
						whereOf(desc, p), labelOf(p).c_str(), m_entries[registered].name.c_str(),
						static_cast<unsigned>(shared.size()), names.c_str(), keeps);
			}
			else
				appendf(note, "%s: no auto-constant %s (%s): %s", whereOf(desc, p), p.semantic.c_str(), p.name.c_str(),
						keeps);
			notes->push_back(note);
		}
		if (!used)
			++(defaulted ? plan.defaulted[r] : plan.zeroed[r]);
	}
	const uint32_t m = static_cast<uint32_t>(Rate::Material);
	for (size_t i = 0; i < desc.samplers.size(); ++i)
	{
		const SamplerDesc &s = desc.samplers[i];
		if (s.offset < 0 || static_cast<uint32_t>(s.offset) + 4 > desc.recordSize[m])
			continue;
		const int32_t e = find(2, s.name.c_str(), false);
		if (e >= 0)
			plan.writes[m].push_back(Write{static_cast<uint32_t>(s.offset), 4, static_cast<uint32_t>(e), -1,
										   static_cast<int32_t>(i), false});
		else
		{
			plan.writes[m].push_back(Write{static_cast<uint32_t>(s.offset), 4,
										   fallback[m] >= 0 ? static_cast<uint32_t>(fallback[m]) : C_NO_ENTRY, -1,
										   static_cast<int32_t>(i), true});
			if (fallback[m] < 0)
				++plan.zeroed[m];
		}
	}
}

AutoConstants::Plan &AutoConstants::slotOf(const EffectDesc &desc)
{
	// At most two plans per effect name, keyed by address. Two live descriptions of one effect (an effect and its
	// alias loaded separately) each keep theirs; a third description (reloaded into another object) takes the slot
	// the last fill did not use. So there are never more than twice as many plans as effects.
	constexpr uint32_t C_PLANS_PER_EFFECT = 2;
	size_t			   at = m_lastPlan;
	// Compare name as well as address: another effect's description may occupy the memory of a freed one.
	if (at >= m_plans.size() || m_plans[at].desc != &desc || m_plans[at].name != desc.name)
	{
		size_t	 spare = m_plans.size();
		uint32_t named = 0;
		for (at = 0; at < m_plans.size(); ++at)
		{
			if (m_plans[at].name != desc.name)
				continue;
			if (m_plans[at].desc == &desc)
				break;
			++named;
			if (spare == m_plans.size() || at != m_lastPlan)
				spare = at;
		}
		if (at == m_plans.size())
		{
			if (named >= C_PLANS_PER_EFFECT)
				at = spare;
			else
			{
				m_plans.push_back();
				m_plans.back().name = desc.name;
			}
		}
		m_lastPlan = static_cast<uint32_t>(at);
	}
	return m_plans[at];
}

uint32_t AutoConstants::planCount() const
{
	Lock lock(m_lock, true);
	return static_cast<uint32_t>(m_plans.size());
}

AutoConstants::Plan &AutoConstants::planOf(const EffectDesc &desc, bool &rebuilt)
{
	Plan &plan = slotOf(desc);
	rebuilt = plan.desc != &desc || plan.generation != m_generation || plan.layoutHash != desc.layoutHash ||
			  plan.params != desc.params.size();
	if (rebuilt)
		build(desc, plan, nullptr, nullptr);
	return plan;
}

bool AutoConstants::check(const EffectDesc &desc, eastl::vector<eastl::string> &errors,
						  eastl::vector<eastl::string> *notes)
{
	// Builds a local plan that is dropped on return: the description may be a temporary on the building thread.
	Lock		 lock(m_lock, true);
	Plan		 plan;
	const size_t before = errors.size();
	build(desc, plan, &errors, notes);
	return errors.size() == before;
}

void AutoConstants::forget(const EffectDesc &desc)
{
	Lock lock(m_lock, false);
	for (size_t i = m_plans.size(); i-- > 0;)
	{
		if (m_plans[i].name == desc.name)
		{
			m_plans.erase(m_plans.begin() + i);
			m_lastPlan = 0;
		}
	}
}

// ------------------------------------------------------------------------------------------------- filling

bool AutoConstants::fillRecord(const EffectDesc &desc, Rate rate, const AutoContext &context, uint8_t *record,
							   size_t size, FillReport *report)
{
	const uint32_t r = static_cast<uint32_t>(rate);
	const uint32_t recordSize = desc.recordSize[r];
	if (!recordSize || !record || size < recordSize)
		return false;
	Lock		   lock(m_lock, false); // the plans change; the providers run under it
	bool		   rebuilt = false;
	const Plan	  &plan = planOf(desc, rebuilt);
	const uint32_t start = rate == Rate::Material ? desc.materialHeaderSize : 0;
	if (recordSize > start)
		memcpy(record + start, plan.image[r].data() + start, recordSize - start);
	AutoContext local = context;
	local.effect = &desc;
	uint32_t provided = 0;
	for (const Write &w : plan.writes[r])
	{
		if (w.byFallback && context.fallback == nullptr && w.entry == C_NO_ENTRY)
			continue; // no fallback at all: the default stays
		local.param = w.param >= 0 ? &desc.params[w.param] : nullptr;
		local.sampler = w.sampler >= 0 ? &desc.samplers[w.sampler] : nullptr;
		if (w.byFallback && context.fallback != nullptr)
			context.fallback(local, record + w.offset, w.size, context.fallbackUser);
		else
		{
			const Entry &e = m_entries[w.entry];
			e.call(e.fn, local, record + w.offset, w.size, e.user);
		}
		++provided;
	}
	if (report)
	{
		report->provided = provided;
		report->defaulted = plan.defaulted[r];
		report->zeroed = plan.zeroed[r];
		report->rebuilt = rebuilt;
	}
	return true;
}

} // namespace render_backend::shader
