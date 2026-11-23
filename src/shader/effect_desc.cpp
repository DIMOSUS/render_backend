// Effect description (effect_desc.hpp): the .desc text format, the state names and enums of the effect language,
// and the evaluation of state expressions for selection values.
//
// .desc format: one record per line, words separated by blanks. A double-quoted word, or a quoted value after
// "key=", may contain blanks. State expressions, annotation values, notes, the source path and a param's default
// take the rest of their line.
#include <render_backend/shader/effect_desc.hpp>
#include <render_backend/shader/vocabulary.hpp>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace render_backend::shader
{
SamplerSettings resolveSamplerSettings(const EffectDesc &desc, const SamplerDesc &sampler, const void *context,
									   SamplerParameterLookup lookup)
{
	enum class Slot
	{
		MinFilter,
		MagFilter,
		MipFilter,
		AddressU,
		AddressV,
		Anisotropy,
	};
	struct SamplerStateSlot
	{
		const char *name;
		Slot		slot;
	};
	constexpr SamplerStateSlot C_SLOTS[] = {
		{"MinFilter", Slot::MinFilter}, {"MagFilter", Slot::MagFilter}, {"MipFilter", Slot::MipFilter},
		{"AddressU", Slot::AddressU},	{"AddressV", Slot::AddressV},	{"MaxAnisotropy", Slot::Anisotropy},
	};
	const auto filter = [](uint32_t code)
	{ return code <= uint32_t(SamplerFilter::BaseMip) ? SamplerFilter(code) : SamplerFilter::Linear; };
	const auto address = [](uint32_t code)
	{ return code <= uint32_t(SamplerAddress::MirrorOnce) ? SamplerAddress(code) : SamplerAddress::Wrap; };

	SamplerSettings settings;
	uint32_t		requestedAnisotropy = 1;
	// A state's last authored value wins, whether it is a literal or a material parameter reference.
	for (const StateValue &state : sampler.states)
		for (const SamplerStateSlot &field : C_SLOTS)
		{
			if (state.state != field.name)
				continue;
			eastl::string text, error;
			if (!desc.evaluate(state.state.c_str(), state.value, SelectionValues(), text, error))
				break;
			int32_t value;
			if (const ParamDesc *param = desc.findParam(text.c_str()))
			{
				if (lookup == nullptr || !lookup(context, *param, value))
					value = int32_t(atoi(param->defaultValue.c_str()));
			}
			else
				value = int32_t(atof(text.c_str()));
			switch (field.slot)
			{
			case Slot::MinFilter:
				settings.minFilter = filter(uint32_t(value));
				break;
			case Slot::MagFilter:
				settings.magFilter = filter(uint32_t(value));
				break;
			case Slot::MipFilter:
				settings.mipFilter = filter(uint32_t(value));
				break;
			case Slot::AddressU:
				settings.addressU = address(uint32_t(value));
				break;
			case Slot::AddressV:
				settings.addressV = address(uint32_t(value));
				break;
			case Slot::Anisotropy:
				requestedAnisotropy = uint32_t(value);
				break;
			}
			break;
		}
	if (settings.minFilter == SamplerFilter::Anisotropic && requestedAnisotropy > 1)
		settings.anisotropy = uint8_t(requestedAnisotropy > 16 ? 16 : requestedAnisotropy);
	return settings;
}

namespace
{
constexpr const char *C_DESC_HEADER = "render-desc 3";
constexpr uint32_t	  C_LEGACY_MATERIAL_HEADER_SIZE = 32;

struct EnumName
{
	const char *name;
	int32_t		value;
};
const EnumName C_CULL[] = {{"NONE", 0}, {"FRONT", 1}, {"BACK", 2}, {nullptr, 0}};
const EnumName C_COMPARE[] = {{"NEVER", 0},		{"LESS", 1},		  {"EQUAL", 2},	 {"LESS_EQUAL", 3}, {"GREATER", 4},
							  {"NOT_EQUAL", 5}, {"GREATER_EQUAL", 6}, {"ALWAYS", 7}, {nullptr, 0}};
const EnumName C_BLEND_MODE[] = {{"DUAL_SOURCE", 0}, {"NONE", 1}, {nullptr, 0}};
const EnumName C_MASK[] = {{"NONE", 0}, {"RGB", 7}, {"RGBA", 15}, {"ALL", 15}, {nullptr, 0}};
// Effect language values, including the compiled shader's blend record encoding.
const EnumName C_BLEND[] = {
	{"ZERO", int32_t(BlendFactor::Zero)},
	{"ONE", int32_t(BlendFactor::One)},
	{"SRC_COLOR", int32_t(BlendFactor::SrcColor)},
	{"INV_SRC_COLOR", int32_t(BlendFactor::InvSrcColor)},
	{"SRC_ALPHA", int32_t(BlendFactor::SrcAlpha)},
	{"INV_SRC_ALPHA", int32_t(BlendFactor::InvSrcAlpha)},
	{"DEST_ALPHA", int32_t(BlendFactor::DestAlpha)},
	{"INV_DEST_ALPHA", int32_t(BlendFactor::InvDestAlpha)},
	{"DEST_COLOR", int32_t(BlendFactor::DestColor)},
	{"INV_DEST_COLOR", int32_t(BlendFactor::InvDestColor)},
	{"SRC_ALPHA_SAT", int32_t(BlendFactor::SrcAlphaSat)},
	{nullptr, 0},
};
const EnumName C_FILTER[] = {{"NONE", int32_t(SamplerFilter::BaseMip)},
							 {"POINT", int32_t(SamplerFilter::Point)},
							 {"LINEAR", int32_t(SamplerFilter::Linear)},
							 {"ANISOTROPIC", int32_t(SamplerFilter::Anisotropic)},
							 {nullptr, 0}};
const EnumName C_ADDRESS[] = {{"WRAP", int32_t(SamplerAddress::Wrap)},
							  {"MIRROR", int32_t(SamplerAddress::Mirror)},
							  {"CLAMP", int32_t(SamplerAddress::Clamp)},
							  {"BORDER", int32_t(SamplerAddress::Border)},
							  {"MIRROR_ONCE", int32_t(SamplerAddress::MirrorOnce)},
							  {nullptr, 0}};
const EnumName C_TOPOLOGY[] = {{"TRIANGLES", 0},  {"TRIANGLE_STRIP", 1}, {"LINES", 2},
							   {"LINE_STRIP", 3}, {"POINTS", 4},		 {nullptr, 0}};
const EnumName C_BOOL[] = {{"false", 0}, {"true", 1}, {nullptr, 0}};

struct StateName
{
	const char	   *name;
	int32_t			kind; // stateKind()
	const EnumName *names;
};
const StateName C_STATES[] = {
	{"CullMode", 1, C_CULL},
	{"DepthTest", 1, C_BOOL},
	{"DepthWrite", 1, C_BOOL},
	{"DepthFunc", 1, C_COMPARE},
	{"ColorWriteMask", 1, C_MASK},
	{"DepthBias", 1, nullptr},
	{"SlopeScaledDepthBias", 1, nullptr},
	{"Blend", 1, C_BLEND_MODE},
	{"AlphaTest", 1, C_BOOL},
	{"Meshlets", 1, C_BOOL},
	{"PoolVertices", 1, C_BOOL},
	{"Vertices", 1, C_BOOL},
	{"Topology", 1, C_TOPOLOGY},
	{"Precache", 1, C_BOOL},
	{"Screen", 1, C_BOOL},
	{"Targets", 4, nullptr},
	{"BlendEnable", 2, C_BOOL},
	{"SrcBlend", 2, C_BLEND},
	{"DestBlend", 2, C_BLEND},
	{"SeparateAlphaBlend", 2, C_BOOL},
	{"SrcBlendAlpha", 2, C_BLEND},
	{"DestBlendAlpha", 2, C_BLEND},
	{"AlphaRef", 2, nullptr},
	{"BackFaceCull", 2, C_BOOL},
	{"MinFilter", 3, C_FILTER},
	{"MagFilter", 3, C_FILTER},
	{"MipFilter", 3, C_FILTER},
	{"AddressU", 3, C_ADDRESS},
	{"AddressV", 3, C_ADDRESS},
	{"AddressW", 3, C_ADDRESS},
	{"MaxAnisotropy", 3, nullptr},
	{"MipLODBias", 3, nullptr},
	{"MaxMipLevel", 3, nullptr},
};

const StateName *findState(const char *state)
{
	for (const StateName &s : C_STATES)
		if (strcmp(s.name, state) == 0)
			return &s;
	return nullptr;
}

// The end of the word that starts at p. A '"' at the start of the word or right after '=' opens a quoted part that
// runs to the closing '"' and may contain blanks.
const char *wordEnd(const char *p, const char *end)
{
	const char *start = p;
	while (p < end && *p != ' ' && *p != '\t' && *p != '\r')
	{
		if (*p == '"' && (p == start || p[-1] == '='))
		{
			++p;
			while (p < end && *p != '"')
				++p;
		}
		if (p < end)
			++p;
	}
	return p;
}

// The words of one line.
void splitWords(const char *begin, const char *end, eastl::vector<eastl::string> &words)
{
	words.clear();
	const char *p = begin;
	while (p < end)
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
			++p;
		if (p >= end)
			break;
		const char *start = p;
		p = wordEnd(p, end);
		words.push_back(eastl::string(start, p));
	}
}

// A value written as key=value, quoted when it contains blanks.
eastl::string quotedIfBlank(const eastl::string &value)
{
	return value.find_first_of(" \t") == eastl::string::npos ? value : "\"" + value + "\"";
}

// The inverse of quotedIfBlank.
eastl::string unquoted(const eastl::string &value)
{
	return value.size() >= 2 && value.front() == '"' && value.back() == '"' ? value.substr(1, value.size() - 2) : value;
}

// "key=value" -> key, value; false when there is no '='
bool splitPair(const eastl::string &word, eastl::string &key, eastl::string &value)
{
	const size_t eq = word.find('=');
	if (eq == eastl::string::npos)
		return false;
	key.assign(word.begin(), word.begin() + eq);
	value.assign(word.begin() + eq + 1, word.end());
	return true;
}

const char *dimName(TextureDim dim)
{
	switch (dim)
	{
	case TextureDim::Tex2D:
		return "2d";
	case TextureDim::Cube:
		return "cube";
	case TextureDim::Tex3D:
		return "3d";
	default:
		return "none";
	}
}

bool parseDim(const eastl::string &text, TextureDim &dim)
{
	if (text == "2d")
		dim = TextureDim::Tex2D;
	else if (text == "cube")
		dim = TextureDim::Cube;
	else if (text == "3d")
		dim = TextureDim::Tex3D;
	else if (text == "none")
		dim = TextureDim::None;
	else
		return false;
	return true;
}

const char *C_STAGE_KEYS[C_STAGES] = {"vs", "ps", "ms", "as", "cs"};

void appendf(eastl::string &out, const char *format, ...)
{
	char	buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	out += buffer;
}

void writeAnnotations(eastl::string &out, const eastl::vector<Annotation> &annotations)
{
	for (const Annotation &a : annotations)
		out += "annot " + a.name + " " + a.type + " " + a.value + "\n";
}

// The rest of a line after its first n words (splitWords' words), trimmed.
eastl::string restAfter(const char *begin, const char *end, int words)
{
	const char *p = begin;
	for (int i = 0; i < words; ++i)
	{
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;
		p = wordEnd(p, end);
	}
	while (p < end && (*p == ' ' || *p == '\t'))
		++p;
	const char *e = end;
	while (e > p && (e[-1] == '\r' || e[-1] == ' '))
		--e;
	return eastl::string(p, e);
}

// --------------------------------------------------------------------------------------------------- expressions
// value := or ['?' value ':' value]; or := and {'||' and}; and := compare {'&&' compare};
// compare := unary [('==' | '!=') unary]; unary := ['!'] primary; primary := '(' value ')' | number | identifier
struct Value
{
	double		  number = 0;
	eastl::string name;			 // the value's enum name (a named selection value, an enum name), if any
	bool		  param = false; // a material parameter: name is its name, number means nothing
};

class Evaluator
{
  public:
	Evaluator(const EffectDesc &desc, const char *state, const SelectionValues &values, eastl::string &error)
		: m_desc(desc), m_state(findState(state)), m_values(values), m_error(error)
	{
	}

	bool run(const eastl::string &expression, Value &out)
	{
		splitWords(expression.c_str(), expression.c_str() + expression.size(), m_tokens);
		m_at = 0;
		if (!value(out))
			return false;
		if (m_at != m_tokens.size())
			return fail("unexpected '" + m_tokens[m_at] + "'");
		return true;
	}

  private:
	bool fail(const eastl::string &what)
	{
		if (m_error.empty())
			m_error = what;
		return false;
	}
	const eastl::string &peek() const
	{
		static const eastl::string none;
		return m_at < m_tokens.size() ? m_tokens[m_at] : none;
	}
	bool value(Value &out)
	{
		if (!condition(out))
			return false;
		if (peek() != "?")
			return true;
		++m_at;
		Value a, b;
		if (!value(a))
			return false;
		if (peek() != ":")
			return fail("':' expected");
		++m_at;
		if (!value(b))
			return false;
		if (out.param)
			return fail("a parameter is not a condition");
		out = out.number != 0 ? a : b;
		return true;
	}
	// condition := and ('||' and)*; and := compare ('&&' compare)*: && binds tighter, as in HLSL
	bool condition(Value &out)
	{
		if (!conjunction(out))
			return false;
		while (peek() == "||")
		{
			++m_at;
			Value rhs;
			if (!conjunction(rhs))
				return false;
			const bool either = out.number != 0 || rhs.number != 0;
			out = Value();
			out.number = either ? 1 : 0;
		}
		return true;
	}
	bool conjunction(Value &out)
	{
		if (!comparison(out))
			return false;
		while (peek() == "&&")
		{
			++m_at;
			Value rhs;
			if (!comparison(rhs))
				return false;
			const bool both = out.number != 0 && rhs.number != 0;
			out = Value();
			out.number = both ? 1 : 0;
		}
		return true;
	}
	bool comparison(Value &out)
	{
		if (!unary(out))
			return false;
		const eastl::string op = peek();
		if (op == "==" || op == "!=")
		{
			++m_at;
			Value rhs;
			if (!unary(rhs))
				return false;
			if (out.param || rhs.param)
				return fail("a parameter in a comparison");
			const bool same = !out.name.empty() && !rhs.name.empty() ? out.name == rhs.name : out.number == rhs.number;
			out = Value();
			out.number = (op == "==") == same ? 1 : 0;
		}
		return true;
	}
	bool unary(Value &out)
	{
		if (peek() == "!")
		{
			++m_at;
			if (!primary(out))
				return false;
			const double n = out.number;
			out = Value();
			out.number = n == 0 ? 1 : 0;
			return true;
		}
		return primary(out);
	}
	bool primary(Value &out)
	{
		if (m_at >= m_tokens.size())
			return fail("a value expected");
		const eastl::string token = m_tokens[m_at++];
		out = Value();
		if (token == "(")
		{
			if (!value(out))
				return false;
			if (peek() != ")")
				return fail("')' expected");
			++m_at;
			return true;
		}
		const char c = token[0];
		if ((c >= '0' && c <= '9') || c == '-' || c == '.')
		{
			char *end = nullptr;
			out.number = strtod(token.c_str(), &end);
			if (end != token.c_str() + token.size() && !(*end == 'f' && end[1] == 0))
				return fail("not a number: " + token);
			return true;
		}
		// a selection: its value (and the value's name)
		const int32_t s = m_desc.findSelection(token.c_str());
		if (s >= 0)
		{
			const SelectionDesc &sel = m_desc.selections[s];
			const int32_t		 v = static_cast<size_t>(s) < m_values.size() ? m_values[s] : sel.defaultValue;
			out.number = v;
			for (const SelectionDesc::Value &named : sel.values)
				if (named.value == v)
					out.name = named.name;
			return true;
		}
		// an enum name of the state, of a bool, or a named value of a selection
		if (m_state && m_state->names)
			for (const EnumName *e = m_state->names; e->name; ++e)
				if (token == e->name)
				{
					out.number = e->value;
					out.name = token;
					return true;
				}
		if (token == "true" || token == "false")
		{
			out.number = token == "true" ? 1 : 0;
			return true;
		}
		for (const SelectionDesc &sel : m_desc.selections)
			for (const SelectionDesc::Value &named : sel.values)
				if (!named.name.empty() && named.name == token)
				{
					out.number = named.value;
					out.name = token;
					return true;
				}
		// a material parameter (data and sampler states only)
		const ParamDesc *p = m_desc.findParam(token.c_str());
		if (p && m_state && m_state->kind >= 2)
		{
			out.param = true;
			out.name = token;
			return true;
		}
		if (p)
			return fail(token + " is a parameter: a pipeline state takes selections and constants");
		return fail("unknown name " + token);
	}

	const EffectDesc			&m_desc;
	const StateName				*m_state;
	const SelectionValues		&m_values;
	eastl::string				&m_error;
	eastl::vector<eastl::string> m_tokens;
	size_t						 m_at = 0;
};

eastl::string numberText(double n)
{
	char text[48];
	if (n == floor(n) && fabs(n) < 1e9)
		snprintf(text, sizeof text, "%lld", static_cast<long long>(n));
	else
		snprintf(text, sizeof text, "%.9g", n);
	return text;
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
const char *pathName(Path path)
{
	return path == Path::Pool ? "pool" : path == Path::Meshlets ? "ml" : "";
}

const char *rateName(Rate rate)
{
	switch (rate)
	{
	case Rate::Frame:
		return "frame";
	case Rate::Instance:
		return "instance";
	default:
		return "material";
	}
}

int32_t stateEnum(const char *state, const char *name)
{
	const StateName *s = findState(state);
	if (s && s->names)
		for (const EnumName *e = s->names; e->name; ++e)
			if (strcmp(e->name, name) == 0)
				return e->value;
	return INT32_MIN;
}

int32_t stateKind(const char *state)
{
	if (strcmp(state, "Texture") == 0)
		return 3;
	const StateName *s = findState(state);
	return s ? s->kind : 0;
}

//----------------------------------------------------------------------------------------------------------------------
const TechniqueDesc *EffectDesc::findTechnique(const char *techniqueName) const
{
	for (const TechniqueDesc &t : techniques)
		if (t.name == techniqueName)
			return &t;
	return nullptr;
}

const ParamDesc *EffectDesc::findParam(const char *paramName) const
{
	for (const ParamDesc &p : params)
		if (p.name == paramName)
			return &p;
	return nullptr;
}

int32_t EffectDesc::findSelection(const char *selectionName) const
{
	for (size_t i = 0; i < selections.size(); ++i)
		if (selections[i].name == selectionName)
			return static_cast<int32_t>(i);
	return -1;
}

void EffectDesc::defaults(SelectionValues &values) const
{
	values.clear();
	for (const SelectionDesc &s : selections)
		values.push_back(s.defaultValue);
}

bool parseRemap(const char *file, const eastl::string &text, eastl::vector<RemapRow> &rows,
				eastl::vector<eastl::string> &passthrough, eastl::string &error)
{
	rows.clear();
	passthrough.clear();
	uint32_t line = 0;
	size_t	 at = 0;
	auto	 words = [](const eastl::string &s, eastl::vector<eastl::string> &out)
	{
		out.clear();
		size_t i = 0;
		while (i < s.size())
		{
			while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
				++i;
			size_t e = i;
			while (e < s.size() && s[e] != ' ' && s[e] != '\t')
				++e;
			if (e > i)
				out.push_back(s.substr(i, e - i));
			i = e;
		}
	};
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == eastl::string::npos)
			end = text.size();
		eastl::string s = text.substr(at, end - at);
		at = end + 1;
		++line;
		const size_t hash = s.find('#');
		if (hash != eastl::string::npos)
			s.resize(hash);
		while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
			s.pop_back();
		char where[16];
		snprintf(where, sizeof where, ":%u", line);
		const eastl::string			 here = eastl::string(file) + where;
		eastl::vector<eastl::string> w;
		words(s, w);
		if (w.empty())
			continue;
		if (w[0] == "passthrough")
		{
			passthrough.insert(passthrough.end(), w.begin() + 1, w.end());
			continue;
		}
		// <original> <technique> -> <effect> <technique> [sel=value ...] | values | params
		eastl::vector<eastl::string> sections[3];
		{
			size_t start = 0;
			for (int k = 0; k < 3; ++k)
			{
				size_t bar = k < 2 ? s.find('|', start) : eastl::string::npos;
				words(s.substr(start, bar == eastl::string::npos ? eastl::string::npos : bar - start), sections[k]);
				if (bar == eastl::string::npos)
					break;
				start = bar + 1;
			}
		}
		const eastl::vector<eastl::string> &head = sections[0];
		if (head.size() < 5 || head[2] != "->")
			return error = here + ": error: a row is <effect> <technique> -> <effect> <technique> | values | params",
				   false;
		RemapRow &row = rows.push_back();
		row.where = here;
		row.original = head[0];
		row.originalTechnique = head[1];
		row.effect = head[3];
		row.technique = head[4];
		for (size_t i = 5; i < head.size(); ++i)
		{
			const size_t eq = head[i].find('=');
			if (eq == eastl::string::npos)
				return error = here + ": error: a selection is <name>=<value>: " + head[i], false;
			row.selections.push_back(eastl::make_pair(head[i].substr(0, eq), head[i].substr(eq + 1)));
		}
		for (const eastl::string &v : sections[1])
		{
			const size_t eq = v.find('=');
			if (eq == eastl::string::npos)
				return error = here + ": error: a value is <param>=<number[,number...]>: " + v, false;
			RemapRow::Value &value = row.values.push_back();
			value.param = v.substr(0, eq);
			const char *p = v.c_str() + eq + 1;
			while (*p)
			{
				char *next = nullptr;
				value.values.push_back(strtof(p, &next));
				if (next == p)
					return error = here + ": error: not a number in " + v, false;
				p = *next == ',' ? next + 1 : next;
			}
		}
		for (const eastl::string &m : sections[2])
		{
			const size_t eq = m.find('=');
			if (eq == eastl::string::npos)
				return error = here + ": error: a parameter is <caller parameter>=<effect parameter>:" + m, false;
			row.params.push_back(eastl::make_pair(m.substr(0, eq), m.substr(eq + 1)));
		}
	}
	return true;
}

