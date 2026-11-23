// Effect language front-end (see front_end.hpp). Contains a tokenizer over DXC's preprocessed text (#line directives
// give every token its file and line), the parser for the file-scope constructs, the parameter record layout (tight
// packing, as ByteAddressBuffer.Load<T> reads it), automatic register assignment, entry point generation, and the C++
// layout header generator.
#include "front_end.hpp"
#include <render_backend/shader/vocabulary.hpp>
#include <render_backend/shader/shader_cache.hpp>
#include <EASTL/sort.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace render_backend::shader::front
{
const char *const C_FRONTEND_VERSION = "render-fx 16";

namespace
{
// ------------------------------------------------------------------------------------------------------- tokens
enum TokenKind : uint8_t
{
	T_IDENT,
	T_NUMBER,
	T_STRING,
	T_PUNCT,
};

struct Token
{
	TokenKind kind = T_PUNCT;
	uint32_t  begin = 0, end = 0;
	uint32_t  file = 0, line = 0, column = 0;
};

bool isIdentStart(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isIdent(char c)
{
	return isIdentStart(c) || (c >= '0' && c <= '9');
}
bool isDigit(char c)
{
	return c >= '0' && c <= '9';
}

// "effects/../common/x.hlsli" -> "common/x.hlsli", "./x" -> "x", '\' -> '/'
eastl::string normalizePath(eastl::string path)
{
	for (char &c : path)
		if (c == '\\')
			c = '/';
	eastl::vector<eastl::string> parts;
	size_t						 start = 0;
	while (start <= path.size())
	{
		size_t slash = path.find('/', start);
		if (slash == eastl::string::npos)
			slash = path.size();
		const eastl::string part = path.substr(start, slash - start);
		if (part == ".." && !parts.empty() && parts.back() != "..")
			parts.pop_back();
		else if (!part.empty() && part != ".")
			parts.push_back(part);
		start = slash + 1;
	}
	eastl::string out;
	for (const eastl::string &p : parts)
		out += (out.empty() ? "" : "/") + p;
	return out;
}

// the tokens of preprocessed text; #line directives set the file and line, other directives are skipped
void tokenize(const eastl::string &s, eastl::vector<Token> &tokens, eastl::vector<eastl::string> &files)
{
	tokens.clear();
	files.clear();
	files.push_back("?");
	uint32_t	 file = 0, line = 1;
	size_t		 lineStart = 0;
	bool		 atLineStart = true;
	const size_t n = s.size();
	for (size_t i = 0; i < n;)
	{
		const char c = s[i];
		if (c == '\n')
		{
			++line;
			++i;
			lineStart = i;
			atLineStart = true;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r')
		{
			++i;
			continue;
		}
		if (c == '#' && atLineStart)
		{
			size_t e = s.find('\n', i);
			if (e == eastl::string::npos)
				e = n;
			const eastl::string directive = s.substr(i, e - i);
			const char		   *p = directive.c_str() + 1;
			while (*p == ' ')
				++p;
			if (!strncmp(p, "line", 4))
				p += 4;
			while (*p == ' ')
				++p;
			if (isDigit(*p))
			{
				const uint32_t next = static_cast<uint32_t>(strtoul(p, nullptr, 10));
				const char	  *q = strchr(p, '"');
				if (q)
				{
					const char		   *r = strchr(q + 1, '"');
					const eastl::string name = normalizePath(eastl::string(q + 1, r ? r : q + 1));
					file = 0;
					for (uint32_t f = 0; f < files.size(); ++f)
						if (files[f] == name)
							file = f;
					if (file == 0)
					{
						files.push_back(name);
						file = static_cast<uint32_t>(files.size() - 1);
					}
				}
				line = next - 1; // the newline ending the directive makes it `next`
			}
			i = e;
			continue;
		}
		atLineStart = false;
		Token t;
		t.begin = static_cast<uint32_t>(i);
		t.file = file;
		t.line = line;
		t.column = static_cast<uint32_t>(i - lineStart + 1);
		if (isIdentStart(c))
		{
			while (i < n && isIdent(s[i]))
				++i;
			t.kind = T_IDENT;
		}
		else if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(s[i + 1])))
		{
			while (i < n && (isIdent(s[i]) || s[i] == '.' ||
							 ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
				++i;
			t.kind = T_NUMBER;
		}
		else if (c == '"')
		{
			++i;
			while (i < n && s[i] != '"' && s[i] != '\n')
				i += s[i] == '\\' ? 2 : 1;
			if (i < n && s[i] == '"')
				++i;
			t.kind = T_STRING;
		}
		else
		{
			++i;
			if (i < n && ((c == '=' && s[i] == '=') || (c == '!' && s[i] == '=') || (c == '&' && s[i] == '&') ||
						  (c == '|' && s[i] == '|') || (c == ':' && s[i] == ':')))
				++i;
			t.kind = T_PUNCT;
		}
		t.end = static_cast<uint32_t>(i);
		tokens.push_back(t);
	}
}

void appendf(eastl::string &out, const char *format, ...)
{
	char	buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	out += buffer;
}

// ------------------------------------------------------------------------------------------------------- types
// a type as a record holds it: bool -> uint, half -> float, textures and samplers -> uint (a heap index)
bool isResource(const eastl::string &type);

eastl::string recordType(const eastl::string &type)
{
	if (type.compare(0, 4, "bool") == 0)
		return "uint" + type.substr(4);
	if (type.compare(0, 4, "half") == 0)
		return "float" + type.substr(4);
	if (isResource(type))
		return "uint";
	return type;
}

TextureDim textureDim(const eastl::string &type)
{
	const eastl::string t = type.compare(0, 2, "RW") == 0 ? type.substr(2) : type;
	if (t == "Texture2D" || t.compare(0, 10, "Texture2D<") == 0)
		return TextureDim::Tex2D;
	if (t == "TextureCube" || t.compare(0, 12, "TextureCube<") == 0)
		return TextureDim::Cube;
	if (t == "Texture3D" || t.compare(0, 10, "Texture3D<") == 0)
		return TextureDim::Tex3D;
	return TextureDim::None;
}

// a resource a record holds as a ResourceDescriptorHeap index: textures and buffers, read or written
bool isResource(const eastl::string &type)
{
	static const char *const prefixes[] = {
		"Texture",			 "RWTexture",			"Buffer",
		"RWBuffer",			 "StructuredBuffer",	"RWStructuredBuffer",
		"ByteAddressBuffer", "RWByteAddressBuffer", "RaytracingAccelerationStructure"};
	for (const char *prefix : prefixes)
	{
		const size_t n = strlen(prefix);
		if (type.compare(0, n, prefix) == 0 && (type.size() == n || type[n] == '<' || isDigit(type[n]) ||
												type.compare(n, 4, "Cube") == 0 || type.compare(n, 5, "Array") == 0))
			return true;
	}
	return false;
}

// rows and columns of a numeric type (float -> 1x1, float3 -> 1x3, float4x3 -> 4x3); false if not numeric
bool shapeOf(const eastl::string &type, eastl::string &scalar, uint32_t &rows, uint32_t &columns)
{
	static const char *const scalars[] = {"float", "half", "int", "uint", "bool", "dword"};
	for (const char *s : scalars)
	{
		const size_t n = strlen(s);
		if (type.compare(0, n, s) != 0)
			continue;
		const char *p = type.c_str() + n;
		scalar = s;
		rows = 1;
		columns = 1;
		if (*p == 0)
			return true;
		if (!isDigit(*p))
			continue; // e.g. "float_like"
		columns = static_cast<uint32_t>(*p++ - '0');
		if (*p == 'x' && isDigit(p[1]))
		{
			rows = columns;
			columns = static_cast<uint32_t>(p[1] - '0');
			p += 2;
		}
		return *p == 0 && rows >= 1 && rows <= 4 && columns >= 1 && columns <= 4;
	}
	return false;
}

// components of an interpolant type (float3 -> 3); 0 if it is not a scalar or vector
uint32_t componentsOf(const eastl::string &type)
{
	eastl::string scalar;
	uint32_t	  rows, columns;
	return shapeOf(type, scalar, rows, columns) && rows == 1 ? columns : 0;
}

eastl::string vectorType(const eastl::string &type, uint32_t n)
{
	eastl::string scalar;
	uint32_t	  rows, columns;
	shapeOf(type, scalar, rows, columns);
	if (scalar == "half")
		scalar = "float";
	if (scalar == "dword")
		scalar = "uint";
	char text[16];
	snprintf(text, sizeof text, n > 1 ? "%u" : "", n);
	return scalar + text;
}

eastl::string upper(eastl::string s)
{
	for (char &c : s)
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
	return s;
}

// a semantic as D3D9 and SM6 both mean it: upper case, no _centroid, an index (COLOR -> COLOR0)
eastl::string normSemantic(const eastl::string &semantic)
{
	eastl::string s = upper(semantic);
	const size_t  c = s.find("_CENTROID");
	if (c != eastl::string::npos)
		s.erase(c, 9);
	if (s.empty() || !isDigit(s.back()))
		s += "0";
	return s;
}

bool hasCentroid(const eastl::string &semantic)
{
	return upper(semantic).find("_CENTROID") != eastl::string::npos;
}

eastl::string withoutCentroid(const eastl::string &semantic)
{
	const size_t c = upper(semantic).find("_CENTROID");
	return c == eastl::string::npos ? semantic : semantic.substr(0, c) + semantic.substr(c + 9);
}

bool hasWord(const eastl::string &words, const char *word)
{
	const size_t n = strlen(word);
	for (size_t i = 0; i + n <= words.size(); ++i)
		if (words.compare(i, n, word) == 0 && (i == 0 || words[i - 1] == ' ') &&
			(i + n == words.size() || words[i + n] == ' '))
			return true;
	return false;
}

bool isNumberText(const eastl::string &s)
{
	if (s.empty())
		return false;
	char *end = nullptr;
	strtod(s.c_str(), &end);
	return end == s.c_str() + s.size();
}

uint64_t fnv(const eastl::string &text, uint64_t hash = 14695981039346656037ull)
{
	for (char c : text)
		hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ull;
	return hash;
}

const char *const C_STAGE_PROFILE[C_STAGES] = {"vs_6_6", "ps_6_6", "ms_6_6", "as_6_6", "cs_6_6"};
const char *const C_STAGE_STATE[C_STAGES] = {"VertexShader", "PixelShader", "MeshShader", "AmplificationShader",
											 "ComputeShader"};
const char *const C_RATE_RECORD[C_RATES] = {"Frame", "Instance", "Material"};

// ------------------------------------------------------------------------------------------------------- parser
struct RenderStateBlock
{
	eastl::string			  name;
	eastl::vector<StateValue> states;
	eastl::vector<uint32_t>	  tokens; // where each state was written
};

// where a state was written (its value is checked after the whole file is read): a pass state (sampler -1) or a
// sampler state (technique and pass -1)
struct StateUse
{
	int32_t	 technique, pass, sampler;
	uint32_t index, token;
};

// "Pin g_X = value;" / "Pin g_X = [a, b];" of a pass (the values as written: the selection is resolved at the end)
struct PinUse
{
	uint32_t					 technique = 0, pass = 0, token = 0;
	eastl::string				 selection;
	eastl::vector<eastl::string> values;
};

// a SamplerState as declared, with where its states were written
struct SampledState
{
	SamplerDesc				desc;
	eastl::vector<uint32_t> tokens;
	bool					shared = false;
};

// where a ShaderRef of a pass was written (the function is checked after the whole file is read)
struct ShaderUse
{
	uint32_t technique, pass, stage, token;
};

class Parser
{
  public:
	Parser(const eastl::string &text, Effect &effect, EffectDesc &desc, eastl::vector<Struct> &structs,
		   eastl::vector<Function> &functions, eastl::vector<eastl::string> &files, const char *preludePath,
		   eastl::string &error)
		: m_s(text), m_desc(desc), m_structs(structs), m_functions(functions), m_files(files), m_error(error),
		  m_preludePath(preludePath)
	{
		(void)effect;
		tokenize(text, m_t, files);
		// where the effect's own text begins: after the prelude's last token (global scope); the parameters' statics
		// go there, so all of the effect's code sees them wherever it stands
		for (size_t i = 0; i < m_t.size(); ++i)
			if (!m_preludePath.empty() && m_files[m_t[i].file] == m_preludePath)
				m_effectStart = m_t[i].end;
	}

	bool run();

	// the removed ranges (blanked in the body), the inserted texts (offset, text) and the identifiers the code uses
	eastl::vector<eastl::pair<uint32_t, uint32_t>>		removed;
	eastl::vector<eastl::pair<uint32_t, eastl::string>> inserted;
	eastl::vector<eastl::string>						used;
	int32_t												lastParamToken = -1; // the '}' of the last parameter block
	eastl::vector<ShaderUse>							shaderUses;
	eastl::vector<StateUse>								stateUses;
	eastl::vector<SampledState>							samplers;
	eastl::vector<PinUse>								pins;
	eastl::vector<bool>									standard; // per desc param: declared by the prelude

	const Token &token(size_t i) const
	{
		return m_t[i];
	}
	eastl::string where(size_t i) const
	{
		const Token &t = m_t[i < m_t.size() ? i : m_t.size() - 1];
		char		 at[32];
		snprintf(at, sizeof at, ":%u:%u", t.line, t.column);
		return m_files[t.file] + at;
	}
	bool failAt(size_t i, const eastl::string &what)
	{
		if (m_error.empty())
			m_error = where(i) + ": error: " + what;
		return false;
	}

  private:
	bool end() const
	{
		return m_at >= m_t.size();
	}
	eastl::string text(size_t i) const
	{
		return i < m_t.size() ? eastl::string(m_s.c_str() + m_t[i].begin, m_t[i].end - m_t[i].begin) : eastl::string();
	}
	bool is(size_t i, const char *what) const
	{
		if (i >= m_t.size())
			return false;
		const size_t n = m_t[i].end - m_t[i].begin;
		return n == strlen(what) && !memcmp(m_s.c_str() + m_t[i].begin, what, n);
	}
	bool isIdentAt(size_t i) const
	{
		return i < m_t.size() && m_t[i].kind == T_IDENT;
	}
	bool fail(const eastl::string &what)
	{
		return failAt(m_at, what);
	}
	bool expect(const char *what)
	{
		if (!is(m_at, what))
			return fail(eastl::string("'") + what + "' expected, not '" + text(m_at) + "'");
		++m_at;
		return true;
	}
	bool identifier(eastl::string &out)
	{
		if (!isIdentAt(m_at))
			return fail("a name expected, not '" + text(m_at) + "'");
		out = text(m_at++);
		return true;
	}
	void removeFrom(size_t first)
	{
		removed.push_back(eastl::make_pair(m_t[first].begin, m_t[m_at - 1].end));
	}
	void skipSemicolon()
	{
		if (is(m_at, ";"))
			++m_at;
	}
	// a type: a name, a template argument list <...> joined
	bool type(eastl::string &out)
	{
		if (!identifier(out))
			return false;
		if (is(m_at, "<"))
		{
			int depth = 0;
			do
			{
				if (is(m_at, "<"))
					++depth;
				else if (is(m_at, ">"))
					--depth;
				out += text(m_at++);
			} while (!end() && depth > 0);
		}
		return true;
	}
	bool annotations(eastl::vector<Annotation> &out);
	bool defaultValue(eastl::string &out);
	bool expression(eastl::string &out);
	bool stateLines(eastl::vector<StateValue> &out, bool sampler, eastl::string *texture,
					eastl::vector<uint32_t> &tokens);
	bool member(Member &m, bool allowDefault);

	bool selection();
	bool pin(eastl::vector<PinUse> &out);
	bool targetSet();
	bool params(Rate rate);
	bool renderState();
	bool sampler();
	bool technique();
	bool structure();
	bool generic();

	const eastl::string			   &m_s;
	EffectDesc					   &m_desc;
	eastl::vector<Struct>		   &m_structs;
	eastl::vector<Function>		   &m_functions;
	eastl::vector<eastl::string>   &m_files;
	eastl::string				   &m_error;
	eastl::string					m_preludePath;
	eastl::vector<Token>			m_t;
	size_t							m_at = 0;
	eastl::vector<RenderStateBlock> m_blocks;
	uint32_t						m_registers[4] = {}; // b t s u
	uint32_t						m_effectStart = 0;	 // the text offset after the prelude
};

bool Parser::annotations(eastl::vector<Annotation> &out)
{
	if (!is(m_at, "<"))
		return true;
	++m_at;
	while (!end() && !is(m_at, ">"))
	{
		Annotation &a = out.push_back();
		if (!identifier(a.type) || !identifier(a.name) || !expect("="))
			return false;
		if (!end() && m_t[m_at].kind == T_STRING)
			a.value = text(m_at++);
		else if (!defaultValue(a.value))
			return false;
		if (a.value.empty())
			a.value = "0";
		if (!expect(";"))
			return false;
	}
	return expect(">");
}

// a D3DX initial value up to ';' (not consumed): numbers joined by ',' (true/false as 1/0, all zero -> ""), a string
bool Parser::defaultValue(eastl::string &out)
{
	out.clear();
	if (!end() && m_t[m_at].kind == T_STRING)
	{
		out = text(m_at++);
		return true;
	}
	// a name (a texture's built-in default, the caller's; checked where the parameters are known)
	if (!end() && m_t[m_at].kind == T_IDENT && !is(m_at, "true") && !is(m_at, "false") && is(m_at + 1, ";"))
	{
		out = text(m_at++);
		return true;
	}
	bool zero = true;
	int	 depth = 0;
	while (!end() && !(depth == 0 && (is(m_at, ";") || is(m_at, ">"))))
	{
		eastl::string w = text(m_at);
		if (w == "{")
			++depth;
		else if (w == "}")
			--depth;
		else if (w == "," || w == "+")
		{
		}
		else if (w == "-" && m_at + 1 < m_t.size() && m_t[m_at + 1].kind == T_NUMBER)
		{
			++m_at;
			w = "-" + text(m_at);
			goto number;
		}
		else if (w == "true" || w == "false")
		{
			w = w == "true" ? "1" : "0";
			goto number;
		}
		else if (m_t[m_at].kind == T_NUMBER)
		{
		number:
			while (!w.empty() && (w.back() == 'f' || w.back() == 'F' || w.back() == 'h' || w.back() == 'H'))
				w.pop_back();
			if (strtod(w.c_str(), nullptr) != 0)
				zero = false;
			out += (out.empty() ? "" : ",") + w;
		}
		else
			return fail("a default value is numbers, true/false or a string, not '" + w + "'");
		++m_at;
	}
	if (zero)
		out.clear();
	return true;
}

// the tokens of a state value up to ';' (not consumed), joined by blanks
bool Parser::expression(eastl::string &out)
{
	out.clear();
	int depth = 0;
	while (!end() && !(depth == 0 && is(m_at, ";")))
	{
		if (is(m_at, "("))
			++depth;
		else if (is(m_at, ")"))
			--depth;
		if (is(m_at, "{") || is(m_at, "}"))
			return fail("';' expected after a state value");
		out += (out.empty() ? "" : " ") + text(m_at++);
	}
	if (out.empty())
		return fail("a state value expected");
	return true;
}

// Name = value; lines up to '}' (consumed)
bool Parser::stateLines(eastl::vector<StateValue> &out, bool sampler, eastl::string *texture,
						eastl::vector<uint32_t> &tokens)
{
	if (!expect("{"))
		return false;
	while (!end() && !is(m_at, "}"))
	{
		const size_t  at = m_at;
		eastl::string name;
		if (!identifier(name) || !expect("="))
			return false;
		const int32_t kind = stateKind(name.c_str());
		if (sampler ? kind != 3 : (kind != 1 && kind != 2 && kind != 4))
			return failAt(at, "unknown " + eastl::string(sampler ? "sampler" : "render") + " state " + name);
		if (name == "Texture")
		{
			if (is(m_at, "<"))
				++m_at;
			if (!identifier(*texture))
				return false;
			if (is(m_at, ">"))
				++m_at;
		}
		else
		{
			StateValue &v = out.push_back();
			v.state = name;
			tokens.push_back(static_cast<uint32_t>(at));
			if (!expression(v.value))
				return false;
			if (name == "BackFaceCull")
				used.push_back(v.value); // the material parameter the generated entries read: a record field
		}
		if (!expect(";"))
			return false;
	}
	return expect("}");
}

// [modifiers] type name [ '[' N ']' ] [':' semantic] - a struct member, a function parameter, a parameter declaration
bool Parser::member(Member &m, bool allowDefault)
{
	static const char *const modifiers[] = {"in",		"out",		  "inout",			 "uniform",		  "const",
											"centroid", "linear",	  "nointerpolation", "noperspective", "sample",
											"precise",	"static",	  "row_major",		 "column_major",  "vertices",
											"indices",	"primitives", "payload",		 "point",		  "line",
											"triangle", "lineadj",	  "triangleadj",	 "snorm",		  "unorm"};
	for (;;)
	{
		bool found = false;
		for (const char *mod : modifiers)
			if (is(m_at, mod))
				found = true;
		if (!found)
			break;
		m.modifiers += (m.modifiers.empty() ? "" : " ") + text(m_at++);
	}
	if (!type(m.type) || !identifier(m.name))
		return false;
	while (is(m_at, "["))
	{
		// an array; several dimensions count as their product (the record holds them in a row)
		++m_at;
		const uint32_t n = static_cast<uint32_t>(strtoul(text(m_at++).c_str(), nullptr, 0));
		m.count = m.count ? m.count * n : n;
		if (!expect("]"))
			return false;
	}
	if (is(m_at, ":") && isIdentAt(m_at + 1) && !is(m_at + 1, "register"))
	{
		m.semantic = text(m_at + 1);
		m_at += 2;
	}
	if (allowDefault && is(m_at, "="))
	{
		// a function parameter's default: skipped (to the ',' or ')' outside its own parentheses)
		int depth = 0;
		while (!end() && !(depth == 0 && (is(m_at, ",") || is(m_at, ")"))))
		{
			if (is(m_at, "("))
				++depth;
			else if (is(m_at, ")"))
				--depth;
			++m_at;
		}
	}
	return true;
}

bool Parser::selection()
{
	const size_t   first = m_at++;
	SelectionDesc &s = m_desc.selections.push_back();
	eastl::string  kind, defaultText;
	if (!identifier(kind))
		return false;
	if (kind != "bool" && kind != "int")
		return failAt(first + 1, "a selection is bool or int");
	s.isBool = kind == "bool";
	if (!identifier(s.name))
		return false;
	for (size_t i = 0; i + 1 < m_desc.selections.size(); ++i)
		if (m_desc.selections[i].name == s.name)
			return failAt(first + 2, "selection " + s.name + " declared twice");
	size_t defaultAt = 0;
	if (is(m_at, "="))
	{
		++m_at;
		defaultAt = m_at;
		if (is(m_at, "-"))
			defaultText = text(m_at++);
		defaultText += text(m_at++);
	}
	if (!expect(":") || !expect("["))
		return false;
	while (!end() && !is(m_at, "]"))
	{
		SelectionDesc::Value &v = s.values.push_back();
		const size_t		  at = m_at;
		eastl::string		  w = text(m_at++);
		if (s.isBool)
		{
			if (w != "true" && w != "false")
				return failAt(at, "a bool selection's values are true and false");
			v.value = w == "true";
		}
		else
		{
			if (m_t[at].kind == T_IDENT)
			{
				v.name = w;
				if (!expect("="))
					return false;
				w = text(m_at++);
			}
			if (w == "-")
				w += text(m_at++);
			if (!isNumberText(w))
				return failAt(at, "an int selection's value is a number or NAME = number");
			v.value = atoi(w.c_str());
		}
		if (is(m_at, ","))
			++m_at;
	}
	if (!expect("]") || !expect(";"))
		return false;
	if (s.values.empty())
		return failAt(first, "selection " + s.name + " has no values");
	s.defaultValue = s.values[0].value;
	if (!defaultText.empty())
	{
		bool found = false;
		for (const SelectionDesc::Value &v : s.values)
		{
			const bool match = s.isBool ? (defaultText == (v.value ? "true" : "false"))
										: (defaultText == v.name ||
										   (isNumberText(defaultText) && atoi(defaultText.c_str()) == v.value));
			if (match && !found)
			{
				s.defaultValue = v.value;
				found = true;
			}
		}
		if (!found)
			return failAt(defaultAt, "the default of " + s.name + " is not one of its values");
	}
	removeFrom(first);
	return true;
}

bool Parser::params(Rate rate)
{
	const size_t first = m_at++;
	if (!expect("{"))
		return false;
	// the block's text goes; each declaration becomes "static <type> <name>;" where it stood (ordinary HLSL scoping:
	// a local of the same name shadows it, the code after it sees it), the generated loader fills it
	eastl::vector<eastl::pair<uint32_t, eastl::string>> statics;
	while (!end() && !is(m_at, "}"))
	{
		const size_t at = m_at;
		Member		 m;
		if (!member(m, false))
			return false;
		ParamDesc p;
		p.name = m.name;
		p.type = m.type;
		p.count = m.count;
		p.semantic = m.semantic;
		p.rate = rate;
		p.dim = textureDim(m.type);
		if (!annotations(p.annotations))
			return false;
		if (is(m_at, "="))
		{
			++m_at;
			if (!defaultValue(p.defaultValue))
				return false;
		}
		if (!expect(";"))
			return false;
		const bool prelude = !m_preludePath.empty() && m_files[m_t[at].file] == m_preludePath;
		bool	   merged = false;
		for (size_t i = 0; i < m_desc.params.size(); ++i)
		{
			ParamDesc &old = m_desc.params[i];
			if (old.name != p.name)
				continue;
			// an effect may declare a shared parameter of the prelude again (same type and rate): its default and
			// annotations are taken
			if (!standard[i] || old.type != p.type || old.count != p.count || old.rate != p.rate)
				return failAt(at, "parameter " + p.name + " declared twice" +
									  (standard[i] ? " (the prelude's has another type or rate)" : ""));
			if (!p.defaultValue.empty())
				old.defaultValue = p.defaultValue;
			if (!p.annotations.empty())
				old.annotations = p.annotations;
			merged = true;
		}
		if (merged)
			continue;
		{
			char line[16];
			snprintf(line, sizeof line, ":%u", m_t[at].line);
			p.where = m_files[m_t[at].file] + line;
		}
		// another name of a shared parameter (the same semantic, e.g. g_LightAmbientIntensity : AMBIENT): no field of
		// its own; it holds the shared one's value
		if (!prelude && !p.semantic.empty())
			for (size_t i = 0; i < m_desc.params.size(); ++i)
			{
				const ParamDesc &shared = m_desc.params[i];
				if (!standard[i] || upper(shared.semantic) != upper(p.semantic))
					continue;
				if (shared.type != p.type || shared.count != p.count)
					return failAt(at, p.name + " : " + p.semantic + " is the shared " + shared.name + ", which is " +
										  shared.type);
				p.alias = shared.name;
				p.rate = shared.rate;
			}
		eastl::string decl =
			(p.type == "string" ? eastl::string("// ") : eastl::string("static ")) + p.type + " " + p.name;
		if (p.count)
			appendf(decl, "[%u]", p.count);
		// Prelude statics stay where they were declared. The effect's statics go to the start of its text, so all of
		// its code sees them, except those whose type is a struct of the effect: they stay where their block was,
		// after the struct.
		bool ofStruct = false;
		for (const Struct &s : m_structs)
			ofStruct = ofStruct || s.name == p.type;
		statics.push_back(eastl::make_pair(prelude || ofStruct ? m_t[at].begin : m_effectStart, decl + ";"));
		m_desc.params.push_back(p);
		standard.push_back(prelude);
	}
	if (!expect("}"))
		return false;
	skipSemicolon();
	removeFrom(first);
	for (const auto &d : statics)
		inserted.push_back(d);
	return true;
}

bool Parser::renderState()
{
	const size_t	  first = m_at++;
	RenderStateBlock &b = m_blocks.push_back();
	if (!identifier(b.name))
		return false;
	if (is(m_at, ":"))
	{
		++m_at;
		eastl::string base;
		const size_t  at = m_at;
		if (!identifier(base))
			return false;
		bool found = false;
		for (size_t i = 0; i + 1 < m_blocks.size(); ++i)
			if (m_blocks[i].name == base)
			{
				b.states = m_blocks[i].states;
				b.tokens = m_blocks[i].tokens;
				found = true;
			}
		if (!found)
			return failAt(at, "no RenderState " + base + " before this one");
	}
	if (!stateLines(b.states, false, nullptr, b.tokens))
		return false;
	skipSemicolon();
	removeFrom(first);
	return true;
}

bool Parser::sampler()
{
	const size_t first = m_at++;
	SampledState s;
	if (!identifier(s.desc.name))
		return false;
	if (is(m_at, ":"))
	{
		// SamplerState Name : Base { ... }: the base's states first, then its own
		++m_at;
		eastl::string base;
		const size_t  at = m_at;
		if (!identifier(base))
			return false;
		const SampledState *found = nullptr;
		for (const SampledState &candidate : samplers)
			if (candidate.desc.name == base)
				found = &candidate;
		if (!found)
			return failAt(at, "no SamplerState " + base + " before this one");
		s.desc.states = found->desc.states;
		s.desc.texture = found->desc.texture;
		s.tokens = found->tokens;
	}
	const size_t inherited = s.desc.states.size();
	if (!stateLines(s.desc.states, true, &s.desc.texture, s.tokens))
		return false;
	// its own line replaces the base's of the same state (the base's value is never evaluated then)
	for (size_t own = inherited; own < s.desc.states.size(); ++own)
		for (size_t i = 0; i < inherited; ++i)
			if (s.desc.states[i].state == s.desc.states[own].state)
				s.desc.states[i].state.clear();
	for (size_t i = s.desc.states.size(); i-- > 0;)
		if (s.desc.states[i].state.empty())
		{
			s.desc.states.erase(s.desc.states.begin() + i);
			s.tokens.erase(s.tokens.begin() + i);
		}
	// A sampler declared in a file under common/ (root-relative) is a shared template: it becomes part of the effect
	// only when the effect's code samples with it.
	s.shared = m_files[m_t[first].file].compare(0, 7, "common/") == 0;
	samplers.push_back(s);
	skipSemicolon();
	removeFrom(first);
	inserted.push_back(eastl::make_pair(m_t[first].begin, "static SamplerState " + s.desc.name + ";"));
	return true;
}

bool Parser::pin(eastl::vector<PinUse> &out)
{
	PinUse &use = out.push_back();
	use.token = static_cast<uint32_t>(m_at);
	++m_at; // Pin
	if (!identifier(use.selection) || !expect("="))
		return false;
	const bool list = is(m_at, "[");
	if (list)
		++m_at;
	while (!end() && !is(m_at, list ? "]" : ";"))
	{
		eastl::string v = text(m_at++);
		if (v == "-")
			v += text(m_at++);
		use.values.push_back(v);
		if (is(m_at, ","))
			++m_at;
	}
	if (list && !expect("]"))
		return false;
	if (use.values.empty())
		return failAt(use.token, "Pin " + use.selection + " has no values");
	return expect(";");
}

// TargetSet NAME { Color0 = FORMAT; ... Depth = FORMAT; };
bool Parser::targetSet()
{
	const size_t   first = m_at++;
	TargetSetDesc &t = m_desc.targetSets.push_back();
	if (!identifier(t.name) || !expect("{"))
		return false;
	static const char *const formats[] = {
		"RGBA16_FLOAT",		 "RGBA8_UNORM",		  "RGBA8_UNORM_SRGB", "BGRA8_UNORM",		 "BGRA8_UNORM_SRGB",
		"R10G10B10A2_UNORM", "R11G11B10_FLOAT",	  "RG16_FLOAT",		  "R16_FLOAT",			 "R32_FLOAT",
		"RG32_FLOAT",		 "RGBA32_FLOAT",	  "R8_UNORM",		  "RG8_UNORM",			 "R16_UNORM",
		"D32_FLOAT",		 "D24_UNORM_S8_UINT", "D16_UNORM",		  "D32_FLOAT_S8X24_UINT", "RGBA8_SNORM"};
	while (!end() && !is(m_at, "}"))
	{
		const size_t  at = m_at;
		eastl::string key, value;
		if (!identifier(key) || !expect("=") || !identifier(value) || !expect(";"))
			return false;
		bool known = false;
		for (const char *f : formats)
			known = known || value == f;
		if (!known)
			return failAt(at + 2, "unknown format " + value);
		const bool depthFormat = value[0] == 'D' && isDigit(value[1]);
		if (key == "Depth")
		{
			if (!depthFormat)
				return failAt(at + 2, value + " is not a depth format");
			t.depth = value;
		}
		else if (key.compare(0, 5, "Color") == 0 && key.size() == 6 && isDigit(key[5]) &&
				 static_cast<size_t>(key[5] - '0') == t.colors.size())
		{
			if (depthFormat)
				return failAt(at + 2, value + " is a depth format");
			t.colors.push_back(value);
		}
		else
			return failAt(at, "Color0, Color1 ... in order, or Depth expected, not " + key);
	}
	if (!expect("}"))
		return false;
	for (size_t i = 0; i + 1 < m_desc.targetSets.size(); ++i)
		if (m_desc.targetSets[i].name == t.name)
			return failAt(first + 1, "TargetSet " + t.name + " declared twice");
	skipSemicolon();
	removeFrom(first);
	return true;
}

bool Parser::technique()
{
	const size_t   first = m_at++;
	TechniqueDesc &t = m_desc.techniques.push_back();
	if (!identifier(t.name) || !annotations(t.annotations) || !expect("{"))
		return false;
	eastl::vector<PinUse> techniquePins;
	while (!end() && !is(m_at, "}"))
	{
		if (is(m_at, "Pin"))
		{
			if (!pin(techniquePins))
				return false;
			continue;
		}
		if (!is(m_at, "pass"))
			return fail("'pass' or 'Pin' expected");
		++m_at;
		PassDesc &p = t.passes.push_back();
		if (!identifier(p.name) || !annotations(p.annotations) || !expect("{"))
			return false;
		for (PinUse use : techniquePins)
		{
			use.technique = static_cast<uint32_t>(m_desc.techniques.size() - 1);
			use.pass = static_cast<uint32_t>(t.passes.size() - 1);
			pins.push_back(use);
		}
		while (!end() && !is(m_at, "}"))
		{
			if (is(m_at, "Pin"))
			{
				eastl::vector<PinUse> own;
				if (!pin(own))
					return false;
				own.back().technique = static_cast<uint32_t>(m_desc.techniques.size() - 1);
				own.back().pass = static_cast<uint32_t>(t.passes.size() - 1);
				pins.push_back(own.back());
				continue;
			}
			const size_t  at = m_at;
			eastl::string name;
			if (!identifier(name) || !expect("="))
				return false;
			uint32_t stage = 0;
			while (stage < C_STAGES && name != C_STAGE_STATE[stage])
				++stage;
			if (stage < C_STAGES)
			{
				if (is(m_at, "compile"))
				{
					// the D3DX form: the profile, which can only be SM 6.6's
					++m_at;
					eastl::string profile;
					if (!identifier(profile))
						return false;
					if (profile != C_STAGE_PROFILE[stage])
						return failAt(at, name + " needs the profile " + C_STAGE_PROFILE[stage] + ", not " + profile);
				}
				ShaderRef	&ref = p.shaders[stage];
				const size_t fn = m_at;
				if (!identifier(ref.function) || !expect("("))
					return false;
				while (!end() && !is(m_at, ")"))
				{
					eastl::string a = text(m_at++);
					if (a == "-")
						a += text(m_at++);
					ref.args.push_back(a);
					if (is(m_at, ","))
						++m_at;
				}
				if (!expect(")"))
					return false;
				shaderUses.push_back(ShaderUse{static_cast<uint32_t>(m_desc.techniques.size() - 1),
											   static_cast<uint32_t>(t.passes.size() - 1), stage,
											   static_cast<uint32_t>(fn)});
			}
			else if (name == "RenderState")
			{
				eastl::string block;
				const size_t  bt = m_at;
				if (!identifier(block))
					return false;
				const RenderStateBlock *found = nullptr;
				for (const RenderStateBlock &b : m_blocks)
					if (b.name == block)
						found = &b;
				if (!found)
					return failAt(bt, "no RenderState " + block + " before this technique");
				for (size_t i = 0; i < found->states.size(); ++i)
				{
					stateUses.push_back(StateUse{static_cast<int32_t>(m_desc.techniques.size() - 1),
												 static_cast<int32_t>(t.passes.size() - 1), -1,
												 static_cast<uint32_t>(p.states.size()), found->tokens[i]});
					p.states.push_back(found->states[i]);
				}
			}
			else
			{
				const int32_t kind = stateKind(name.c_str());
				if (kind != 1 && kind != 2 && kind != 4)
					return failAt(at, "unknown pass state " + name);
				stateUses.push_back(StateUse{static_cast<int32_t>(m_desc.techniques.size() - 1),
											 static_cast<int32_t>(t.passes.size() - 1), -1,
											 static_cast<uint32_t>(p.states.size()), static_cast<uint32_t>(at)});
				StateValue &v = p.states.push_back();
				v.state = name;
				if (!expression(v.value))
					return false;
				if (name == "BackFaceCull")
					used.push_back(v.value); // the material parameter the generated entries read: a record field
			}
			if (!expect(";"))
				return false;
		}
		if (!expect("}"))
			return false;
		if (p.shader(Stage::Vertex).empty() && p.shader(Stage::Mesh).empty() && p.shader(Stage::Compute).empty())
			return failAt(first, "pass " + t.name + "/" + p.name + " has no VertexShader, MeshShader or ComputeShader");
		if (!p.shader(Stage::Vertex).empty() && !p.shader(Stage::Mesh).empty())
			return failAt(first, "pass " + t.name + "/" + p.name +
									 " has both a VertexShader and a MeshShader: a pass is drawn by one of them");
	}
	if (!expect("}"))
		return false;
	skipSemicolon();
	removeFrom(first);
	return true;
}

bool Parser::structure()
{
	++m_at; // struct
	Struct &s = m_structs.push_back();
	if (!identifier(s.name) || !expect("{"))
		return false;
	while (!end() && !is(m_at, "}"))
	{
		// a member function (HLSL 2021): not data - skipped to its closing brace
		{
			size_t i = m_at;
			while (i < m_t.size() && !is(i, ";") && !is(i, "(") && !is(i, "}"))
				++i;
			if (is(i, "("))
			{
				// its body still counts: the parameters it reads get their fields, the selections it reads go into
				// the variants of whatever calls it (a function of its name: calls are found by identifier)
				Function method;
				method.name = text(i - 1);
				method.file = m_t[i].file;
				method.line = m_t[i].line;
				method.method = true;
				while (!end() && !is(m_at, "{"))
					++m_at;
				int depth = 0;
				while (!end())
				{
					if (m_t[m_at].kind == T_IDENT)
					{
						used.push_back(text(m_at));
						method.idents.push_back(text(m_at));
					}
					if (is(m_at, "{"))
						++depth;
					else if (is(m_at, "}") && --depth == 0)
					{
						++m_at;
						break;
					}
					++m_at;
				}
				m_functions.push_back(method);
				continue;
			}
		}
		Member &m = s.members.push_back();
		if (!member(m, false))
			return false;
		while (is(m_at, ","))
		{
			// float a, b; - one type, several names
			++m_at;
			Member next = m;
			next.count = 0;
			next.semantic.clear();
			if (!identifier(next.name))
				return false;
			s.members.push_back(next);
		}
		if (!expect(";"))
			return false;
	}
	if (!expect("}"))
		return false;
	skipSemicolon();
	return true;
}

// anything else at file scope: a function (its signature is recorded), a declaration (a resource without a register
// gets one), a cbuffer
bool Parser::generic()
{
	const size_t first = m_at;
	auto		 note = [&](size_t i)
	{
		if (m_t[i].kind == T_IDENT)
			used.push_back(text(i));
	};
	auto registerAfter = [&](size_t nameToken, char kind)
	{
		const uint32_t k = kind == 'b' ? 0 : kind == 't' ? 1 : kind == 's' ? 2 : 3;
		char		   reg[48];
		snprintf(reg, sizeof reg, " : register(%c%u)", kind, m_registers[k]);
		RegisterDesc &r = m_desc.registers.push_back();
		r.name = text(nameToken);
		r.kind = kind;
		r.index = m_registers[k]++;
		inserted.push_back(eastl::make_pair(m_t[nameToken].end, eastl::string(reg)));
	};
	if (is(m_at, "cbuffer") || is(m_at, "tbuffer"))
	{
		const char kind = is(m_at, "cbuffer") ? 'b' : 't';
		++m_at;
		if (!isIdentAt(m_at))
			return fail("a cbuffer name expected");
		if (!is(m_at + 1, ":"))
			registerAfter(m_at, kind);
		while (!end() && !is(m_at, "{"))
			++m_at;
		int depth = 0;
		while (!end())
		{
			note(m_at);
			if (is(m_at, "{"))
				++depth;
			else if (is(m_at, "}") && --depth == 0)
			{
				++m_at;
				break;
			}
			++m_at;
		}
		skipSemicolon();
		return true;
	}
	// the head up to ';' or '{' at depth 0
	int	   depth = 0;
	size_t open = 0; // the first '(' at depth 0
	while (!end())
	{
		if (depth == 0 && (is(m_at, ";") || is(m_at, "{")))
			break;
		if (is(m_at, "(") || is(m_at, "["))
		{
			if (depth == 0 && is(m_at, "(") && !open)
				open = m_at;
			++depth;
		}
		else if (is(m_at, ")") || is(m_at, "]"))
			--depth;
		note(m_at);
		++m_at;
	}
	if (end())
		return failAt(first, "this declaration does not end");
	if (is(m_at, "{") && open && !is(open - 1, "=") && isIdentAt(open - 1))
	{
		// a function: the signature, then the body
		Function &f = m_functions.push_back();
		f.name = text(open - 1);
		f.file = m_t[open - 1].file;
		f.line = m_t[open - 1].line;
		size_t start = first;
		while (start < open - 1 && is(start, "["))
		{
			// an attribute [numthreads(...)]: kept for the generated compute entry
			const size_t from = start;
			int			 d = 0;
			do
			{
				if (is(start, "["))
					++d;
				else if (is(start, "]"))
					--d;
				++start;
			} while (start < open && d > 0);
			f.attributes += m_s.substr(m_t[from].begin, m_t[start - 1].end - m_t[from].begin);
		}
		for (size_t i = start; i < open - 1; ++i)
			if (!is(i, "inline") && !is(i, "static") && !is(i, "precise") && !is(i, "export"))
				f.returnType += text(i);
		const size_t keep = m_at;
		m_at = open + 1;
		while (!is(m_at, ")"))
		{
			Member p;
			if (is(m_at, "void") && is(m_at + 1, ")"))
			{
				++m_at;
				break;
			}
			if (!member(p, true))
				return false;
			f.params.push_back(p);
			if (is(m_at, ","))
				++m_at;
			else if (!is(m_at, ")"))
				return fail("',' or ')' expected in the parameters of " + f.name);
		}
		++m_at;
		if (is(m_at, ":") && isIdentAt(m_at + 1))
			f.returnSemantic = text(m_at + 1);
		m_at = keep;
		depth = 0;
		while (!end())
		{
			note(m_at);
			if (m_t[m_at].kind == T_IDENT)
				f.idents.push_back(text(m_at)); // what it reads and calls: the selections a variant depends on
			if (is(m_at, "{"))
				++depth;
			else if (is(m_at, "}") && --depth == 0)
			{
				++m_at;
				break;
			}
			++m_at;
		}
		skipSemicolon();
		return true;
	}
	if (is(m_at, "{"))
	{
		// an initialiser list: on to the ';'
		depth = 0;
		while (!end() && !(depth == 0 && is(m_at, ";")))
		{
			if (is(m_at, "{"))
				++depth;
			else if (is(m_at, "}"))
				--depth;
			note(m_at);
			++m_at;
		}
	}
	// a resource declaration without a register: Type[<...>] name [ [n] ] ;
	size_t i = first;
	bool   isStatic = false;
	while (is(i, "static") || is(i, "uniform") || is(i, "globallycoherent") || is(i, "const"))
	{
		if (is(i, "static"))
			isStatic = true;
		++i;
	}
	const eastl::string typeName = text(i);
	char				kind = 0;
	if (typeName == "ConstantBuffer")
		kind = 'b';
	else if (typeName.compare(0, 2, "RW") == 0 || typeName == "AppendStructuredBuffer" ||
			 typeName == "ConsumeStructuredBuffer" || typeName.compare(0, 13, "RasterizerOrd") == 0)
		kind = 'u';
	else if (typeName == "SamplerState" || typeName == "SamplerComparisonState")
		kind = 's';
	else if (typeName.compare(0, 7, "Texture") == 0 || typeName == "Buffer" || typeName == "ByteAddressBuffer" ||
			 typeName == "StructuredBuffer" || typeName == "RaytracingAccelerationStructure")
		kind = 't';
	if (kind && !isStatic && is(m_at, ";"))
	{
		++i;
		if (is(i, "<"))
		{
			int d = 0;
			do
			{
				if (is(i, "<"))
					++d;
				else if (is(i, ">"))
					--d;
				++i;
			} while (i < m_at && d > 0);
		}
		bool hasRegister = false;
		for (size_t j = i; j < m_at; ++j)
			hasRegister = hasRegister || is(j, "register");
		if (isIdentAt(i) && !hasRegister && typeName == "RaytracingAccelerationStructure")
			// the renderer binds no t register of a scene pipeline: the scene's TLAS comes from the frame header,
			// another one from a parameter block (a heap index in the record, like a texture)
			return failAt(first, "a RaytracingAccelerationStructure without a register is bound by nobody: the scene's "
								 "is a scene TLAS supplied by the shader profile, another one a parameter of a block "
								 "(dispatch { RaytracingAccelerationStructure t_Tlas; })");
		if (isIdentAt(i) && !hasRegister)
		{
			size_t nameEnd = i;
			if (is(i + 1, "["))
				while (nameEnd < m_at && !is(nameEnd, "]"))
					++nameEnd;
			registerAfter(nameEnd, kind);
			if (nameEnd != i)
				m_desc.registers.back().name = text(i);
		}
	}
	if (is(m_at, ";"))
		++m_at;
	return true;
}

bool Parser::run()
{
	while (!end())
	{
		if (is(m_at, "selection") && isIdentAt(m_at + 1))
		{
			if (!selection())
				return false;
		}
		else if ((is(m_at, "frame") || is(m_at, "instance") || is(m_at, "material") || is(m_at, "dispatch")) &&
				 is(m_at + 1, "{"))
		{
			if (!params(is(m_at, "frame") ? Rate::Frame : is(m_at, "instance") ? Rate::Instance : Rate::Material))
				return false;
		}
		else if (is(m_at, "RenderState") && isIdentAt(m_at + 1) && (is(m_at + 2, "{") || is(m_at + 2, ":")))
		{
			if (!renderState())
				return false;
		}
		else if (is(m_at, "SamplerState") && isIdentAt(m_at + 1) && (is(m_at + 2, "{") || is(m_at + 2, ":")))
		{
			if (!sampler())
				return false;
		}
		else if (is(m_at, "TargetSet") && isIdentAt(m_at + 1) && is(m_at + 2, "{"))
		{
			if (!targetSet())
				return false;
		}
		else if (is(m_at, "technique") && isIdentAt(m_at + 1))
		{
			if (!technique())
				return false;
		}
		else if (is(m_at, "struct") && isIdentAt(m_at + 1) && is(m_at + 2, "{"))
		{
			if (!structure())
				return false;
		}
		else if (!generic())
			return false;
	}
	return true;
}

// ------------------------------------------------------------------------------------------------------- layout
struct Field
{
	eastl::string name;
	eastl::string type; // record type
	uint32_t	  count = 0;
	uint32_t	  offset = 0;
	uint32_t	  size = 0;
};

} // namespace

//----------------------------------------------------------------------------------------------------------------------
const Struct *Effect::findStruct(const eastl::string &name) const
{
	for (const Struct &s : m_structs)
		if (s.name == name)
			return &s;
	return nullptr;
}

const Function *Effect::findFunction(const eastl::string &name) const
{
	for (const Function &f : m_functions)
		if (f.name == name && !f.method)
			return &f;
	return nullptr;
}

namespace
{
// the bytes of a record type (tight, 4-byte units); 0 if unknown. Structs of the effect are laid out recursively
// and added to desc.structs once.
uint32_t sizeOf(const eastl::string &type, const eastl::vector<Struct> &structs, EffectDesc &desc, eastl::string &error,
				const MaterialHeaderAbi &header)
{
	eastl::string scalar;
	uint32_t	  rows, columns;
	if (header.type != nullptr && type == header.type)
		return header.size;
	if (shapeOf(type, scalar, rows, columns))
		return rows * columns * 4;
	for (const StructDesc &d : desc.structs)
		if (d.name == type)
			return d.size;
	for (const Struct &s : structs)
	{
		if (s.name != type)
			continue;
		StructDesc d;
		d.name = type;
		for (const Member &m : s.members)
		{
			StructDesc::Field &f = d.fields.push_back();
			f.name = m.name;
			f.type = recordType(m.type);
			f.count = m.count;
			f.offset = d.size;
			const uint32_t n = sizeOf(f.type, structs, desc, error, header);
			if (n == 0)
				return 0;
			d.size += n * (m.count ? m.count : 1);
		}
		desc.structs.push_back(d);
		return d.size;
	}
	error = "a record cannot hold the type " + type;
	return 0;
}

bool usedName(const eastl::vector<eastl::string> &used, const eastl::string &name)
{
	return eastl::binary_search(used.begin(), used.end(), name);
}

// the selections an expression names
void selectionsIn(const EffectDesc &desc, const eastl::string &expression, eastl::vector<int32_t> &out)
{
	size_t start = 0;
	while (start < expression.size())
	{
		size_t blank = expression.find(' ', start);
		if (blank == eastl::string::npos)
			blank = expression.size();
		const int32_t s = desc.findSelection(expression.substr(start, blank - start).c_str());
		if (s >= 0 && eastl::find(out.begin(), out.end(), s) == out.end())
			out.push_back(s);
		start = blank + 1;
	}
}

bool namesParam(const EffectDesc &desc, const eastl::string &expression)
{
	size_t start = 0;
	while (start < expression.size())
	{
		size_t blank = expression.find(' ', start);
		if (blank == eastl::string::npos)
			blank = expression.size();
		if (desc.findParam(expression.substr(start, blank - start).c_str()))
			return true;
		start = blank + 1;
	}
	return false;
}

// the blend's five numbers (enable, src, dst, srcAlpha, dstAlpha) when the pass fixes it; false: the blend is the
// material's (the material header's blend flags)
bool constantBlend(const PassState &s, eastl::string (&out)[5])
{
	if (s.blendEnable == "0")
	{
		// blending off: A = colour, B = 0 whatever the factors
		out[0] = "0";
		out[1].clear();
		out[2].clear();
		appendf(out[1], "%u", uint32_t(BlendFactor::One));
		appendf(out[2], "%u", uint32_t(BlendFactor::Zero));
		out[3] = out[1];
		out[4] = out[2];
		return true;
	}
	if (!isNumberText(s.blendEnable) || !isNumberText(s.srcBlend) || !isNumberText(s.destBlend))
		return false;
	const bool separate = isNumberText(s.separateAlphaBlend) && atoi(s.separateAlphaBlend.c_str()) != 0;
	if (separate && (!isNumberText(s.srcBlendAlpha) || !isNumberText(s.destBlendAlpha)))
		return false;
	out[0] = s.blendEnable;
	out[1] = s.srcBlend;
	out[2] = s.destBlend;
	out[3] = separate ? s.srcBlendAlpha : s.srcBlend;
	out[4] = separate ? s.destBlendAlpha : s.destBlend;
	return true;
}

// The five operands of the blend function as HLSL: the pass's constants, the rest read from the material's packed
// blend factors (rbBlendFactors). true when all are constants.
bool blendOperands(const PassState &s, eastl::string (&out)[5])
{
	if (constantBlend(s, out))
		return true;
	static const char *const bits[5] = {"((rbBlendFactors >> 16) & 1)", "(rbBlendFactors & 15)", "((rbBlendFactors >> 4) & 15)", "((rbBlendFactors >> 8) & 15)",
										"((rbBlendFactors >> 12) & 15)"};
	const eastl::string		*colour[3] = {&s.blendEnable, &s.srcBlend, &s.destBlend};
	for (int i = 0; i < 3; ++i)
		out[i] = isNumberText(*colour[i]) ? *colour[i] : eastl::string(bits[i]);
	if (s.separateAlphaBlend == "0")
	{
		out[3] = out[1];
		out[4] = out[2];
	}
	else
	{
		// a separate alpha blend, or one the material decides (the renderer packs the alpha bits resolved)
		const bool constant = s.separateAlphaBlend == "1";
		out[3] = constant && isNumberText(s.srcBlendAlpha) ? s.srcBlendAlpha : eastl::string(bits[3]);
		out[4] = constant && isNumberText(s.destBlendAlpha) ? s.destBlendAlpha : eastl::string(bits[4]);
	}
	return false;
}

const char *C_DATA_STATES[] = {"BlendEnable",		 "SrcBlend",	  "DestBlend",
							   "SeparateAlphaBlend", "SrcBlendAlpha", "DestBlendAlpha",
							   "AlphaRef",			 "AlphaTest",	  "Blend"};

} // namespace

//----------------------------------------------------------------------------------------------------------------------
bool Effect::parse(const char *effectName, const char *sourceRel, const eastl::string &preprocessed,
				   const ShaderPolicy &policy, eastl::string &error)
{
	desc = EffectDesc();
	body.clear();
	m_structs.clear();
	m_functions.clear();
	m_records = false;
	m_recordAbi = policy.recordAbi;
	error.clear();
	desc.name = effectName;
	desc.source = sourceRel;
	desc.entryModel = policy.entries;
	// The .desc text format separates words by blanks.
	for (const char *c = effectName; *c; ++c)
		if (*c == ' ' || *c == '\t')
			return error = eastl::string(sourceRel) + ": error: effect names cannot contain blanks", false;
	Parser p(preprocessed, *this, desc, m_structs, m_functions, m_files,
			 policy.preludePath != nullptr ? policy.preludePath : "", error);
	if (!p.run())
		return false;
	eastl::sort(p.used.begin(), p.used.end());
	for (Function &f : m_functions)
	{
		eastl::sort(f.idents.begin(), f.idents.end());
		f.idents.erase(eastl::unique(f.idents.begin(), f.idents.end()), f.idents.end());
	}

	// the functions the passes name
	for (const ShaderUse &u : p.shaderUses)
	{
		PassDesc		&pass = desc.techniques[u.technique].passes[u.pass];
		const ShaderRef &ref = pass.shaders[u.stage];
		const Function	*f = findFunction(ref.function);
		if (!f)
			return p.failAt(u.token, "no function " + ref.function + " before this technique");
		if (u.stage == static_cast<uint32_t>(Stage::Pixel))
			pass.pixelOwnOutputs = findStruct(f->returnType) != nullptr;
		for (const eastl::string &a : ref.args)
			if (desc.findSelection(a.c_str()) < 0 && !isNumberText(a) && a != "true" && a != "false")
				return p.failAt(u.token, "an argument of " + ref.function + " is a selection or a literal, not " + a);
	}

	// Drop selections that nothing names (no state, argument or function body). A shared include can declare
	// selections that only some effects use.
	{
		eastl::vector<SelectionDesc> kept;
		for (const SelectionDesc &s : desc.selections)
		{
			bool named = false;
			auto inExpression = [&](const eastl::string &e)
			{
				eastl::vector<int32_t> found;
				EffectDesc			   one;
				one.selections.push_back(s);
				selectionsIn(one, e, found);
				return !found.empty();
			};
			for (const TechniqueDesc &t : desc.techniques)
				for (const PassDesc &pass : t.passes)
				{
					for (const StateValue &v : pass.states)
						named = named || inExpression(v.value);
					for (uint32_t st = 0; st < C_STAGES; ++st)
						for (const eastl::string &a : pass.shaders[st].args)
							named = named || a == s.name;
				}
			for (const Function &f : m_functions)
				named = named || eastl::binary_search(f.idents.begin(), f.idents.end(), s.name);
			if (named)
				kept.push_back(s);
		}
		desc.selections.swap(kept);
	}
	// the pins: values of the selection's list
	for (const PinUse &use : p.pins)
	{
		const int32_t s = desc.findSelection(use.selection.c_str());
		if (s < 0)
			return p.failAt(use.token, "Pin " + use.selection + ": no selection of that name in this effect");
		PassDesc::Pin &pin = desc.techniques[use.technique].passes[use.pass].pins.push_back();
		pin.selection = use.selection;
		for (const eastl::string &text : use.values)
		{
			const SelectionDesc &sel = desc.selections[s];
			bool				 found = false;
			for (const SelectionDesc::Value &v : sel.values)
			{
				const bool match = sel.isBool
									   ? text == (v.value ? "true" : "false")
									   : (text == v.name || (isNumberText(text) && atoi(text.c_str()) == v.value));
				if (match && !found)
				{
					pin.values.push_back(v.value);
					found = true;
				}
			}
			if (!found)
				return p.failAt(use.token, "Pin " + use.selection + " = " + text + ": not one of its values");
		}
	}
	// The selections each stage reads by name, in its function and every function it calls.
	auto readsOf = [&](const eastl::string &function, eastl::vector<eastl::string> &out)
	{
		out.clear();
		eastl::vector<eastl::string> todo, seen;
		todo.push_back(function);
		while (!todo.empty())
		{
			const eastl::string name = todo.back();
			todo.pop_back();
			if (eastl::find(seen.begin(), seen.end(), name) != seen.end())
				continue;
			seen.push_back(name);
			for (const Function &f : m_functions)
			{
				if (f.name != name)
					continue;
				for (const eastl::string &id : f.idents)
				{
					if (desc.findSelection(id.c_str()) >= 0 && eastl::find(out.begin(), out.end(), id) == out.end())
						out.push_back(id);
					if (findFunction(id))
						todo.push_back(id);
				}
			}
		}
		eastl::sort(out.begin(), out.end());
	};
	for (TechniqueDesc &t : desc.techniques)
		for (PassDesc &pass : t.passes)
			for (uint32_t st = 0; st < C_STAGES; ++st)
				if (!pass.shaders[st].empty())
					readsOf(pass.shaders[st].function, pass.shaders[st].reads);

	// Collect the vertex inputs and choose the geometry paths. A pass with "Meshlets = true;" or
	// "PoolVertices = true;" is drawn from the pools only if its vertex function reads nothing beyond a pool vertex.
	for (TechniqueDesc &t : desc.techniques)
		for (PassDesc &pass : t.passes)
		{
			const Function *vs = findFunction(pass.shader(Stage::Vertex).function);
			if (vs)
				for (const Member &m : vs->params)
				{
					if (hasWord(m.modifiers, "uniform"))
						continue;
					auto addInput = [&](const Member &in)
					{
						eastl::string name = withoutCentroid(in.semantic);
						uint32_t	  index = 0;
						size_t		  digits = name.size();
						while (digits > 0 && isDigit(name[digits - 1]))
							--digits;
						if (digits < name.size())
							index = static_cast<uint32_t>(atoi(name.c_str() + digits));
						name.resize(digits);
						if (upper(name).compare(0, 3, "SV_") == 0)
							return; // a system value, not a vertex element
						PassDesc::Input &i = pass.inputs.push_back();
						i.semantic = name;
						i.index = index;
						i.type = in.type;
					};
					if (!m.semantic.empty())
						addInput(m);
					else if (const Struct *s = findStruct(m.type))
						for (const Member &sm : s->members)
							if (!sm.semantic.empty())
								addInput(sm);
				}
			PassState	  state;
			eastl::string why;
			if (!vs || !desc.resolve(pass, SelectionValues(), state, why))
			{
				pass.vertices = vs != nullptr;
				continue;
			}
			const bool capable = pass.shader(Stage::Mesh).empty() && meshletInputs(*vs);
			pass.vertices = true; // its own vertices unless it has another path (below)
			if ((state.meshlets || state.poolVertices) && !capable)
			{
				char at[32];
				snprintf(at, sizeof at, ":%u", vs->line);
				desc.notes.push_back(m_files[vs->file] + at + ": " + t.name + "/" + pass.name + ": " + vs->name +
									 " reads more than a pool vertex has (POSITION, TEXCOORD0, NORMAL, TANGENT, "
									 "BINORMAL, COLOR0): drawn from its own vertices only");
				continue;
			}
			// one geometry path: meshlets, else the pools, else its own vertices (and those too when it says so)
			pass.meshlets = state.meshlets;
			pass.poolVertices = state.poolVertices && !state.meshlets;
			pass.vertices = state.vertices || (!pass.meshlets && !pass.poolVertices);
		}

	// resources, what the code reads, what the states name
	for (ParamDesc &param : desc.params)
	{
		if (isResource(param.type))
		{
			param.use |= C_USE_TEXTURE;
			param.writable = param.type.compare(0, 2, "RW") == 0;
		}
		if (param.type != "string" && usedName(p.used, param.name))
			param.use |= C_USE_RECORD;
		if (desc.findSelection(param.name.c_str()) >= 0)
			return error = param.where + ": error: " + param.name + " is a parameter and a selection", false;
	}
	auto markState = [&](const eastl::string &expression)
	{
		for (ParamDesc &param : desc.params)
		{
			size_t at = expression.find(param.name);
			while (at != eastl::string::npos)
			{
				const size_t e = at + param.name.size();
				if ((at == 0 || expression[at - 1] == ' ') && (e == expression.size() || expression[e] == ' '))
					param.use |= C_USE_STATE;
				at = expression.find(param.name, e);
			}
		}
	};
	// Keep the effect's own samplers and the shared ones (common/) that its code samples with.
	for (const SampledState &s : p.samplers)
	{
		if (s.shared && !usedName(p.used, s.desc.name))
			continue;
		desc.samplers.push_back(s.desc);
		for (size_t i = 0; i < s.tokens.size(); ++i)
			p.stateUses.push_back(StateUse{-1, -1, static_cast<int32_t>(desc.samplers.size() - 1),
										   static_cast<uint32_t>(i), s.tokens[i]});
	}
	SelectionValues defaults;
	desc.defaults(defaults);
	// every state value evaluates (both branches of a ?: are evaluated): an error names the line it was written on;
	// a blend factor that reads the target cannot be A + dst * B
	for (const StateUse &u : p.stateUses)
	{
		const StateValue &v = u.sampler >= 0 ? desc.samplers[u.sampler].states[u.index]
											 : desc.techniques[u.technique].passes[u.pass].states[u.index];
		eastl::string	  text, why;
		if (v.state == "Targets")
		{
			PassDesc  probe;
			PassState state;
			probe.states.push_back(v);
			desc.resolve(probe, defaults, state, why);
			for (const eastl::string &name : state.targets)
			{
				bool found = false;
				for (const TargetSetDesc &t : desc.targetSets)
					found = found || t.name == name;
				if (!found)
					return p.failAt(u.token, "no TargetSet " + name);
			}
			continue;
		}
		if (!desc.evaluate(v.state.c_str(), v.value, defaults, text, why))
			return p.failAt(u.token, why);
		if (isNumberText(text))
		{
			const BlendFactor factor = blendFactorOf(uint32_t(atoi(text.c_str())));
			const bool		  destination = v.state == "DestBlend" || v.state == "DestBlendAlpha";
			const bool		  source = v.state == "SrcBlend" || v.state == "SrcBlendAlpha";
			const bool		  needsDestinationAlpha = factor == BlendFactor::DestAlpha ||
													  factor == BlendFactor::InvDestAlpha ||
													  factor == BlendFactor::SrcAlphaSat;
			const bool needsDestinationColor = factor == BlendFactor::DestColor || factor == BlendFactor::InvDestColor;
			if (((source || destination) && needsDestinationAlpha) || (destination && needsDestinationColor))
				return p.failAt(u.token, v.state + " = " + v.value +
											 ": a factor that reads the target cannot be written as A + dst * B");
		}
	}
	for (SamplerDesc &s : desc.samplers)
	{
		if (const ParamDesc *t = desc.findParam(s.texture.c_str()))
			s.dim = t->dim == TextureDim::None ? TextureDim::Tex2D : t->dim;
		for (const StateValue &v : s.states)
			markState(v.value);
	}
	bool screenLoader = false; // a compute or Screen pass: screen record loader is written too
	for (TechniqueDesc &t : desc.techniques)
		for (PassDesc &pass : t.passes)
		{
			screenLoader = screenLoader || !pass.shader(Stage::Compute).empty();
			for (const StateValue &v : pass.states)
			{
				markState(v.value);
				for (const char *data : C_DATA_STATES)
					if (v.state == data && namesParam(desc, v.value))
						desc.materialHeader = true;
			}
			// at every value of the selections the pixel stage depends on (a blend from the material at one value only
			// still needs the material header)
			eastl::vector<SelectionValues> all;
			combinations(desc, pass, Stage::Pixel, all);
			if (all.empty())
				all.push_back(defaults);
			for (const SelectionValues &values : all)
			{
				PassState state;
				if (!desc.resolve(pass, values, state, error))
					return error = desc.source + ": error: " + t.name + "/" + pass.name + ": " + error, false;
				// a full-screen pass: drawn from its own vertices (the full-screen triangle), never from the pools
				if (state.screen && (state.meshlets || state.poolVertices || !pass.shader(Stage::Mesh).empty()))
					return error =
							   desc.source + ": error: " + t.name + "/" + pass.name +
							   ": Screen = true; is a full-screen pass drawn from its own vertices, not as meshlets "
							   "or from the pools",
						   false;
				screenLoader = screenLoader || state.screen;
				eastl::string blend[5];
				if (state.blend == BlendMode::DualSource && !constantBlend(state, blend))
					desc.materialHeader = true; // the blend comes from the material record
				if (state.alphaTest && pass.pixelOwnOutputs)
					return error = desc.source + ": error: " + t.name + "/" + pass.name +
								   ": AlphaTest needs a pixel function that returns one colour",
						   false;
				// dual-source blending writes one render target: a blended pass draws into a set of one colour
				if (state.blend == BlendMode::DualSource)
					for (const eastl::string &name : state.targets)
						for (const TargetSetDesc &ts : desc.targetSets)
							if (ts.name == name && ts.colors.size() > 1)
								return error = desc.source + ": error: " + t.name + "/" + pass.name + ": it blends " +
											   "into " + name +
											   ", which has several colour targets: dual-source blending writes one "
											   "(use a set with one colour target, such as TG_SceneBlend)",
									   false;
			}
		}

	auto finishBody = [&](const eastl::string &records)
	{
		// the body: constructs blanked (newlines and directives stay), the parameters' statics and registers written
		// in, the records and their loader at the end (the generated entries follow)
		body = preprocessed;
		for (const auto &range : p.removed)
		{
			bool lineStart = range.first == 0 || body[range.first - 1] == '\n';
			for (uint32_t i = range.first; i < range.second; ++i)
			{
				if (body[i] == '\n')
				{
					lineStart = true;
					continue;
				}
				if (lineStart && body[i] == '#')
				{
					while (i < range.second && body[i] != '\n')
						++i;
					--i;
					continue;
				}
				lineStart = false;
				body[i] = ' ';
			}
		}
		eastl::vector<eastl::pair<uint32_t, eastl::string>> inserts = p.inserted;
		eastl::sort(inserts.begin(), inserts.end(),
					[](const eastl::pair<uint32_t, eastl::string> &a, const eastl::pair<uint32_t, eastl::string> &b)
					{ return a.first > b.first; });
		for (const auto &insert : inserts)
		{
			// over the blanks of the construct it replaces where they suffice: the columns after it stay
			size_t room = 0;
			while (insert.first + room < body.size() && body[insert.first + room] == ' ')
				++room;
			if (room >= insert.second.size())
				body.replace(insert.first, insert.second.size(), insert.second);
			else
				body.insert(insert.first, insert.second);
		}
		body += records;
		return true;
	};
	if (policy.entries == EntryModel::Authored)
	{
		if (!desc.params.empty())
			return error = desc.source + ": authored shader entries declare their own HLSL buffers and resources",
				   false;
		for (const TechniqueDesc &technique : desc.techniques)
			for (const PassDesc &pass : technique.passes)
			{
				for (const ShaderRef &shader : pass.shaders)
					if (!shader.args.empty())
						return error = desc.source +
									   ": authored shader entries take HLSL shader inputs, not compile arguments",
							   false;
				if (pass.poolVertices || (pass.meshlets && pass.shader(Stage::Mesh).empty()))
					return error = desc.source + ": authored shader entries do not generate pool or meshlet stages",
						   false;
			}
		desc.materialHeader = false;
		desc.layoutHash = fnv(eastl::string("authored shader entries"));
		return finishBody({});
	}

	const ShaderRecordAbi &abi = *m_recordAbi;
	// A semantic must be derived (record ABI), provided or per-draw (vocabulary.hpp tables), or declared by the
	// prelude. Any other semantic is treated as a typo and rejected at its declaration, so a hot reload with
	// ": WROLD" fails instead of leaving the field zero.
	for (size_t i = 0; i < desc.params.size(); ++i)
	{
		const ParamDesc &param = desc.params[i];
		if (param.semantic.empty() || p.standard[i])
			continue;
		const eastl::string sem = upper(param.semantic);
		bool				known = false;
		for (size_t d = 0; d < abi.derivedCount; ++d)
			known = known || sem == abi.derived[d].semantic;
		known = known || providedSemantic(sem.c_str());
		// Per-draw semantics are ordinary fields of the instance block.
		if (instanceSemantic(sem.c_str()) != nullptr)
		{
			if (param.rate != Rate::Instance)
				return error = param.where + ": error: " + param.name + " : " + param.semantic +
							   ": the renderer fills it per draw - a parameter of the instance block",
					   false;
			known = true;
		}
		for (size_t j = 0; j < desc.params.size() && !known; ++j)
			known = p.standard[j] && upper(desc.params[j].semantic) == sem;
		if (!known)
			return error = param.where + ": error: " + param.name + " : " + param.semantic +
						   ": no such semantic (the renderer's: the prelude's frame block, the derived ones, " +
						   "the provided and the per-draw ones the caller registered)",
				   false;
	}
	// A texture default (Texture2D t_Specular = NAME;) names one of the host's built-in textures
	// (setTextureDefaults), bound when the material sets none.
	uint32_t				 builtinCount = 0;
	const char *const *const builtins = textureDefaults(builtinCount);
	for (const ParamDesc &param : desc.params)
	{
		if (!isResource(param.type) || param.defaultValue.empty())
			continue;
		bool known = false;
		for (uint32_t i = 0; i < builtinCount; ++i)
			known = known || param.defaultValue == builtins[i];
		if (!known)
		{
			eastl::string list;
			for (uint32_t i = 0; i < builtinCount; ++i)
				list += eastl::string(i ? ", " : "") + builtins[i];
			return error = param.where + ": error: " + param.name + " = " + param.defaultValue +
						   ": a texture's default is one of the caller's built-in textures (" + list + ")",
				   false;
		}
	}
	// BackFaceCull: a constant or one bool parameter of the material record (what the generated entries read)
	for (const TechniqueDesc &t : desc.techniques)
		for (const PassDesc &pass : t.passes)
			for (const StateValue &v : pass.states)
			{
				if (v.state != "BackFaceCull" || v.value == "true" || v.value == "false" || v.value == "0" ||
					v.value == "1")
					continue;
				const ParamDesc *param = desc.findParam(v.value.c_str());
				if (!param || param->rate != Rate::Material || param->type != "bool")
					return error = desc.source + ": error: " + t.name + "/" + pass.name +
								   ": BackFaceCull = " + v.value +
								   ": a constant or the name of a bool parameter of the material block",
						   false;
			}
	// derivedCode: every derived parameter the code reads (record loader); screenDerivedCode: those not from the
	// instance header (screen record loader: a screen entry that reads no instance data has none)
	eastl::string derivedCode, screenDerivedCode;
	auto		  instanceDerived = [&](const eastl::string &semantic)
	{
		const eastl::string sem = upper(semantic);
		for (size_t d = 0; d < abi.derivedCount; ++d)
			if (sem == abi.derived[d].semantic)
				return abi.derived[d].instance;
		return false;
	};
	for (ParamDesc &param : desc.params)
	{
		const eastl::string sem = upper(param.semantic);
		for (size_t i = 0; i < abi.derivedCount; ++i)
		{
			const DerivedBinding &d = abi.derived[i];
			if (sem != d.semantic)
				continue;
			const bool			read = (param.use & C_USE_RECORD) != 0;
			const eastl::string type =
				recordType(param.type) == "uint" && param.type == "bool" ? param.type : recordType(param.type);
			if (!strcmp(d.type, "Texture2D"))
			{
				// a scene texture of the view: any element type (depth float, normals float4, visibility uint ...)
				if ((param.type != "Texture2D" && param.type.compare(0, 10, "Texture2D<") != 0) || param.count)
					return error = param.where + ": error: " + param.name + " : " + param.semantic +
								   " is the view's scene texture, a Texture2D, not " + param.type,
						   false;
				if (read)
				{
					appendf(derivedCode, "\t%s = ResourceDescriptorHeap[%s];\n", param.name.c_str(), d.expression);
					appendf(screenDerivedCode, "\t%s = ResourceDescriptorHeap[%s];\n", param.name.c_str(),
							d.expression);
				}
			}
			else if (*d.type)
			{
				const bool intLike = !strcmp(d.type, "int") && (type == "int" || type == "uint");
				if ((type != d.type && !intLike) || param.count)
					return error = param.where + ": error: " + param.name + " : " + param.semantic +
								   " is the renderer's " + d.type + ", not " + param.type,
						   false;
				if (read)
					appendf(derivedCode, "\t%s = (%s)(%s);\n", param.name.c_str(), param.type.c_str(), d.expression);
				if (read && !d.instance)
					appendf(screenDerivedCode, "\t%s = (%s)(%s);\n", param.name.c_str(), param.type.c_str(),
							d.expression);
			}
			else if (d.write != nullptr)
			{
				eastl::vector<DerivedMember> members;
				if (const Struct *structure = findStruct(param.type))
					for (const Member &member : structure->members)
						members.push_back(DerivedMember{member.name.c_str(), componentsOf(member.type)});
				if (!d.write(param, members.data(), members.size(), read ? &derivedCode : nullptr, error) ||
					(read && !d.instance && !d.write(param, members.data(), members.size(), &screenDerivedCode, error)))
					return false;
			}
			else
				return error = param.where + ": error: no writer for derived semantic " + param.semantic, false;
			param.use = static_cast<uint8_t>((param.use & ~C_USE_RECORD) | C_USE_DERIVED);
			break;
		}
	}
	// another name of a shared parameter: reading it reads the shared one
	eastl::string aliasCode, screenAliasCode;
	for (ParamDesc &param : desc.params)
		if (!param.alias.empty() && (param.use & C_USE_RECORD))
		{
			param.use = static_cast<uint8_t>(param.use & ~C_USE_RECORD);
			ParamDesc *target = const_cast<ParamDesc *>(desc.findParam(param.alias.c_str()));
			target->use |= C_USE_RECORD;
			appendf(aliasCode, "\t%s = %s;\n", param.name.c_str(), param.alias.c_str());
			if (target->rate != Rate::Instance && !instanceDerived(target->semantic))
				appendf(screenAliasCode, "\t%s = %s;\n", param.name.c_str(), param.alias.c_str());
		}

	// the records: the effect's frame record, its instance data (the range of the effect-data pool the instance
	// header points to), its material record (the header, resources, samplers, then the values, in declaration order)
	eastl::vector<Field> fields[C_RATES];
	if (desc.materialHeader)
	{
		desc.materialHeaderSize = abi.materialHeader.size;
		fields[2].push_back(Field{abi.materialHeader.member, abi.materialHeader.type, 0, 0, desc.materialHeaderSize});
	}
	for (int pass = 0; pass < 3; ++pass)
	{
		for (ParamDesc &param : desc.params)
		{
			const int wanted = isResource(param.type) ? 0 : 2;
			if (!(param.use & C_USE_RECORD) || wanted != pass)
				continue;
			Field f;
			f.name = param.name;
			f.type = recordType(param.type);
			f.count = param.count;
			f.size = sizeOf(f.type, m_structs, desc, error, abi.materialHeader) * (param.count ? param.count : 1);
			if (f.size == 0)
				return error = param.where + ": error: " + param.name + ": " + error, false;
			fields[static_cast<uint32_t>(param.rate)].push_back(f);
		}
		if (pass == 1)
			for (const SamplerDesc &s : desc.samplers)
				if (usedName(p.used, s.name))
					fields[2].push_back(Field{s.name, "uint", 0, 0, 4});
	}
	eastl::string records = "\n#line 1 \"<records " + desc.name + ">\"\n";
	eastl::string hashText = "native blend factors and sampler modes;";
	for (uint32_t r = 0; r < C_RATES; ++r)
	{
		uint32_t offset = 0;
		for (Field &f : fields[r])
		{
			f.offset = offset;
			offset += f.size;
			appendf(hashText, "%u %s %s %u %u;", r, f.name.c_str(), f.type.c_str(), f.count, f.offset);
			if (ParamDesc *param = const_cast<ParamDesc *>(desc.findParam(f.name.c_str())))
			{
				param->offset = static_cast<int32_t>(f.offset);
				param->size = f.size;
			}
			for (SamplerDesc &s : desc.samplers)
				if (s.name == f.name)
					s.offset = static_cast<int32_t>(f.offset);
		}
		desc.recordSize[r] = offset;
		if (fields[r].empty())
			continue;
		appendf(records, "struct %s%s\n{\n", abi.rates[r].typePrefix, effectName);
		for (const Field &f : fields[r])
		{
			appendf(records, "\t%s %s", f.type.c_str(), f.name.c_str());
			if (f.count)
				appendf(records, "[%u]", f.count);
			records += ";\n";
		}
		records += "};\n";
	}
	for (const StructDesc &s : desc.structs)
	{
		appendf(hashText, "struct %s %u;", s.name.c_str(), s.size);
		for (const StructDesc::Field &f : s.fields)
			appendf(hashText, "%s %s %u %u;", f.name.c_str(), f.type.c_str(), f.count, f.offset);
	}
	for (const RegisterDesc &r : desc.registers)
		appendf(hashText, "reg %s %c%u %u;", r.name.c_str(), r.kind, r.index, r.space);
	desc.layoutHash = fnv(hashText);
	m_records = true;
	if (desc.materialHeader)
		appendf(records, "static %s %s;\n", abi.materialHeader.type, abi.materialHeader.variable);
	// the loader: the renderer's headers, then this effect's records, into the parameters' statics
	// (the record loads and the field assignments of each rate: the same text in both loaders)
	eastl::string loadRecord[C_RATES], assign[C_RATES];
	for (uint32_t r = 0; r < C_RATES; ++r)
		if (!fields[r].empty())
		{
			const RateRecordAbi &record = abi.rates[r];
			appendf(loadRecord[r], "\tconst %s%s %s = %s<%s%s>(%s);\n", record.typePrefix, effectName, record.local,
					abi.loadRecord, record.typePrefix, effectName, record.loadArguments);
		}
	for (uint32_t r = 0; r < C_RATES; ++r)
		for (const Field &f : fields[r])
		{
			const char		*rec = abi.rates[r].local;
			const ParamDesc *param = desc.findParam(f.name.c_str());
			if (desc.materialHeader && r == uint32_t(Rate::Material) && f.name == abi.materialHeader.member)
				appendf(assign[r], "\t%s = %s.%s;\n", abi.materialHeader.variable, rec, f.name.c_str());
			// NonUniformResourceIndex: the pool and meshlet paths load the records per instance, so the pixels of
			// one wave may belong to several instances and materials. Without it, D3D12 GPU-based validation reports
			// "non-uniform resource index used when uniform resource index was expected".
			else if (!param) // a sampler
				appendf(assign[r], "\t%s = SamplerDescriptorHeap[NonUniformResourceIndex(%s.%s)];\n", f.name.c_str(),
						rec, f.name.c_str());
			else if (isResource(param->type))
				appendf(assign[r], "\t%s = ResourceDescriptorHeap[NonUniformResourceIndex(%s.%s)];\n", f.name.c_str(),
						rec, f.name.c_str());
			else if (param->type.compare(0, 4, "bool") == 0)
				appendf(assign[r], "\t%s = %s.%s != 0;\n", f.name.c_str(), rec, f.name.c_str());
			else
				appendf(assign[r], "\t%s = %s.%s;\n", f.name.c_str(), rec, f.name.c_str());
		}
	records += abi.recordLoaderStart;
	records += loadRecord[0] + loadRecord[1] + loadRecord[2] + assign[0] + assign[1] + assign[2];
	records += derivedCode + aliasCode + "}\n"; // an alias may name a derived one
	// The screen loader serves entries that read no instance data: compute entries and the entries of a Screen pass.
	// It loads the frame header and the effect's frame and material (or dispatch) records. It loads no instance
	// header, instance data or instance-derived value; the instance header and index stay zero and the code reads
	// neither.
	if (screenLoader)
	{
		records += abi.screenLoaderStart;
		records += loadRecord[0] + loadRecord[2] + assign[0] + assign[2];
		records += screenDerivedCode + screenAliasCode + "}\n";
	}

	return finishBody(records);
}

//----------------------------------------------------------------------------------------------------------------------
namespace
{
// the value of an argument for the values: a literal as it is, a selection's value
bool argumentValue(const EffectDesc &desc, const eastl::string &arg, const SelectionValues &values, eastl::string &out)
{
	const int32_t s = desc.findSelection(arg.c_str());
	if (s < 0)
	{
		out = arg;
		return true;
	}
	const int32_t v = static_cast<size_t>(s) < values.size() ? values[s] : desc.selections[s].defaultValue;
	if (desc.selections[s].isBool)
		out = v ? "true" : "false";
	else
	{
		char text[16];
		snprintf(text, sizeof text, "%d", v);
		out = text;
	}
	return true;
}

} // namespace

bool variantOf(const EffectDesc &desc, const PassDesc &pass, Stage stage, const SelectionValues &values, Path path,
			   eastl::string &variant, eastl::string &entry, eastl::string &error, const ShaderRecordAbi *abi)
{
	for (size_t i = 0; i < values.size() && i < desc.selections.size(); ++i)
	{
		bool listed = false;
		for (const SelectionDesc::Value &v : desc.selections[i].values)
			listed = listed || v.value == values[i];
		if (!listed)
		{
			char text[160];
			snprintf(text, sizeof text, "%s = %d is not one of its declared values", desc.selections[i].name.c_str(),
					 values[i]);
			error = text;
			return false;
		}
	}
	// the meshlet path of a pass drawn from its vertex function: generated mesh and amplification entries
	const bool generated = (stage == Stage::Mesh || stage == Stage::Amplification) && pass.shader(stage).empty();
	if (generated)
		path = Path::Meshlets;
	if (generated && !pass.meshlets)
	{
		error = "the pass has no " + eastl::string(stage == Stage::Mesh ? "mesh" : "amplification") +
				" shader and is not drawn as meshlets";
		return false;
	}
	if (stage == Stage::Vertex && path == Path::Meshlets)
	{
		error = "the meshlet path has no vertex shader";
		return false;
	}
	// a pass with its own mesh shader is drawn as meshlets only: its pixel shader reads the instance the mesh shader
	// passes (.ix)
	if (!pass.shader(Stage::Mesh).empty())
		path = Path::Meshlets;
	if ((path == Path::Vertices && stage != Stage::Compute && !pass.shader(Stage::Vertex).empty() && !pass.vertices) ||
		(path == Path::Pool && !pass.poolVertices) ||
		(path == Path::Meshlets && !pass.meshlets && pass.shader(Stage::Mesh).empty()))
	{
		error = eastl::string("the pass is not drawn ") + (path == Path::Pool ? "from the pools" : "as meshlets");
		return false;
	}
	const ShaderRef &ref = generated ? pass.shader(Stage::Vertex) : pass.shader(stage);
	if (ref.empty())
	{
		error = "the pass has no shader of this stage";
		return false;
	}
	// the values the text depends on: the arguments, then the selections the code reads by name
	eastl::string args;
	auto		  add = [&](const eastl::string &v)
	{
		args += (args.empty() ? "" : "_") + (v == "true" ? eastl::string("1") : v == "false" ? eastl::string("0") : v);
	};
	for (const eastl::string &a : ref.args)
	{
		eastl::string v;
		argumentValue(desc, a, values, v);
		add(v);
	}
	for (const eastl::string &r : ref.reads)
	{
		eastl::string v;
		argumentValue(desc, r, values, v);
		add(v);
	}
	if (desc.entryModel == EntryModel::Authored)
	{
		entry = ref.function;
		variant = eastl::string("native_") + stageName(stage) + "_" + entry;
		if (!args.empty())
			variant += "." + args;
		if (desc.policyHash != 0)
			appendf(variant, ".p%016llx", static_cast<unsigned long long>(desc.policyHash));
		return true;
	}
	if (abi == nullptr || abi->entries == nullptr)
		return error = "renderer-record shaders require the caller's entry ABI", false;
	const RendererEntryAbi &entryAbi = *abi->entries;
	const ShaderRef		   &ps = pass.shader(Stage::Pixel);
	const eastl::string		psName = ps.empty() ? eastl::string("none") : ps.function;
	if (stage == Stage::Vertex)
		entry = (path == Path::Pool ? entryAbi.poolPrefix : entryAbi.vertexPrefix) + ref.function + "_" + psName;
	else if (stage == Stage::Pixel)
		entry = entryAbi.pixelPrefix + ref.function;
	else if (stage == Stage::Mesh && generated)
		entry = entryAbi.meshPrefix + ref.function + "_" + psName;
	else if (stage == Stage::Amplification && generated)
		entry =
			entryAbi.amplificationEntry; // one for every pass, but a pass whose back faces the material culls (below)
	else if (stage == Stage::Compute)
		entry = entryAbi.computePrefix + ref.function;
	else
		entry = ref.function; // mesh and amplification functions written by the author are the entries as written
	variant = entry;
	if (stage == Stage::Amplification && generated)
	{
		args.clear(); // the same for every pass: tasks, instances, meshlets
		// ... unless the material decides the back faces (BackFaceCull): the cone test asks the instance's material
		PassState s;
		if (!desc.resolve(pass, values, s, error))
			return false;
		if (s.backFaceCull != "0")
			args = "cull_" + s.backFaceCull;
	}
	if (!args.empty())
		variant += "." + args;
	// the vertex and pixel entries of a Screen pass may load other records than the same function's in another pass
	// (screen record loader): their own files (.sc)
	if ((stage == Stage::Vertex || stage == Stage::Pixel) && path == Path::Vertices)
	{
		PassState s;
		if (!desc.resolve(pass, values, s, error))
			return false;
		if (s.screen)
			variant += ".sc";
	}
	if (stage == Stage::Pixel)
	{
		if (path != Path::Vertices)
			variant += ".ix"; // the records by the instance the pool or mesh entry hands over
		PassState s;
		if (!desc.resolve(pass, values, s, error))
			return false;
		if (s.alphaTest)
			variant += ".at" + (isNumberText(s.alphaRef) ? s.alphaRef : eastl::string("m"));
		eastl::string blend[5];
		if (s.blend == BlendMode::None)
			variant += ".bn";
		else if (s.blend == BlendMode::DualSource)
		{
			blendOperands(s, blend);
			eastl::string text = ".b";
			for (int i = 0; i < 5; ++i)
				text += (i ? "-" : "") + (isNumberText(blend[i]) ? blend[i] : eastl::string("m"));
			variant += text == ".bm-m-m-m-m" ? eastl::string(".bm") : text;
		}
		// the back faces the material culls: the vertex and pool paths' pixel entry discards them (.cf)
		if (path != Path::Meshlets && s.backFaceCull != "0")
			variant += ".cf_" + s.backFaceCull;
	}
	if (stage == Stage::Mesh && generated)
	{
		// the generated mesh entry culls them per primitive (.cf)
		PassState s;
		if (!desc.resolve(pass, values, s, error))
			return false;
		if (s.backFaceCull != "0")
			variant += ".cf_" + s.backFaceCull;
	}
	return true;
}

void combinations(const EffectDesc &desc, const PassDesc &pass, Stage stage, eastl::vector<SelectionValues> &out)
{
	out.clear();
	eastl::vector<int32_t> used;
	const bool		 generated = (stage == Stage::Mesh || stage == Stage::Amplification) && pass.shader(stage).empty();
	const ShaderRef &ref = generated ? pass.shader(Stage::Vertex) : pass.shader(stage);
	auto			 use = [&](const eastl::string &name)
	{
		const int32_t s = desc.findSelection(name.c_str());
		if (s >= 0 && eastl::find(used.begin(), used.end(), s) == used.end())
			used.push_back(s);
	};
	if (stage != Stage::Amplification || !generated)
	{
		for (const eastl::string &a : ref.args)
			use(a);
		for (const eastl::string &r : ref.reads)
			use(r);
	}
	if (stage == Stage::Pixel)
		for (const StateValue &v : pass.states)
			for (const char *data : C_DATA_STATES)
				if (v.state == data)
					selectionsIn(desc, v.value, used);
	// each one's values, within the pass's pins (a pinned-out value is never drawn: not built either)
	eastl::vector<eastl::vector<int32_t>> choices;
	for (int32_t s : used)
	{
		eastl::vector<int32_t> &c = choices.push_back();
		for (const SelectionDesc::Value &v : desc.selections[s].values)
			c.push_back(v.value);
		for (const PassDesc::Pin &pin : pass.pins)
			if (pin.selection == desc.selections[s].name)
				c = pin.values;
	}
	SelectionValues values;
	desc.defaults(values);
	eastl::vector<uint32_t> at(used.size(), 0);
	for (;;)
	{
		for (size_t i = 0; i < used.size(); ++i)
			values[used[i]] = choices[i][at[i]];
		out.push_back(values);
		size_t i = 0;
		while (i < used.size() && ++at[i] == choices[i].size())
			at[i++] = 0;
		if (i == used.size())
			break;
	}
}

//----------------------------------------------------------------------------------------------------------------------
namespace
{
eastl::string valueText(const SelectionDesc &s, int32_t v)
{
	if (s.isBool)
		return v ? "true" : "false";
	for (const SelectionDesc::Value &named : s.values)
		if (named.value == v && !named.name.empty())
			return named.name;
	char text[16];
	snprintf(text, sizeof text, "%d", v);
	return text;
}
} // namespace

eastl::string pinText(const EffectDesc &desc, const PassDesc &pass)
{
	eastl::string out;
	for (const PassDesc::Pin &pin : pass.pins)
	{
		const int32_t s = desc.findSelection(pin.selection.c_str());
		if (s < 0)
			continue;
		out += (out.empty() ? "" : " ") + eastl::string("Pin ") + pin.selection + " = ";
		if (pin.values.size() == 1)
			out += valueText(desc.selections[s], pin.values[0]) + ";";
		else
		{
			out += "[";
			for (size_t i = 0; i < pin.values.size(); ++i)
				out += (i ? ", " : "") + valueText(desc.selections[s], pin.values[i]);
			out += "];";
		}
	}
	return out;
}

bool pinned(const EffectDesc &desc, const PassDesc &pass, const SelectionValues &values, eastl::string &which)
{
	for (const PassDesc::Pin &pin : pass.pins)
	{
		const int32_t s = desc.findSelection(pin.selection.c_str());
		if (s < 0 || static_cast<size_t>(s) >= values.size())
			continue;
		if (eastl::find(pin.values.begin(), pin.values.end(), values[s]) == pin.values.end())
		{
			which = pin.selection + " = " + valueText(desc.selections[s], values[s]);
			return false;
		}
	}
	return true;
}

void passSelections(const EffectDesc &desc, const PassDesc &pass, eastl::vector<int32_t> &used)
{
	used.clear();
	for (const StateValue &v : pass.states)
		if (v.state != "Targets")
			selectionsIn(desc, v.value, used);
	for (uint32_t st = 0; st < C_STAGES; ++st)
	{
		for (const eastl::string &a : pass.shaders[st].args)
			selectionsIn(desc, a, used);
		for (const eastl::string &r : pass.shaders[st].reads)
			selectionsIn(desc, r, used);
	}
	eastl::sort(used.begin(), used.end());
}

void geometryPaths(const PassDesc &pass, eastl::vector<Path> &out)
{
	out.clear();
	if (!pass.shader(Stage::Compute).empty())
		out.push_back(Path::Vertices); // a dispatch: no geometry
	else if (!pass.shader(Stage::Mesh).empty())
		out.push_back(Path::Meshlets);
	else
	{
		if (pass.meshlets)
			out.push_back(Path::Meshlets);
		if (pass.poolVertices)
			out.push_back(Path::Pool);
		if (pass.vertices || out.empty())
			out.push_back(Path::Vertices);
	}
}

void pipelines(const EffectDesc &desc, eastl::vector<PipelineDesc> &out, const ShaderRecordAbi *abi)
{
	out.clear();
	// Default target set: a draw pass without a Targets state uses the set named TG_Scene when the effect declares
	// one (see TargetSetDesc in effect_desc.hpp).
	const TargetSetDesc *scene = nullptr;
	for (const TargetSetDesc &t : desc.targetSets)
		if (t.name == "TG_Scene")
			scene = &t;
	for (const TechniqueDesc &t : desc.techniques)
		for (const PassDesc &pass : t.passes)
		{
			// the selections this pass reads: its states (pipeline and data) and the code of its shaders
			eastl::vector<int32_t> used;
			passSelections(desc, pass, used);
			// each one's values, within the pins
			eastl::vector<eastl::vector<int32_t>> choices;
			for (int32_t s : used)
			{
				eastl::vector<int32_t> &c = choices.push_back();
				for (const SelectionDesc::Value &v : desc.selections[s].values)
					c.push_back(v.value);
				for (const PassDesc::Pin &pin : pass.pins)
					if (pin.selection == desc.selections[s].name)
						c = pin.values;
			}
			const bool		compute = !pass.shader(Stage::Compute).empty();
			SelectionValues values;
			desc.defaults(values);
			eastl::vector<uint32_t> at(used.size(), 0);
			for (;;)
			{
				for (size_t i = 0; i < used.size(); ++i)
					values[used[i]] = choices[i][at[i]];
				PassState	  state;
				eastl::string error;
				if (desc.resolve(pass, values, state, error))
				{
					eastl::vector<Path> paths;
					geometryPaths(pass, paths);
					eastl::vector<TargetSetDesc> sets;
					for (const eastl::string &name : state.targets)
						for (const TargetSetDesc &ts : desc.targetSets)
							if (ts.name == name)
								sets.push_back(ts);
					if (sets.empty())
						sets.push_back(compute || !scene ? TargetSetDesc() : *scene);
					for (Path path : paths)
						for (const TargetSetDesc &ts : sets)
						{
							PipelineDesc pd;
							pd.technique = t.name;
							pd.pass = pass.name;
							pd.path = path;
							pd.values = values;
							pd.state = state;
							pd.targets = ts;
							pd.precache = state.precache;
							eastl::string entry;
							bool		  ok = true;
							if (compute)
								ok = variantOf(desc, pass, Stage::Compute, values, path, pd.shaders[4], entry, error,
											   abi);
							else if (path == Path::Meshlets)
							{
								ok = variantOf(desc, pass, Stage::Mesh, values, path, pd.shaders[2], entry, error, abi);
								if (ok && (pass.meshlets || !pass.shader(Stage::Amplification).empty()))
									ok = variantOf(desc, pass, Stage::Amplification, values, path, pd.shaders[3], entry,
												   error, abi);
							}
							else
								ok = variantOf(desc, pass, Stage::Vertex, values, path, pd.shaders[0], entry, error,
											   abi);
							if (ok && !compute && !pass.shader(Stage::Pixel).empty())
								ok =
									variantOf(desc, pass, Stage::Pixel, values, path, pd.shaders[1], entry, error, abi);
							if (!ok)
								continue;
							// the canonical key: everything the pipeline is made of, in a fixed order
							appendf(pd.key, "%s %s/%s path=%s targets=%s:", desc.name.c_str(), t.name.c_str(),
									pass.name.c_str(), path == Path::Vertices ? "vertices" : pathName(path),
									ts.name.c_str());
							for (const eastl::string &c : ts.colors)
								pd.key += c + ",";
							appendf(pd.key,
									"%s samples=%u topology=%d cull=%d depth=%d,%d,%d mask=%u bias=%d,%g blend=%d",
									ts.depth.c_str(), pd.samples, static_cast<int>(state.topology),
									static_cast<int>(state.cullMode), state.depthTest, state.depthWrite,
									static_cast<int>(state.depthFunc), state.colorWriteMask, state.depthBias,
									state.slopeScaledDepthBias, static_cast<int>(state.blend));
							for (uint32_t st = 0; st < C_STAGES; ++st)
								if (!pd.shaders[st].empty())
									appendf(pd.key, " %u=%s", st, pd.shaders[st].c_str());
							pd.hash = fnv(pd.key);
							bool seen = false;
							for (const PipelineDesc &other : out)
								seen = seen || other.hash == pd.hash;
							if (!seen)
								out.push_back(pd);
						}
				}
				size_t i = 0;
				while (i < used.size() && ++at[i] == choices[i].size())
					at[i++] = 0;
				if (i == used.size())
					break;
			}
		}
}

//----------------------------------------------------------------------------------------------------------------------
namespace
{
struct LinkMember
{
	eastl::string type;		// float3 ...
	eastl::string name;		// v_IN_Color
	eastl::string semantic; // as written, without _centroid
	eastl::string interp;	// "centroid " ...
	eastl::string dest;		// IN.Color (a struct member) or the parameter's name
	eastl::string destType; // the parameter's type (a parameter that is not a struct)
	eastl::string norm;		// COLOR0
};

// Convert a host-provided pool input to the authored component type.
eastl::string poolInput(const ShaderRecordAbi *abi, const eastl::string &norm, const eastl::string &type)
{
	const uint32_t k = componentsOf(type);
	if (abi == nullptr || k == 0)
		return {};
	for (size_t i = 0; i < abi->poolInputCount; ++i)
	{
		const PoolInputBinding &binding = abi->poolInputs[i];
		if (norm != binding.semantic)
			continue;
		eastl::string expr = binding.expression;
		if (k < binding.components)
			expr += "." + eastl::string("xyzw").substr(0, k);
		else if (k > binding.components)
			expr = vectorType(type, k) + "(" + expr + ", " + binding.extraComponent + ")";
		return expr;
	}
	return {};
}

} // namespace

bool Effect::meshletInputs(const Function &f) const
{
	for (const Member &m : f.params)
	{
		if (hasWord(m.modifiers, "uniform"))
			continue;
		if (!m.semantic.empty())
		{
			if (poolInput(m_recordAbi, normSemantic(m.semantic), m.type).empty())
				return false;
			continue;
		}
		const Struct *s = findStruct(m.type);
		if (!s)
			continue; // a uniform without the keyword
		for (const Member &sm : s->members)
			if (sm.semantic.empty() || poolInput(m_recordAbi, normSemantic(sm.semantic), sm.type).empty())
				return false;
	}
	return true;
}

bool Effect::entry(const PassDesc &pass, Stage stage, const SelectionValues &values, Path path, Entry &out,
				   eastl::string &error) const
{
	out = Entry();
	if (!variantOf(desc, pass, stage, values, path, out.variant, out.name, error, m_recordAbi))
		return false;
	const bool generated = (stage == Stage::Mesh || stage == Stage::Amplification) && pass.shader(stage).empty();
	if (generated)
		path = Path::Meshlets;
	const ShaderRef &ref = generated ? pass.shader(Stage::Vertex) : pass.shader(stage);
	const Function	*f = findFunction(ref.function);
	if (!f)
	{
		error = desc.source + ": error: no function " + ref.function;
		return false;
	}
	// the selections as the code reads them by name: this variant's values (the others' do not reach its code)
	for (const SelectionDesc &s : desc.selections)
	{
		eastl::string v;
		argumentValue(desc, s.name, values, v);
		appendf(out.prefix, "static const %s %s = %s;\n", s.isBool ? "bool" : "int", s.name.c_str(), v.c_str());
	}
	// the names of the int selections' values, so the code compares by name (if (g_CullMode == BACK)): each as
	// <selection>_<NAME>, and as the bare NAME where no other selection gives that name another value
	for (const SelectionDesc &s : desc.selections)
		for (const SelectionDesc::Value &named : s.values)
		{
			if (s.isBool || named.name.empty())
				continue;
			appendf(out.prefix, "static const int %s_%s = %d;\n", s.name.c_str(), named.name.c_str(), named.value);
			bool clash = false, first = true;
			for (const SelectionDesc &o : desc.selections)
				for (const SelectionDesc::Value &w : o.values)
					if (w.name == named.name && (&o != &s || &w != &named))
					{
						clash = clash || w.value != named.value;
						first = first && (&o > &s || (&o == &s && &w > &named)); // the first declaration emits it
					}
			if (!clash && first)
				appendf(out.prefix, "static const int %s = %d;\n", named.name.c_str(), named.value);
		}
	if (desc.entryModel == EntryModel::Authored)
		return true;
	const RendererEntryAbi &entryAbi = *m_recordAbi->entries;
	char					where[32];
	snprintf(where, sizeof where, ":%u", f->line);
	const eastl::string at = m_files[f->file] + where + ": error: " + ref.function + ": ";
	eastl::string	   &t = out.text;
	// A screen entry (compute, or the vertex path of a Screen pass) uses the screen loader unless its code reads
	// instance data. Instance data is the instance header or index, a parameter of the instance block, or a value
	// derived from the instance header (WORLD, ALPHA, ...), read by the stage function or any function it calls,
	// prelude functions included.
	bool screen = stage == Stage::Compute;
	if (!screen && path == Path::Vertices && (stage == Stage::Vertex || stage == Stage::Pixel))
	{
		PassState s;
		if (!desc.resolve(pass, values, s, error))
			return false;
		screen = s.screen;
	}
	auto readsInstance = [&](const Function &entryFunction)
	{
		eastl::vector<const Function *> todo, seen;
		todo.push_back(&entryFunction);
		while (!todo.empty())
		{
			const Function *g = todo.back();
			todo.pop_back();
			if (eastl::find(seen.begin(), seen.end(), g) != seen.end())
				continue;
			seen.push_back(g);
			for (const eastl::string &id : g->idents)
			{
				for (size_t i = 0; i < m_recordAbi->instanceIdentifierCount; ++i)
					if (id == m_recordAbi->instanceIdentifiers[i])
						return true;
				if (const ParamDesc *p = desc.findParam(id.c_str()))
				{
					const ParamDesc	   *source = p->alias.empty() ? p : desc.findParam(p->alias.c_str());
					const eastl::string sem = upper(source ? source->semantic : p->semantic);
					bool fromInstance = p->rate == Rate::Instance || (source && source->rate == Rate::Instance);
					for (size_t i = 0; i < m_recordAbi->derivedCount; ++i)
					{
						const DerivedBinding &d = m_recordAbi->derived[i];
						fromInstance = fromInstance || (d.instance && sem == d.semantic);
					}
					if (fromInstance)
						return true;
				}
				for (const Function &h : m_functions)
					if (h.name == id)
						todo.push_back(&h);
			}
		}
		return false;
	};
	const eastl::string load = screen && !readsInstance(*f) ? m_recordAbi->loadScreenEntry
															: m_recordAbi->loadVertexEntry; // the vertex path's records
	if ((stage == Stage::Mesh || stage == Stage::Amplification) && !generated)
	{
		if (!ref.args.empty())
		{
			error = at + "a mesh or amplification function must load its own records and take no compile arguments";
			return false;
		}
		t = "\n";
		return true;
	}
	if (stage == Stage::Amplification)
	{
		PassState state;
		if (!desc.resolve(pass, values, state, error))
			return false;
		if (entryAbi.writeAmplification == nullptr)
			return error = at + "the shader policy has no amplification generator", false;
		appendf(t, "\n#line 1 \"<generated %s %s>\"\n", desc.name.c_str(), out.variant.c_str());
		entryAbi.writeAmplification(out.name.c_str(), state.backFaceCull, t);
		return true;
	}

	// the call: uniform parameters from the arguments, in order
	auto isVarying = [&](const Member &m)
	{
		if (hasWord(m.modifiers, "uniform"))
			return false;
		if (!m.semantic.empty())
			return true;
		const Struct *s = findStruct(m.type);
		if (!s)
			return false;
		for (const Member &sm : s->members)
			if (!sm.semantic.empty())
				return true;
		return false;
	};
	eastl::vector<eastl::string> bound;
	for (const eastl::string &a : ref.args)
	{
		eastl::string v;
		argumentValue(desc, a, values, v);
		bound.push_back(v);
	}
	uint32_t uniforms = 0;
	for (const Member &m : f->params)
	{
		if (hasWord(m.modifiers, "out") || hasWord(m.modifiers, "inout"))
		{
			error = at + "an out parameter: return a struct of outputs instead";
			return false;
		}
		uniforms += isVarying(m) ? 0 : 1;
	}
	if (uniforms != bound.size())
	{
		char text[96];
		snprintf(text, sizeof text, "%u uniform parameters, the technique gives %u arguments", uniforms,
				 static_cast<uint32_t>(bound.size()));
		error = at + text;
		return false;
	}
	if (stage == Stage::Compute)
	{
		// the author's attributes ([numthreads ...]), the records loaded, the function called with its arguments
		eastl::string params, call;
		size_t		  u = 0;
		for (const Member &m : f->params)
		{
			if (!call.empty())
				call += ", ";
			if (!isVarying(m))
			{
				call += bound[u++];
				continue;
			}
			call += m.name;
			params += (params.empty() ? "" : ", ") + m.type + " " + m.name + " : " + m.semantic;
		}
		appendf(t, "\n#line 1 \"<generated %s %s>\"\n%s\nvoid %s(%s)\n{\n", desc.name.c_str(), out.variant.c_str(),
				f->attributes.c_str(), out.name.c_str(), params.c_str());
		t += load;
		appendf(t, "\t%s(%s);\n}\n", f->name.c_str(), call.c_str());
		return true;
	}

	// the link: the pixel function's inputs in its order, the instance index (the pool paths), SV_Position last
	const ShaderRef			 &psRef = pass.shader(Stage::Pixel);
	const Function			 *ps = psRef.empty() ? nullptr : findFunction(psRef.function);
	eastl::vector<LinkMember> link;
	eastl::string			  face, position; // the destinations of the system inputs
	if (ps)
	{
		auto add = [&](const Member &m, const eastl::string &dest, const eastl::string &linkName) -> bool
		{
			const eastl::string norm = normSemantic(m.semantic);
			if (norm.compare(0, 5, "VFACE") == 0 && norm.find_first_not_of("0123456789", 5) == eastl::string::npos)
			{
				error = at + dest + ": VFACE is unsupported; use bool with SV_IsFrontFace";
				return false;
			}
			if (norm == "SV_ISFRONTFACE0")
			{
				if (m.type != "bool")
				{
					error = at + dest + ": SV_IsFrontFace requires bool";
					return false;
				}
				face = dest;
				return true;
			}
			if (norm.compare(0, 4, "VPOS") == 0 && norm.find_first_not_of("0123456789", 4) == eastl::string::npos)
			{
				error = at + dest + ": VPOS is unsupported; use SV_Position (pixel centres at +0.5)";
				return false;
			}
			if (norm == "SV_POSITION0")
			{
				position = dest;
				return true;
			}
			const uint32_t n = componentsOf(m.type);
			if (n == 0)
			{
				error = at + m.name + ": an interpolant is a scalar or a vector";
				return false;
			}
			LinkMember &l = link.push_back();
			l.type = vectorType(m.type, n);
			l.name = "v_" + linkName;
			l.semantic = withoutCentroid(m.semantic);
			l.norm = norm;
			l.dest = dest;
			l.destType = m.type;
			eastl::string scalar;
			uint32_t	  r, c;
			shapeOf(m.type, scalar, r, c);
			if (scalar != "float" && scalar != "half")
				l.interp = "nointerpolation ";
			static const char *const modes[] = {"centroid", "nointerpolation", "linear", "noperspective", "sample"};
			for (const char *mode : modes)
				if (hasWord(m.modifiers, mode) && l.interp.find(mode) == eastl::string::npos)
					l.interp += eastl::string(mode) + " ";
			if (hasCentroid(m.semantic) && l.interp.find("centroid") == eastl::string::npos)
				l.interp += "centroid ";
			return true;
		};
		for (const Member &m : ps->params)
		{
			if (!isVarying(m))
				continue;
			const Struct *s = m.semantic.empty() ? findStruct(m.type) : nullptr;
			if (s)
			{
				for (const Member &sm : s->members)
				{
					if (sm.semantic.empty())
					{
						error = at + "member " + sm.name + " of " + s->name + " has no semantic";
						return false;
					}
					if (!add(sm, m.name + "." + sm.name, m.name + "_" + sm.name))
						return false;
				}
			}
			else if (!add(m, m.name, m.name))
				return false;
		}
	}
	// the vertex and mesh entries name the link after both functions, the pixel entry after its own
	const char		   *tagPrefix = stage == Stage::Mesh ? entryAbi.meshPrefix
									: path == Path::Pool ? entryAbi.poolPrefix
														 : entryAbi.vertexPrefix;
	const eastl::string tag = stage == Stage::Pixel ? psRef.function : out.name.substr(strlen(tagPrefix));
	const eastl::string linkType = entryAbi.linkTypePrefix + tag;
	appendf(t, "\n#line 1 \"<generated %s %s>\"\n", desc.name.c_str(), out.variant.c_str());
	appendf(t, "struct %s\n{\n", linkType.c_str());
	for (const LinkMember &l : link)
		appendf(t, "\t%s%s %s : %s;\n", l.interp.c_str(), l.type.c_str(), l.name.c_str(), l.semantic.c_str());
	if (path != Path::Vertices)
		appendf(t, "\tnointerpolation uint %s : %s; // whose records the pixel reads\n", entryAbi.instanceMember,
				entryAbi.instanceSemantic);
	appendf(t, "\tfloat4 %s : SV_Position;\n};\n\n", entryAbi.positionMember);

	if (stage == Stage::Vertex || stage == Stage::Mesh)
	{
		// the outputs of the vertex function by semantic
		struct Output
		{
			eastl::string type, access;
		};
		eastl::vector<eastl::pair<eastl::string, Output>> outputs;
		if (const Struct *s = findStruct(f->returnType))
		{
			for (const Member &m : s->members)
				if (!m.semantic.empty())
					outputs.push_back(eastl::make_pair(normSemantic(m.semantic), Output{m.type, "rbVertex." + m.name}));
		}
		else if (!f->returnSemantic.empty())
			outputs.push_back(eastl::make_pair(normSemantic(f->returnSemantic), Output{f->returnType, "rbVertex"}));
		const Output *pos = nullptr;
		for (const auto &o : outputs)
			if (o.first == "POSITION0" || o.first == "SV_POSITION0")
				pos = &o.second;
		if (!pos)
		{
			error = at + "the vertex function writes no POSITION / SV_Position";
			return false;
		}
		const bool	  pulled = path != Path::Vertices; // the input from the pools, not the input assembler
		eastl::string params, call, inputs;
		size_t		  u = 0;
		for (const Member &m : f->params)
		{
			if (!call.empty())
				call += ", ";
			if (!isVarying(m))
			{
				call += bound[u++];
				continue;
			}
			call += m.name;
			if (pulled)
			{
				// the vertex function's input filled by semantic from the pool vertex
				if (!m.semantic.empty())
				{
					appendf(inputs, "\t%s %s = %s;\n", m.type.c_str(), m.name.c_str(),
							poolInput(m_recordAbi, normSemantic(m.semantic), m.type).c_str());
					continue;
				}
				appendf(inputs, "\t%s %s;\n", m.type.c_str(), m.name.c_str());
				for (const Member &sm : findStruct(m.type)->members)
					appendf(inputs, "\t%s.%s = %s;\n", m.name.c_str(), sm.name.c_str(),
							poolInput(m_recordAbi, normSemantic(sm.semantic), sm.type).c_str());
				continue;
			}
			if (!params.empty())
				params += ", ";
			params += m.type + " " + m.name;
			if (m.count)
				appendf(params, "[%u]", m.count);
			if (!m.semantic.empty())
				params += " : " + m.semantic;
		}
		eastl::string copy; // the link from the vertex function's outputs
		for (const LinkMember &l : link)
		{
			const Output *src = nullptr;
			for (const auto &o : outputs)
				if (o.first == l.norm)
					src = &o.second;
			const uint32_t k = componentsOf(l.type);
			eastl::string  expr;
			if (!src)
				expr = "0"; // read by the pixel function, not written by the vertex function (undefined in D3D9)
			else
			{
				const uint32_t kv = componentsOf(src->type);
				expr = src->access;
				if (kv > k)
					expr += eastl::string(".") + eastl::string("xyzw").substr(0, k);
				else if (kv < k)
				{
					eastl::string padded = vectorType(l.type, k) + "(" + expr;
					for (uint32_t i = kv; i < k; ++i)
						padded += ", 0";
					expr = padded + ")";
				}
			}
			appendf(copy, "\trbLink.%s = %s;\n", l.name.c_str(), expr.c_str());
		}
		appendf(copy, "\trbLink.%s = %s;\n", entryAbi.positionMember, pos->access.c_str());
		if (pulled)
			appendf(copy, "\trbLink.%s = %s;\n", entryAbi.instanceMember, entryAbi.instanceExpression);
		if (stage == Stage::Vertex && !pulled)
		{
			appendf(t, "%s %s(%s)\n{\n", linkType.c_str(), out.name.c_str(), params.c_str());
			t += load;
			appendf(t, "\t%s rbVertex = %s(%s);\n", f->returnType.c_str(), f->name.c_str(), call.c_str());
			appendf(t, "\t%s rbLink;\n%s\treturn rbLink;\n}\n", linkType.c_str(), copy.c_str());
			return true;
		}
		PulledVertexSource pulledStage{out.name.c_str(),	  linkType.c_str(), tag.c_str(),
									   f->returnType.c_str(), f->name.c_str(),	call.c_str(),
									   inputs.c_str(),		  copy.c_str(),		nullptr};
		if (stage == Stage::Vertex)
		{
			if (entryAbi.writePoolVertex == nullptr)
				return error = at + "the shader policy has no pool vertex generator", false;
			entryAbi.writePoolVertex(pulledStage, t);
			return true;
		}
		PassState meshState;
		if (!desc.resolve(pass, values, meshState, error))
			return false;
		if (entryAbi.writeMeshlet == nullptr)
			return error = at + "the shader policy has no meshlet generator", false;
		pulledStage.backFaceCull = meshState.backFaceCull != "0" ? meshState.backFaceCull.c_str() : nullptr;
		entryAbi.writeMeshlet(pulledStage, t);
		return true;
	}

	// the pixel entry
	PassState state;
	if (!desc.resolve(pass, values, state, error))
		return false;
	const bool own = findStruct(f->returnType) != nullptr;
	// back faces of a material that culls them: the meshlet path culled the primitive; the vertex and pool paths
	// cannot, their pixel entry discards it
	const bool			discardBack = state.backFaceCull != "0" && path != Path::Meshlets;
	const eastl::string args =
		linkType + " rbLinkInput" + (face.empty() && !discardBack ? "" : ", bool rbFront : SV_IsFrontFace");
	if (own)
		appendf(t, "%s %s(%s)\n{\n", f->returnType.c_str(), out.name.c_str(), args.c_str());
	else if (state.blend == BlendMode::None)
		appendf(t, "float4 %s(%s) : SV_Target\n{\n", out.name.c_str(), args.c_str());
	else
		appendf(t, "%s %s(%s)\n{\n", entryAbi.blendOutput, out.name.c_str(), args.c_str());
	t += path != Path::Vertices ? eastl::string(entryAbi.loadLinkedRecords) : load;
	eastl::string call;
	size_t		  u = 0;
	for (const Member &m : f->params)
	{
		if (!call.empty())
			call += ", ";
		if (!isVarying(m))
		{
			call += bound[u++];
			continue;
		}
		call += m.name;
		if (m.semantic.empty())
			appendf(t, "\t%s %s;\n", m.type.c_str(), m.name.c_str());
	}
	for (const LinkMember &l : link)
	{
		if (l.dest.find('.') != eastl::string::npos)
			appendf(t, "\t%s = rbLinkInput.%s;\n", l.dest.c_str(), l.name.c_str());
		else
			appendf(t, "\t%s %s = rbLinkInput.%s;\n", l.destType.c_str(), l.dest.c_str(), l.name.c_str());
	}
	if (!face.empty() && face.find('.') != eastl::string::npos)
		appendf(t, "\t%s = rbFront;\n", face.c_str());
	else if (!face.empty())
		appendf(t, "\tbool %s = rbFront;\n", face.c_str());
	if (!position.empty() && position.find('.') != eastl::string::npos)
		appendf(t, "\t%s = rbLinkInput.%s;\n", position.c_str(), entryAbi.positionMember);
	else if (!position.empty())
		appendf(t, "\tfloat4 %s = rbLinkInput.%s;\n", position.c_str(), entryAbi.positionMember);
	if (own)
	{
		appendf(t, "\treturn %s(%s);\n}\n", f->name.c_str(), call.c_str());
		return true;
	}
	if (componentsOf(f->returnType) != 4)
	{
		error = at + "a pixel function returns a 4-component colour or a struct of SV_Target outputs";
		return false;
	}
	if (discardBack)
		appendf(t, "\tif ((bool)(%s) && !rbFront)\n\t\tdiscard;\n", state.backFaceCull.c_str());
	appendf(t, "\tfloat4 rbColor = %s(%s);\n", f->name.c_str(), call.c_str());
	if (state.alphaTest)
	{
		// D3D9's alpha test, AlphaFunc GREATEREQUAL: the pixel stays when alpha * 255 >= AlphaRef
		if (isNumberText(state.alphaRef))
			appendf(t, "\tclip(rbColor.a - %s / 255.0);\n", state.alphaRef.c_str());
		else
			appendf(t, "\tclip(rbColor.a - %s / 255.0);\n", entryAbi.materialAlphaRef);
	}
	if (state.blend == BlendMode::None)
	{
		t += "\treturn rbColor;\n}\n";
		return true;
	}
	eastl::string blend[5];
	appendf(t, "\t%s rbBlend;\n", entryAbi.blendOutput);
	if (!blendOperands(state, blend))
		appendf(t, "\tconst uint rbBlendFactors = %s;\n", entryAbi.materialBlendFactors);
	appendf(t, "\t%s(rbColor, %s, %s, %s, %s, %s, rbBlend.a, rbBlend.b);\n", entryAbi.blendFunction, blend[0].c_str(),
			blend[1].c_str(), blend[2].c_str(), blend[3].c_str(), blend[4].c_str());
	t += "\treturn rbBlend;\n}\n";
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
namespace
{
// a record type as C++: the scalar and the array dimensions ("float", "[4][3]")
void cppType(const eastl::string &type, uint32_t count, eastl::string &scalar, eastl::string &dims,
			 const char *recordPrefix)
{
	dims.clear();
	if (count)
		appendf(dims, "[%u]", count);
	eastl::string s;
	uint32_t	  rows, columns;
	if (!shapeOf(type, s, rows, columns))
	{
		const size_t prefixLength = recordPrefix != nullptr ? strlen(recordPrefix) : 0;
		scalar =
			prefixLength != 0 && type.compare(0, prefixLength, recordPrefix) == 0 ? type.substr(prefixLength) : type;
		return;
	}
	scalar = s == "float" || s == "half" ? "float" : s == "int" ? "int32_t" : "uint32_t";
	if (rows > 1)
		appendf(dims, "[%u][%u]", rows, columns);
	else if (columns > 1)
		appendf(dims, "[%u]", columns);
}

void writeStruct(eastl::string &out, const char *name, const eastl::vector<StructDesc::Field> &fields, uint32_t size,
				 const char *recordPrefix)
{
	appendf(out, "struct %s\n{\n", name);
	for (const StructDesc::Field &f : fields)
	{
		eastl::string scalar, dims;
		cppType(f.type, f.count, scalar, dims, recordPrefix);
		appendf(out, "\t%s %s%s;\n", scalar.c_str(), f.name.c_str(), dims.c_str());
	}
	out += "};\n";
	for (const StructDesc::Field &f : fields)
		appendf(out, "static_assert(offsetof(%s, %s) == %u, \"%s.%s\");\n", name, f.name.c_str(), f.offset, name,
				f.name.c_str());
	appendf(out, "static_assert(sizeof(%s) == %u, \"%s\");\n", name, size, name);
}
} // namespace

uint32_t Effect::standardStruct(const eastl::string &name, eastl::vector<StructDesc> &out) const
{
	for (const StructDesc &d : out)
		if (d.name == name)
			return d.size;
	const Struct *s = findStruct(name);
	if (!s)
		return 0;
	StructDesc d;
	d.name = name;
	for (const Member &m : s->members)
	{
		StructDesc::Field f;
		f.name = m.name;
		f.type = recordType(m.type);
		f.count = m.count;
		f.offset = d.size;
		eastl::string scalar;
		uint32_t	  rows, columns, n;
		if (shapeOf(f.type, scalar, rows, columns))
			n = rows * columns * 4;
		else
			n = standardStruct(f.type, out); // a record inside a record (the light list's lights)
		if (!n)
			return 0;
		d.size += n * (m.count ? m.count : 1);
		d.fields.push_back(f);
	}
	out.push_back(d);
	return d.size;
}

void Effect::standardRecords(const char *const *names, size_t count, eastl::vector<StructDesc> &out) const
{
	out.clear();
	for (size_t i = 0; i < count; ++i)
		standardStruct(names[i], out);
}

namespace
{
// one effect's namespace: its records, registers, vertex inputs and layout hash
void writeEffectLayout(eastl::string &out, const EffectDesc &d, const ShaderRecordAbi *recordAbi,
					   const char *recordPrefix)
{
	appendf(out, "namespace %s\n{\n", d.name.c_str());
	for (const StructDesc &s : d.structs)
		writeStruct(out, s.name.c_str(), s.fields, s.size, recordPrefix);
	for (uint32_t r = 0; r < C_RATES; ++r)
	{
		if (!d.recordSize[r])
			continue;
		eastl::vector<StructDesc::Field> fields;
		if (r == uint32_t(Rate::Material) && d.materialHeader && recordAbi != nullptr)
			fields.push_back(StructDesc::Field{recordAbi->materialHeader.member, recordAbi->materialHeader.type, 0, 0});
		for (const ParamDesc &p : d.params)
			if (p.offset >= 0 && static_cast<uint32_t>(p.rate) == r)
				fields.push_back(
					StructDesc::Field{p.name, recordType(p.type), p.count, static_cast<uint32_t>(p.offset)});
		if (r == 2)
			for (const SamplerDesc &s : d.samplers)
				if (s.offset >= 0)
					fields.push_back(StructDesc::Field{s.name, "uint", 0, static_cast<uint32_t>(s.offset)});
		eastl::sort(fields.begin(), fields.end(),
					[](const StructDesc::Field &a, const StructDesc::Field &b) { return a.offset < b.offset; });
		writeStruct(out, C_RATE_RECORD[r], fields, d.recordSize[r], recordPrefix);
	}
	for (const RegisterDesc &r : d.registers)
		appendf(out, "constexpr uint32_t C_REGISTER_%s = %u; // %c%u, space %u\n", r.name.c_str(), r.index, r.kind,
				r.index, r.space);
	for (const TechniqueDesc &t : d.techniques)
		for (const PassDesc &p : t.passes)
		{
			if (p.inputs.empty())
				continue;
			appendf(out, "constexpr VertexInput C_INPUTS_%s_%s[] = {", t.name.c_str(), p.name.c_str());
			for (const PassDesc::Input &in : p.inputs)
				appendf(out, "{\"%s\", %u, \"%s\"}, ", in.semantic.c_str(), in.index, in.type.c_str());
			out += "};\n";
		}
	appendf(out, "constexpr uint64_t C_LAYOUT_HASH = 0x%016llxull;\n} // namespace %s\n\n",
			static_cast<unsigned long long>(d.layoutHash), d.name.c_str());
}
} // namespace

void layoutHeaders(const LayoutHeaderPolicy &headers, const ShaderRecordAbi *recordAbi,
				   const eastl::vector<EffectDesc> &effects, const eastl::vector<StructDesc> &standard,
				   const eastl::vector<eastl::string> &layerEffects, eastl::string &layer, eastl::string &host)
{
	layer.clear();
	appendf(layer, "// %s - generated shader layout. Do not edit.\n", headers.sharedHeader);
	layer += "// Records shared by every effect and the layouts of the GPU layer's own effects. The generated entries\n"
			 "// read records with ByteAddressBuffer.Load<T>: tight packing, row-major matrices.\n"
			 "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n"
			 "namespace render_backend::shader::layout\n{\n"
			 "// Shared records declared by the prelude. The renderer fills these structs.\n";
	for (const StructDesc &s : standard)
		writeStruct(layer,
					s.name.c_str() + (headers.recordPrefix != nullptr &&
											  s.name.compare(0, strlen(headers.recordPrefix), headers.recordPrefix) == 0
										  ? strlen(headers.recordPrefix)
										  : 0),
					s.fields, s.size, headers.recordPrefix);
	appendf(layer, "// The maximum vertex count in a pool-path task.\nconstexpr uint32_t C_POOL_VERTICES = %u;\n",
			headers.poolVertices);
	layer += "// An input of a vertex function (the input layout of the vertex path).\n"
			 "struct VertexInput\n{\n\tconst char *semantic;\n\tuint32_t index;\n\tconst char *type;\n};\n\n";
	host.clear();
	appendf(host, "// %s - generated shader layout. Do not edit.\n", headers.consumerHeader);
	appendf(host, "// Host effects (shared records and GPU layer effects are in %s): the records the renderer\n",
			headers.sharedHeader);
	host += "// fills for each effect (frame record, instance data, material record), the registers the front-end\n"
			"// assigned, and the layout hash of every effect, which ShaderCache::expectLayouts compares against.\n"
			"#pragma once\n";
	appendf(host, "#include \"%s\"\n#include <render_backend/shader/effect_desc.hpp>\n\nnamespace render_backend::shader::layout\n{\n",
			headers.sharedHeader);
	for (const EffectDesc &d : effects)
	{
		const bool own = eastl::find(layerEffects.begin(), layerEffects.end(), d.name) != layerEffects.end();
		writeEffectLayout(own ? layer : host, d, recordAbi, headers.recordPrefix);
	}
	layer += "} // namespace render_backend::shader::layout\n";
	host += "constexpr render_backend::shader::EffectLayout C_EFFECT_LAYOUTS[] = {\n";
	for (const EffectDesc &d : effects)
		appendf(host, "\t{\"%s\", %s::C_LAYOUT_HASH},\n", d.name.c_str(), d.name.c_str());
	host += "};\n\n} // namespace render_backend::shader::layout\n";
}

} // namespace render_backend::shader::front