bool vertexLayout(const PassDesc &pass, const VertexElement *elements, uint32_t count,
				  eastl::vector<VertexBinding> &out, eastl::string &error)
{
	struct Name
	{
		const char	  *semantic;
		VertexSemantic value;
	};
	static const Name names[] = {{"POSITION", VertexSemantic::Position},
								 {"BLENDWEIGHT", VertexSemantic::BlendWeight},
								 {"BLENDINDICES", VertexSemantic::BlendIndices},
								 {"NORMAL", VertexSemantic::Normal},
								 {"PSIZE", VertexSemantic::PointSize},
								 {"TEXCOORD", VertexSemantic::TexCoord},
								 {"TANGENT", VertexSemantic::Tangent},
								 {"BINORMAL", VertexSemantic::Binormal},
								 {"COLOR", VertexSemantic::Color},
								 {"TESSFACTOR", VertexSemantic::TessFactor},
								 {"POSITIONT", VertexSemantic::PositionT},
								 {"FOG", VertexSemantic::Fog},
								 {"DEPTH", VertexSemantic::Depth},
								 {"SAMPLE", VertexSemantic::Sample}};
	out.clear();
	for (const PassDesc::Input &in : pass.inputs)
	{
		const Name *name = nullptr;
		for (const Name &n : names)
			if (_stricmp(in.semantic.c_str(), n.semantic) == 0)
				name = &n;
		if (name == nullptr)
		{
			error = "the vertex input " + in.semantic + " has no vertex semantic of the backend";
			return false;
		}
		VertexBinding &b = out.push_back();
		b.semantic = in.semantic;
		b.index = in.index;
		b.type = in.type;
		for (uint32_t i = 0; i < count && !b.bound; ++i)
			if (elements[i].semantic == name->value && elements[i].semanticIndex == in.index)
			{
				b.bound = true;
				b.stream = elements[i].stream;
				b.offset = elements[i].offset;
				b.format = elements[i].format;
			}
	}
	return true;
}

void EffectDesc::carryOver(const EffectDesc &old, const SelectionValues &oldValues, SelectionValues &values) const
{
	defaults(values);
	for (size_t i = 0; i < selections.size(); ++i)
	{
		const int32_t o = old.findSelection(selections[i].name.c_str());
		if (o < 0 || static_cast<size_t>(o) >= oldValues.size())
			continue;
		for (const SelectionDesc::Value &v : selections[i].values)
			if (v.value == oldValues[o])
				values[i] = oldValues[o];
	}
}

bool EffectDesc::evaluate(const char *state, const eastl::string &expression, const SelectionValues &values,
						  eastl::string &text, eastl::string &error) const
{
	error.clear();
	Value	  v;
	Evaluator e(*this, state, values, error);
	if (!e.run(expression, v))
	{
		error = eastl::string(state) + " = " + expression + ": " + error;
		return false;
	}
	if (v.param)
	{
		text = v.name;
		return true;
	}
	// a named value given to an enum state maps by its name (a selection NONE = 1 to CullMode NONE)
	const StateName *s = findState(state);
	if (s && s->names && !v.name.empty())
	{
		const int32_t n = stateEnum(state, v.name.c_str());
		if (n != INT32_MIN)
			v.number = n;
	}
	text = numberText(v.number);
	return true;
}

bool EffectDesc::resolve(const PassDesc &pass, const SelectionValues &values, PassState &out,
						 eastl::string &error) const
{
	out = PassState();
	if (entryModel == EntryModel::Authored)
		out.blend = BlendMode::None;
	// a value the selection does not declare would make a variant nobody asked for (and a state number nobody meant)
	for (size_t i = 0; i < values.size() && i < selections.size(); ++i)
	{
		bool listed = false;
		for (const SelectionDesc::Value &v : selections[i].values)
			listed = listed || v.value == values[i];
		if (!listed)
		{
			char text[160];
			snprintf(text, sizeof text, "%s = %d is not one of its declared values", selections[i].name.c_str(),
					 values[i]);
			error = text;
			return false;
		}
	}
	if (pass.shader(Stage::Pixel).empty())
		out.blend = BlendMode::None;
	else if (pass.pixelOwnOutputs)
		out.blend = BlendMode::Own;
	for (const StateValue &s : pass.states)
	{
		if (s.state == "Targets")
		{
			// a list of target-set names, not an expression
			out.targets.clear();
			size_t start = 0;
			while (start < s.value.size())
			{
				size_t comma = s.value.find(',', start);
				if (comma == eastl::string::npos)
					comma = s.value.size();
				eastl::string target = s.value.substr(start, comma - start);
				while (!target.empty() && target.back() == ' ')
					target.pop_back();
				while (!target.empty() && target[0] == ' ')
					target.erase(0, 1);
				if (!target.empty())
					out.targets.push_back(target);
				start = comma + 1;
			}
			continue;
		}
		eastl::string text;
		if (!evaluate(s.state.c_str(), s.value, values, text, error))
			return false;
		const int32_t n = atoi(text.c_str());
		const char	 *k = s.state.c_str();
		if (!strcmp(k, "CullMode"))
			out.cullMode = static_cast<CullMode>(n);
		else if (!strcmp(k, "DepthTest"))
			out.depthTest = n != 0;
		else if (!strcmp(k, "DepthWrite"))
			out.depthWrite = n != 0;
		else if (!strcmp(k, "DepthFunc"))
			out.depthFunc = static_cast<CompareFunc>(n);
		else if (!strcmp(k, "ColorWriteMask"))
			out.colorWriteMask = static_cast<uint8_t>(n & 15);
		else if (!strcmp(k, "DepthBias"))
			out.depthBias = n;
		else if (!strcmp(k, "SlopeScaledDepthBias"))
			out.slopeScaledDepthBias = static_cast<float>(atof(text.c_str()));
		else if (!strcmp(k, "Blend"))
		{
			if (out.blend != BlendMode::Own)
				out.blend = n == 1 ? BlendMode::None : BlendMode::DualSource;
		}
		else if (!strcmp(k, "Meshlets"))
			out.meshlets = n != 0;
		else if (!strcmp(k, "PoolVertices"))
			out.poolVertices = n != 0;
		else if (!strcmp(k, "Vertices"))
			out.vertices = n != 0;
		else if (!strcmp(k, "Topology"))
			out.topology = static_cast<Topology>(n);
		else if (!strcmp(k, "Precache"))
			out.precache = n != 0;
		else if (!strcmp(k, "Screen"))
			out.screen = n != 0;
		else if (!strcmp(k, "AlphaTest"))
			out.alphaTest = n != 0;
		else if (!strcmp(k, "BlendEnable"))
			out.blendEnable = text;
		else if (!strcmp(k, "SrcBlend"))
			out.srcBlend = text;
		else if (!strcmp(k, "DestBlend"))
			out.destBlend = text;
		else if (!strcmp(k, "SeparateAlphaBlend"))
			out.separateAlphaBlend = text;
		else if (!strcmp(k, "SrcBlendAlpha"))
			out.srcBlendAlpha = text;
		else if (!strcmp(k, "DestBlendAlpha"))
			out.destBlendAlpha = text;
		else if (!strcmp(k, "AlphaRef"))
			out.alphaRef = text;
		else if (!strcmp(k, "BackFaceCull"))
			out.backFaceCull = text;
	}
	// back faces culled by the shaders (a material's data): the pipeline draws both faces
	if (out.backFaceCull != "0")
		out.cullMode = CullMode::None;
	// blending switched off by a constant: one output, no blend state (a full-screen pass, a 2D quad)
	if (out.blend == BlendMode::DualSource && out.blendEnable == "0")
		out.blend = BlendMode::None;
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
eastl::string EffectDesc::text() const
{
	eastl::string out = C_DESC_HEADER;
	out += "\n# generated from " + source + "; do not edit\n";
	out += "effect " + name + "\nsource " + source + "\n"; // the source path is the rest of its line
	if (entryModel == EntryModel::Authored)
		out += "entries authored\n";
	if (policyHash != 0)
		appendf(out, "policy %016llx\n", static_cast<unsigned long long>(policyHash));
	appendf(out, "layout %016llx %u %u %u %d\n", static_cast<unsigned long long>(layoutHash), recordSize[0],
			recordSize[1], recordSize[2], materialHeader ? 1 : 0);
	if (materialHeader && materialHeaderSize != C_LEGACY_MATERIAL_HEADER_SIZE)
		appendf(out, "headerbytes %u\n", materialHeaderSize);
	for (const SelectionDesc &s : selections)
	{
		appendf(out, "selection %s %s %d", s.name.c_str(), s.isBool ? "bool" : "int", s.defaultValue);
		for (const SelectionDesc::Value &v : s.values)
			appendf(out, v.name.empty() ? " %s%d" : " %s=%d", v.name.c_str(), v.value);
		out += "\n";
	}
	for (const ParamDesc &p : params)
	{
		out += "param " + p.name + " " + p.type + " rate=" + rateName(p.rate);
		appendf(out, " use=%u", p.use);
		if (p.count)
			appendf(out, " count=%u", p.count);
		if (!p.semantic.empty())
			out += " sem=" + p.semantic;
		if (p.dim != TextureDim::None)
			out += eastl::string(" dim=") + dimName(p.dim);
		if (p.offset >= 0)
			appendf(out, " offset=%d size=%u", p.offset, p.size);
		if (p.writable)
			out += " rw=1";
		if (!p.alias.empty())
			out += " alias=" + p.alias;
		if (!p.where.empty())
			out += " at=" + quotedIfBlank(p.where); // an include path may contain blanks
		if (!p.defaultValue.empty())
			out += " default=" + p.defaultValue;
		out += "\n";
		writeAnnotations(out, p.annotations);
	}
	for (const SamplerDesc &s : samplers)
	{
		appendf(out, "sampler %s %s texture=%s offset=%d\n", s.name.c_str(), dimName(s.dim),
				s.texture.empty() ? "-" : s.texture.c_str(), s.offset);
		for (const StateValue &v : s.states)
			out += "sstate " + v.state + " " + v.value + "\n";
	}
	for (const eastl::string &n : notes)
		out += "note " + n + "\n";
	for (const TargetSetDesc &t : targetSets)
	{
		out += "targetset " + t.name + " " + (t.depth.empty() ? eastl::string("-") : t.depth);
		for (const eastl::string &c : t.colors)
			out += " " + c;
		out += "\n";
	}
	for (const RegisterDesc &r : registers)
		appendf(out, "register %s %c %u %u\n", r.name.c_str(), r.kind, r.index, r.space);
	for (const StructDesc &s : structs)
	{
		appendf(out, "struct %s %u\n", s.name.c_str(), s.size);
		for (const StructDesc::Field &f : s.fields)
			appendf(out, "field %s %s %u %u\n", f.name.c_str(), f.type.c_str(), f.count, f.offset);
	}
	for (const TechniqueDesc &t : techniques)
	{
		out += "technique " + t.name + "\n";
		writeAnnotations(out, t.annotations);
		for (const PassDesc &p : t.passes)
		{
			out += "pass " + p.name + "\n";
			writeAnnotations(out, p.annotations);
			for (uint32_t s = 0; s < C_STAGES; ++s)
			{
				if (p.shaders[s].empty())
					continue;
				out += eastl::string("shader ") + C_STAGE_KEYS[s] + " " + p.shaders[s].function;
				for (const eastl::string &r : p.shaders[s].reads)
					out += " reads=" + r;
				for (const eastl::string &a : p.shaders[s].args)
					out += " " + a;
				out += "\n";
			}
			if (p.pixelOwnOutputs)
				out += "own\n";
			for (const PassDesc::Input &in : p.inputs)
				appendf(out, "input %s %u %s\n", in.semantic.c_str(), in.index, in.type.c_str());
			if (p.poolVertices)
				out += "poolvertices\n";
			if (p.vertices)
				out += "vertices\n";
			for (const PassDesc::Pin &pin : p.pins)
			{
				out += "pin " + pin.selection;
				for (int32_t v : pin.values)
					appendf(out, " %d", v);
				out += "\n";
			}
			if (p.meshlets)
				out += "meshlets\n";
			for (const StateValue &v : p.states)
				out += "state " + v.state + " " + v.value + "\n";
		}
	}
	return out;
}

//----------------------------------------------------------------------------------------------------------------------
bool EffectDesc::parse(const char *text, size_t size, eastl::string &error)
{
	*this = EffectDesc();
	// Descriptions predating an explicit entry model were all renderer-record descriptions.
	entryModel = EntryModel::RendererRecords;
	eastl::vector<eastl::string> words;
	eastl::vector<Annotation>	*annotations = nullptr;
	const char					*p = text;
	const char					*end = text + size;
	uint32_t					 line = 0;
	bool						 header = false;
	auto						 fail = [&](const char *what)
	{
		char head[32];
		snprintf(head, sizeof head, "line %u: ", line);
		error = eastl::string(head) + what;
		return false;
	};
	while (p < end)
	{
		const char *eol = static_cast<const char *>(memchr(p, '\n', end - p));
		if (!eol)
			eol = end;
		const char *begin = p;
		p = eol + 1;
		++line;
		splitWords(begin, eol, words);
		if (words.empty() || words[0][0] == '#')
			continue;
		const eastl::string &key = words[0];
		if (!header)
		{
			if (restAfter(begin, eol, 0) != C_DESC_HEADER)
				return fail("expected a render-desc 3 file");
			header = true;
			continue;
		}
		if (key == "effect")
		{
			if (words.size() != 2)
				return fail("effect: an effect name cannot contain blanks");
			name = words[1];
		}
		else if (key == "source" && words.size() >= 2)
			source = restAfter(begin, eol, 1);
		else if (key == "entries" && words.size() == 2 && words[1] == "authored")
			entryModel = EntryModel::Authored;
		else if (key == "policy" && words.size() == 2)
			policyHash = _strtoui64(words[1].c_str(), nullptr, 16);
		else if (key == "layout" && words.size() == 6)
		{
			layoutHash = _strtoui64(words[1].c_str(), nullptr, 16);
			for (uint32_t r = 0; r < C_RATES; ++r)
				recordSize[r] = static_cast<uint32_t>(atoi(words[2 + r].c_str()));
			materialHeader = words[5] == "1";
			materialHeaderSize = materialHeader ? C_LEGACY_MATERIAL_HEADER_SIZE : 0;
		}
		else if (key == "headerbytes" && words.size() == 2 && materialHeader)
			materialHeaderSize = static_cast<uint32_t>(strtoul(words[1].c_str(), nullptr, 10));
		else if (key == "selection" && words.size() >= 4)
		{
			if (words.size() == 4)
				return fail("selection: no values");
			SelectionDesc &s = selections.push_back();
			s.name = words[1];
			s.isBool = words[2] == "bool";
			s.defaultValue = atoi(words[3].c_str());
			for (size_t i = 4; i < words.size(); ++i)
			{
				SelectionDesc::Value &v = s.values.push_back();
				eastl::string		  k, value;
				if (splitPair(words[i], k, value))
				{
					v.name = k;
					v.value = atoi(value.c_str());
				}
				else
					v.value = atoi(words[i].c_str());
			}
		}
		else if (key == "param" && words.size() >= 3)
		{
			ParamDesc &d = params.push_back();
			d.name = words[1];
			d.type = words[2];
			for (size_t i = 3; i < words.size(); ++i)
			{
				eastl::string k, v;
				if (!splitPair(words[i], k, v))
					return fail("param: a word without '='");
				if (k == "rate")
					d.rate = v == "frame" ? Rate::Frame : v == "instance" ? Rate::Instance : Rate::Material;
				else if (k == "use")
					d.use = static_cast<uint8_t>(atoi(v.c_str()));
				else if (k == "count")
					d.count = static_cast<uint32_t>(atoi(v.c_str()));
				else if (k == "sem")
					d.semantic = v;
				else if (k == "dim")
				{
					if (!parseDim(v, d.dim))
						return fail("param: dim");
				}
				else if (k == "offset")
					d.offset = atoi(v.c_str());
				else if (k == "size")
					d.size = static_cast<uint32_t>(atoi(v.c_str()));
				else if (k == "default")
				{
					// the last key: the rest of the line (a quoted string default may hold blanks)
					d.defaultValue = restAfter(begin, eol, static_cast<int>(i)).substr(8);
					break;
				}
				else if (k == "rw")
					d.writable = v == "1";
				else if (k == "alias")
					d.alias = v;
				else if (k == "at")
					d.where = unquoted(v);
				else
					return fail("param: unknown key");
			}
			annotations = &d.annotations;
		}
		else if (key == "annot" && words.size() >= 4 && annotations)
		{
			Annotation &a = annotations->push_back();
			a.name = words[1];
			a.type = words[2];
			a.value = restAfter(begin, eol, 3);
		}
		else if (key == "sampler" && words.size() == 5)
		{
			SamplerDesc	 &s = samplers.push_back();
			eastl::string k, v;
			s.name = words[1];
			if (!parseDim(words[2], s.dim))
				return fail("sampler: dim");
			if (splitPair(words[3], k, v) && v != "-")
				s.texture = v;
			if (splitPair(words[4], k, v))
				s.offset = atoi(v.c_str());
			annotations = nullptr;
		}
		else if (key == "sstate" && words.size() >= 3 && !samplers.empty())
		{
			StateValue &v = samplers.back().states.push_back();
			v.state = words[1];
			v.value = restAfter(begin, eol, 2);
		}
		else if (key == "register" && words.size() == 5)
		{
			RegisterDesc &r = registers.push_back();
			r.name = words[1];
			r.kind = words[2][0];
			r.index = static_cast<uint32_t>(atoi(words[3].c_str()));
			r.space = static_cast<uint32_t>(atoi(words[4].c_str()));
		}
		else if (key == "struct" && words.size() == 3)
		{
			StructDesc &s = structs.push_back();
			s.name = words[1];
			s.size = static_cast<uint32_t>(atoi(words[2].c_str()));
		}
		else if (key == "field" && words.size() == 5 && !structs.empty())
		{
			StructDesc::Field &f = structs.back().fields.push_back();
			f.name = words[1];
			f.type = words[2];
			f.count = static_cast<uint32_t>(atoi(words[3].c_str()));
			f.offset = static_cast<uint32_t>(atoi(words[4].c_str()));
		}
		else if (key == "technique" && words.size() == 2)
		{
			techniques.push_back().name = words[1];
			annotations = &techniques.back().annotations;
		}
		else if (key == "pass" && words.size() == 2 && !techniques.empty())
		{
			techniques.back().passes.push_back().name = words[1];
			annotations = &techniques.back().passes.back().annotations;
		}
		else if (key == "shader" && words.size() >= 3 && !techniques.empty() && !techniques.back().passes.empty())
		{
			uint32_t s = 0;
			while (s < C_STAGES && words[1] != C_STAGE_KEYS[s])
				++s;
			if (s == C_STAGES)
				return fail("shader: unknown stage");
			ShaderRef &ref = techniques.back().passes.back().shaders[s];
			ref.function = words[2];
			for (size_t i = 3; i < words.size(); ++i)
			{
				if (words[i].compare(0, 6, "reads=") == 0)
					ref.reads.push_back(words[i].substr(6));
				else
					ref.args.push_back(words[i]);
			}
		}
		else if (key == "own" && !techniques.empty() && !techniques.back().passes.empty())
			techniques.back().passes.back().pixelOwnOutputs = true;
		else if (key == "meshlets" && !techniques.empty() && !techniques.back().passes.empty())
			techniques.back().passes.back().meshlets = true;
		else if (key == "poolvertices" && !techniques.empty() && !techniques.back().passes.empty())
			techniques.back().passes.back().poolVertices = true;
		else if (key == "vertices" && !techniques.empty() && !techniques.back().passes.empty())
			techniques.back().passes.back().vertices = true;
		else if (key == "input" && words.size() == 4 && !techniques.empty() && !techniques.back().passes.empty())
		{
			PassDesc::Input &in = techniques.back().passes.back().inputs.push_back();
			in.semantic = words[1];
			in.index = static_cast<uint32_t>(atoi(words[2].c_str()));
			in.type = words[3];
		}
		else if (key == "note")
			notes.push_back(restAfter(begin, eol, 1));
		else if (key == "targetset" && words.size() >= 3)
		{
			TargetSetDesc &t = targetSets.push_back();
			t.name = words[1];
			t.depth = words[2] == "-" ? eastl::string() : words[2];
			for (size_t i = 3; i < words.size(); ++i)
				t.colors.push_back(words[i]);
		}
		else if (key == "pin" && words.size() >= 2 && !techniques.empty() && !techniques.back().passes.empty())
		{
			if (words.size() == 2)
				return fail("pin: no values");
			PassDesc::Pin &pin = techniques.back().passes.back().pins.push_back();
			pin.selection = words[1];
			for (size_t i = 2; i < words.size(); ++i)
				pin.values.push_back(atoi(words[i].c_str()));
		}
		else if (key == "state" && words.size() >= 3 && !techniques.empty() && !techniques.back().passes.empty())
		{
			StateValue &v = techniques.back().passes.back().states.push_back();
			v.state = words[1];
			v.value = restAfter(begin, eol, 2);
		}
		else
			return fail("unknown or incomplete record");
	}
	if (!header || name.empty())
		return fail("no effect line");
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool EffectDesc::load(const char *path, eastl::string &error)
{
	FILE *file = nullptr;
	if (fopen_s(&file, path, "rb") != 0 || !file)
	{
		error = eastl::string("cannot open ") + path;
		return false;
	}
	eastl::string text;
	char		  buffer[4096];
	size_t		  got;
	while ((got = fread(buffer, 1, sizeof buffer, file)) > 0)
		text.append(buffer, buffer + got);
	fclose(file);
	if (!parse(text.data(), text.size(), error))
	{
		error = eastl::string(path) + ": " + error;
		return false;
	}
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
namespace
{
const InstanceSemantic *g_instanceSemantics = nullptr; // setInstanceSemantics: the caller's table
uint32_t				g_instanceSemanticCount = 0;
const char *const	   *g_providedSemantics = nullptr; // setProvidedSemantics: the caller's table
uint32_t				g_providedSemanticCount = 0;
const char *const	   *g_textureDefaults = nullptr; // setTextureDefaults: the caller's table
uint32_t				g_textureDefaultCount = 0;
} // namespace

void setInstanceSemantics(const InstanceSemantic *table, uint32_t count)
{
	g_instanceSemantics = table;
	g_instanceSemanticCount = table != nullptr ? count : 0;
}

const InstanceSemantic *instanceSemantic(const char *semantic)
{
	for (uint32_t i = 0; i < g_instanceSemanticCount; ++i)
		if (_stricmp(semantic, g_instanceSemantics[i].semantic) == 0)
			return &g_instanceSemantics[i];
	return nullptr;
}

void setProvidedSemantics(const char *const *names, uint32_t count)
{
	g_providedSemantics = names;
	g_providedSemanticCount = names != nullptr ? count : 0;
}

bool providedSemantic(const char *semantic)
{
	for (uint32_t i = 0; i < g_providedSemanticCount; ++i)
		if (_stricmp(semantic, g_providedSemantics[i]) == 0)
			return true;
	return false;
}

void setTextureDefaults(const char *const *names, uint32_t count)
{
	g_textureDefaults = names;
	g_textureDefaultCount = names != nullptr ? count : 0;
}

const char *const *textureDefaults(uint32_t &count)
{
	count = g_textureDefaultCount;
	return g_textureDefaults;
}

} // namespace render_backend::shader
