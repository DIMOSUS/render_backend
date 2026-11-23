// Builds shaders with the front-end and DXC and caches the results next to the sources. Contains the include handler
// that records every file the preprocessor reads, the manifests (.dep) written beside each .desc and .dxil, and the
// load-or-rebuild logic. dxcompiler.dll is loaded at run time with no import library, so a Prebuilt cache never
// loads it.
#include "front_end.hpp"
#include <render_backend/shader/effect_desc.hpp>
#include <render_backend/shader/auto_constants.hpp>
#include <render_backend/shader/shader_cache.hpp>
#include <EASTL/map.h>
#include <EASTL/sort.h>
#include <windows.h>
#include <dxcapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace render_backend::shader
{
const char *const C_COMPILE_OPTIONS = "-Zpr -O3 -HV 2021";

namespace
{
constexpr const char *C_MANIFEST_HEADER = "render-shader 2";
// Bytecode and its variant manifest share an ABI-tagged path. Bump it when the renderer/shader wire format changes.
constexpr const char *C_BYTECODE_ABI_SUFFIX = ".abi3";

// Headers that do not include windows.h store an SRWLOCK as a void* member. SRWLOCK is one pointer and
// SRWLOCK_INIT is zero, so the member is reinterpreted as the Win32 type here.
PSRWLOCK srwOf(void *&member)
{
	static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK is one pointer");
	return reinterpret_cast<PSRWLOCK>(&member);
}

class ExclusiveLock
{
  public:
	explicit ExclusiveLock(void *&member) : m_lock(srwOf(member))
	{
		AcquireSRWLockExclusive(m_lock);
	}
	~ExclusiveLock()
	{
		ReleaseSRWLockExclusive(m_lock);
	}
	ExclusiveLock(const ExclusiveLock &) = delete;
	ExclusiveLock &operator=(const ExclusiveLock &) = delete;

  private:
	PSRWLOCK m_lock;
};

eastl::wstring widen(const char *text)
{
	const int	   n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
	eastl::wstring out(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), n);
	return out;
}

eastl::string narrow(const wchar_t *text)
{
	const int	  n = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
	eastl::string out(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_ACP, 0, text, -1, out.data(), n, nullptr, nullptr);
	return out;
}

bool readFile(const char *path, eastl::vector<uint8_t> &out)
{
	out.clear();
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size{};
	bool		  ok = GetFileSizeEx(file, &size) && size.QuadPart < 0x40000000;
	if (ok)
	{
		out.resize(static_cast<size_t>(size.QuadPart));
		DWORD got = 0;
		ok = out.empty() ||
			 (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
	}
	CloseHandle(file);
	return ok;
}

bool readText(const eastl::string &path, eastl::string &out)
{
	eastl::vector<uint8_t> bytes;
	if (!readFile(path.c_str(), bytes))
		return false;
	out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size()); // the file is text
	return true;
}

// Writes a temporary file and renames it over the target, so a concurrent reader never sees a partial file.
bool writeFile(const eastl::string &path, const void *data, size_t size)
{
	// The temporary name includes the process and thread ids: an offline compiler and a running application may
	// write the same entry at the same time.
	char tag[48];
	snprintf(tag, sizeof tag, ".%lu.%lu.tmp", GetCurrentProcessId(), GetCurrentThreadId());
	const eastl::string temp = path + tag;
	HANDLE file = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD wrote = 0;
	bool  ok = WriteFile(file, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size;
	CloseHandle(file);
	if (ok)
		ok = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
	if (!ok)
		DeleteFileA(temp.c_str());
	return ok;
}

eastl::string fullPath(const char *path)
{
	char  buffer[MAX_PATH * 2];
	DWORD n = GetFullPathNameA(path, sizeof buffer, buffer, nullptr);
	if (n == 0 || n >= sizeof buffer)
		return eastl::string(path);
	return eastl::string(buffer, n);
}

// path relative to root with '/' (both full paths); the full path itself when it is outside root
eastl::string relativeTo(const eastl::string &root, const eastl::string &path)
{
	eastl::string rel = path;
	if (path.size() > root.size() && _strnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
		(path[root.size()] == '\\' || path[root.size()] == '/'))
		rel.assign(path.begin() + root.size() + 1, path.end());
	for (char &c : rel)
		if (c == '\\')
			c = '/';
	return rel;
}

void hexOf(uint64_t value, char (&out)[17])
{
	snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(value));
}

// Reads every file DXC asks for and records its root-relative path with the hash of the bytes read. It lives on the
// stack of one call: AddRef/Release only count, nothing frees it.
class IncludeHandler final : public IDxcIncludeHandler
{
  public:
	IncludeHandler(IDxcUtils *utils, const eastl::string &root, Compiler::Result &result)
		: m_utils(utils), m_root(root), m_result(result)
	{
	}

	HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob **includeSource) override
	{
		*includeSource = nullptr;
		// relative names are relative to the root (the buffer is named effects/<name>.hlsl)
		eastl::string name = narrow(fileName);
		while (name.size() > 2 && (name[0] == '.' && (name[1] == '/' || name[1] == '\\')))
			name.erase(0, 2);
		const bool			   absolute = name.size() > 2 && name[1] == ':';
		eastl::string		   path = fullPath(absolute ? name.c_str() : (m_root + "\\" + name).c_str());
		eastl::vector<uint8_t> bytes;
		if (!readFile(path.c_str(), bytes))
			return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		const eastl::string rel = relativeTo(m_root, path);
		bool				known = false;
		for (const eastl::string &file : m_result.files)
			known = known || _stricmp(file.c_str(), rel.c_str()) == 0;
		if (!known)
		{
			m_result.files.push_back(rel);
			m_result.hashes.push_back(hashOf(bytes.data(), bytes.size()));
		}
		IDxcBlobEncoding *blob = nullptr;
		HRESULT			  hr = m_utils->CreateBlob(bytes.data(), static_cast<UINT32>(bytes.size()), DXC_CP_UTF8, &blob);
		*includeSource = blob;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
	{
		if (iid == __uuidof(IDxcIncludeHandler) || iid == __uuidof(IUnknown))
		{
			*object = static_cast<IDxcIncludeHandler *>(this);
			AddRef();
			return S_OK;
		}
		*object = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++m_references;
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
		return --m_references;
	}

  private:
	IDxcUtils		 *m_utils;
	eastl::string	  m_root;
	Compiler::Result &m_result;
	ULONG			  m_references = 1;
};

// the newest Windows SDK's x64 dxcompiler.dll (bin/10.*/x64), empty when there is none
eastl::string sdkCompilerPath()
{
	char  programs[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("ProgramFiles(x86)", programs, sizeof programs);
	if (n == 0 || n >= sizeof programs)
		return eastl::string();
	const eastl::string bin = eastl::string(programs) + "\\Windows Kits\\10\\bin\\";
	WIN32_FIND_DATAA	found;
	HANDLE				search = FindFirstFileA((bin + "10.*").c_str(), &found);
	eastl::string		best;
	if (search == INVALID_HANDLE_VALUE)
		return best;
	do
	{
		const eastl::string candidate = bin + found.cFileName + "\\x64\\dxcompiler.dll";
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
			GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES && candidate > best)
			best = candidate;
	} while (FindNextFileA(search, &found));
	FindClose(search);
	return best;
}

// one manifest, as written
struct Manifest
{
	eastl::string compiler;
	eastl::string frontend;
	eastl::string entry;
	eastl::string variant; // "<technique> <pass> <stage> <values...>" ("-" for a description)
	eastl::string profile;
	eastl::string options;
	uint64_t	  output = 0;
	uint64_t	  policy = 0;
	uint64_t	  layout = 0; // the description's layoutHash the variant was built against (its records and registers)
	eastl::vector<eastl::string> files;
	eastl::vector<uint64_t>		 hashes;

	bool read(const eastl::string &path)
	{
		eastl::string text;
		if (!readText(path, text))
			return false;
		const char *p = text.c_str();
		const char *end = p + text.size();
		bool		header = false;
		while (p < end)
		{
			const char *eol = static_cast<const char *>(memchr(p, '\n', end - p));
			if (!eol)
				eol = end;
			eastl::string line(p, eol);
			p = eol + 1;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t  space = line.find(' ');
			eastl::string key = line.substr(0, space);
			eastl::string value = space == eastl::string::npos ? eastl::string() : line.substr(space + 1);
			if (!header)
			{
				if (line != C_MANIFEST_HEADER)
					return false;
				header = true;
			}
			else if (key == "compiler")
				compiler = value;
			else if (key == "frontend")
				frontend = value;
			else if (key == "entry")
				entry = value;
			else if (key == "variant")
				variant = value;
			else if (key == "profile")
				profile = value;
			else if (key == "options")
				options = value;
			else if (key == "output")
				output = _strtoui64(value.c_str(), nullptr, 16);
			else if (key == "policy")
				policy = _strtoui64(value.c_str(), nullptr, 16);
			else if (key == "layout")
				layout = _strtoui64(value.c_str(), nullptr, 16);
			else if (key == "file")
			{
				// "file <hash> <path>": the path last, it may hold blanks
				const size_t blank = value.find(' ');
				if (blank == eastl::string::npos)
					return false;
				hashes.push_back(_strtoui64(value.substr(0, blank).c_str(), nullptr, 16));
				files.push_back(value.substr(blank + 1));
			}
			else if (!line.empty())
				return false;
		}
		return header && !files.empty();
	}

	eastl::string text() const
	{
		char		  hex[17];
		eastl::string out = C_MANIFEST_HEADER;
		out += "\ncompiler " + compiler + "\nfrontend " + frontend + "\nentry " + entry + "\nvariant " + variant +
			   "\nprofile " + profile + "\noptions " + options;
		hexOf(output, hex);
		out += "\noutput ";
		out += hex;
		hexOf(policy, hex);
		out += "\npolicy ";
		out += hex;
		hexOf(layout, hex);
		out += "\nlayout ";
		out += hex;
		for (size_t i = 0; i < files.size(); ++i)
		{
			hexOf(hashes[i], hex);
			out += "\nfile ";
			out += hex;
			out += " " + files[i];
		}
		out += "\n";
		return out;
	}
};

eastl::string variantLine(const BuildEntry &e)
{
	eastl::string out = e.technique + " " + e.pass + " " + stageName(e.stage);
	if (e.path != Path::Vertices)
		out += eastl::string("-") + pathName(e.path);
	for (int32_t v : e.values)
	{
		char text[16];
		snprintf(text, sizeof text, " %d", v);
		out += text;
	}
	return out;
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
const char *profileOf(Stage stage)
{
	switch (stage)
	{
	case Stage::Vertex:
		return "vs_6_6";
	case Stage::Pixel:
		return "ps_6_6";
	case Stage::Mesh:
		return "ms_6_6";
	case Stage::Amplification:
		return "as_6_6";
	default:
		return "cs_6_6";
	}
}

const char *stageName(Stage stage)
{
	switch (stage)
	{
	case Stage::Vertex:
		return "vs";
	case Stage::Pixel:
		return "ps";
	case Stage::Mesh:
		return "ms";
	case Stage::Amplification:
		return "as";
	default:
		return "cs";
	}
}

//----------------------------------------------------------------------------------------------------------------------
uint64_t hashOf(const void *data, size_t size)
{
	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	uint64_t	   hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
}

//----------------------------------------------------------------------------------------------------------------------
const char *reasonName(BuildReason reason)
{
	switch (reason)
	{
	case BuildReason::UpToDate:
		return "up to date";
	case BuildReason::NoOutput:
		return "no built file";
	case BuildReason::NoManifest:
		return "no manifest";
	case BuildReason::SourceChanged:
		return "source changed";
	case BuildReason::IncludeChanged:
		return "include changed";
	case BuildReason::CompilerChanged:
		return "compiler changed";
	case BuildReason::OptionsChanged:
		return "options changed";
	}
	return "?";
}

//----------------------------------------------------------------------------------------------------------------------
eastl::string compiledPath(const eastl::string &root, const char *effect, const char *variant, const char *extension)
{
	const char *abi = strcmp(variant, "desc") == 0 ? "" : C_BYTECODE_ABI_SUFFIX;
	return root + "\\effects\\" + effect + "." + variant + abi + extension;
}

//----------------------------------------------------------------------------------------------------------------------
Compiler::~Compiler()
{
	if (m_utils)
		m_utils->Release();
	if (m_compiler)
		m_compiler->Release();
	// the DLL stays loaded: DXC's own threads and allocations may outlive the interfaces (its documentation advises
	// against unloading it)
}

//----------------------------------------------------------------------------------------------------------------------
bool Compiler::load(const char *dllPath, eastl::string &error)
{
	eastl::string path;
	if (dllPath)
		path = dllPath;
	else
	{
		char  exe[MAX_PATH];
		DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
		if (n > 0 && n < sizeof exe)
		{
			path.assign(exe, n);
			path.erase(path.find_last_of("\\/") + 1);
			path += "dxcompiler.dll";
		}
		if (path.empty() || GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
			path = sdkCompilerPath();
	}
	if (path.empty())
	{
		error = "no dxcompiler.dll next to the program and no Windows SDK with one";
		return false;
	}
	m_module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m_module)
	{
		error = "cannot load " + path;
		return false;
	}
	// GetProcAddress gives an untyped address: the documented signature of DxcCreateInstance (dxcapi.h)
	auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m_module, "DxcCreateInstance"));
	if (!create || FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void **>(&m_compiler))) ||
		FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void **>(&m_utils))))
	{
		if (m_compiler)
			m_compiler->Release();
		m_compiler = nullptr; // loaded() stays false: half a compiler is none
		m_utils = nullptr;
		error = path + " gives no IDxcCompiler3";
		return false;
	}
	m_path = path;
	IDxcVersionInfo *info = nullptr;
	UINT32			 major = 0, minor = 0;
	if (SUCCEEDED(m_compiler->QueryInterface(__uuidof(IDxcVersionInfo), reinterpret_cast<void **>(&info))))
	{
		info->GetVersion(&major, &minor);
		info->Release();
	}
	char version[64];
	snprintf(version, sizeof version, "%u.%u", major, minor);
	m_version = version;
	IDxcVersionInfo2 *info2 = nullptr;
	if (SUCCEEDED(m_compiler->QueryInterface(__uuidof(IDxcVersionInfo2), reinterpret_cast<void **>(&info2))))
	{
		UINT32 commits = 0;
		char  *hash = nullptr;
		if (SUCCEEDED(info2->GetCommitInfo(&commits, &hash)))
		{
			snprintf(version, sizeof version, ".%u (%s)", commits, hash ? hash : "");
			m_version += version;
			CoTaskMemFree(hash);
		}
		info2->Release();
	}
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
namespace
{
// one IDxcCompiler3::Compile: the messages, and the object (compile) or the preprocessed text (-P)
bool runDxc(IDxcCompiler3 *compiler, IDxcUtils *utils, const eastl::string &root, const eastl::string &source,
			const eastl::vector<eastl::wstring> &words, bool preprocess, Compiler::Result &out)
{
	eastl::vector<LPCWSTR> arguments;
	for (const eastl::wstring &word : words)
		arguments.push_back(word.c_str());
	IncludeHandler includes(utils, root, out);
	DxcBuffer	   buffer{source.data(), source.size(), DXC_CP_UTF8};
	IDxcResult	  *result = nullptr;
	HRESULT		   hr = compiler->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()), &includes,
										  __uuidof(IDxcResult), reinterpret_cast<void **>(&result));
	if (FAILED(hr) || !result)
	{
		out.messages = "IDxcCompiler3::Compile failed";
		return false;
	}
	IDxcBlobUtf8 *errors = nullptr;
	if (SUCCEEDED(
			result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void **>(&errors), nullptr)) &&
		errors)
	{
		out.messages.assign(errors->GetStringPointer(), errors->GetStringLength());
		errors->Release();
	}
	HRESULT status = E_FAIL;
	result->GetStatus(&status);
	if (SUCCEEDED(status) && preprocess)
	{
		IDxcBlobUtf8 *text = nullptr;
		if (SUCCEEDED(
				result->GetOutput(DXC_OUT_HLSL, __uuidof(IDxcBlobUtf8), reinterpret_cast<void **>(&text), nullptr)) &&
			text)
		{
			out.text.assign(text->GetStringPointer(), text->GetStringLength());
			text->Release();
			out.ok = true;
		}
	}
	else if (SUCCEEDED(status))
	{
		IDxcBlob *object = nullptr;
		if (SUCCEEDED(
				result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void **>(&object), nullptr)) &&
			object)
		{
			const uint8_t *bytes = static_cast<const uint8_t *>(object->GetBufferPointer());
			out.bytecode.assign(bytes, bytes + object->GetBufferSize());
			object->Release();
			out.ok = !out.bytecode.empty();
		}
	}
	result->Release();
	return out.ok;
}
} // namespace

bool Compiler::preprocess(const char *root, const char *sourceRel, Result &out, const char *sourcePrefix) const
{
	out = Result();
	if (!m_compiler)
	{
		out.messages = "no compiler loaded";
		return false;
	}
	const eastl::string	   rootPath = fullPath(root);
	eastl::vector<uint8_t> source;
	if (!readFile((rootPath + "\\" + sourceRel).c_str(), source))
	{
		out.messages = eastl::string("cannot read ") + sourceRel;
		return false;
	}
	out.files.push_back(sourceRel);
	out.hashes.push_back(hashOf(source.data(), source.size()));
	// the prelude, then the effect under its own name
	eastl::string text = sourcePrefix != nullptr ? sourcePrefix : "";
	if (!text.empty() && text.back() != '\n')
		text += '\n';
	text += "#line 1 \"" + eastl::string(sourceRel) + "\"\n";
	text.append(reinterpret_cast<const char *>(source.data()), source.size());
	eastl::vector<eastl::wstring> words;
	words.push_back(L"-P");
	words.push_back(L"-HV");
	words.push_back(L"2021");
	words.push_back(widen(sourceRel));
	return runDxc(m_compiler, m_utils, rootPath, text, words, true, out);
}

bool Compiler::compile(const char *name, const eastl::string &text, const char *entry, Stage stage, Result &out) const
{
	out = Result();
	if (!m_compiler)
	{
		out.messages = "no compiler loaded";
		return false;
	}
	// the arguments: entry, profile, the fixed options, the file name (for the messages)
	eastl::vector<eastl::wstring> words;
	words.push_back(L"-E");
	words.push_back(widen(entry));
	words.push_back(L"-T");
	words.push_back(widen(profileOf(stage)));
	const eastl::wstring options = widen(C_COMPILE_OPTIONS);
	for (size_t start = 0; start < options.size();)
	{
		size_t end = options.find(L' ', start);
		if (end == eastl::wstring::npos)
			end = options.size();
		if (end > start)
			words.push_back(options.substr(start, end - start));
		start = end + 1;
	}
	words.push_back(widen(name));
	return runDxc(m_compiler, m_utils, eastl::string("."), text, words, false, out);
}

bool Compiler::disassemble(const eastl::vector<uint8_t> &bytecode, eastl::string &out) const
{
	out.clear();
	if (!m_compiler)
		return false;
	DxcBuffer	buffer{bytecode.data(), bytecode.size(), 0};
	IDxcResult *result = nullptr;
	if (FAILED(m_compiler->Disassemble(&buffer, __uuidof(IDxcResult), reinterpret_cast<void **>(&result))) || !result)
		return false;
	IDxcBlobUtf8 *text = nullptr;
	if (SUCCEEDED(result->GetOutput(DXC_OUT_DISASSEMBLY, __uuidof(IDxcBlobUtf8), reinterpret_cast<void **>(&text),
									nullptr)) &&
		text)
	{
		out.assign(text->GetStringPointer(), text->GetStringLength());
		text->Release();
	}
	result->Release();
	return !out.empty();
}

//----------------------------------------------------------------------------------------------------------------------
// one build round: the hashes of the files (a file several entries include is read once) and the parsed effects
struct ShaderCache::Round
{
	struct Parsed
	{
		eastl::string				 name;
		bool						 ok = false;
		eastl::string				 error;
		front::Effect				 effect;
		eastl::vector<eastl::string> files;
		eastl::vector<uint64_t>		 hashes;
	};
	eastl::map<eastl::string, uint64_t> hashes;
	eastl::string						root;
	eastl::vector<Parsed *>				parsed;

	const ShaderPolicy &policy;
	uint64_t			policyHash;

	Round(const ShaderPolicy &inputPolicy, uint64_t inputPolicyHash) : policy(inputPolicy), policyHash(inputPolicyHash)
	{
	}
	Round(const Round &) = delete;
	Round &operator=(const Round &) = delete;
	~Round()
	{
		for (Parsed *p : parsed)
			delete p;
	}

	// 0 for a file that is gone
	uint64_t get(const eastl::string &rel)
	{
		auto found = hashes.find(rel);
		if (found != hashes.end())
			return found->second;
		// a file outside the root is recorded with its full path
		const bool			   absolute = rel.size() > 2 && rel[1] == ':';
		eastl::vector<uint8_t> bytes;
		uint64_t			   hash =
			readFile((absolute ? rel : root + "\\" + rel).c_str(), bytes) ? hashOf(bytes.data(), bytes.size()) : 0;
		hashes[rel] = hash;
		return hash;
	}

	// the effect preprocessed and parsed once per round
	Parsed &parse(const Compiler &compiler, const eastl::string &name)
	{
		for (Parsed *p : parsed)
			if (p->name == name)
				return *p;
		Parsed *p = new Parsed;
		parsed.push_back(p);
		p->name = name;
		const eastl::string sourceRel = "effects/" + name + ".hlsl";
		Compiler::Result	pre;
		if (!compiler.preprocess(root.c_str(), sourceRel.c_str(), pre, policy.sourcePrefix))
		{
			p->error = pre.messages;
			return *p;
		}
		p->files = pre.files;
		p->hashes = pre.hashes;
		for (size_t i = 0; i < pre.files.size(); ++i)
			hashes[pre.files[i]] = pre.hashes[i];
		p->ok = p->effect.parse(name.c_str(), sourceRel.c_str(), pre.text, policy, p->error);
		p->effect.desc.policyHash = policyHash;
		return *p;
	}
};

//----------------------------------------------------------------------------------------------------------------------
ShaderCache::ShaderCache() = default;
ShaderCache::~ShaderCache() = default;

//----------------------------------------------------------------------------------------------------------------------
bool ShaderCache::open(const char *rootDir, Mode mode, eastl::string &error, const char *dxcPath,
					   const ShaderPolicy &policy)
{
	if (policy.entries == EntryModel::RendererRecords &&
		(policy.recordAbi == nullptr || policy.recordAbi->entries == nullptr))
	{
		error = "renderer-record shaders require the caller's record and entry ABI";
		return false;
	}
	m_sourcePrefix = policy.sourcePrefix != nullptr ? policy.sourcePrefix : "";
	m_preludePath = policy.preludePath != nullptr ? policy.preludePath : "";
	m_policy = {policy.entries, m_sourcePrefix.c_str(), m_preludePath.c_str(), policy.recordAbi};
	eastl::string key = m_sourcePrefix;
	key += '\0';
	key += m_preludePath;
	key += '\0';
	key += char(m_policy.entries);
	if (m_policy.recordAbi != nullptr && m_policy.recordAbi->versionTag != nullptr && *m_policy.recordAbi->versionTag)
	{
		key += '\0';
		key += m_policy.recordAbi->versionTag;
	}
	m_policyHash = hashOf(key.data(), key.size());

	m_root = fullPath(rootDir);
	while (!m_root.empty() && (m_root.back() == '\\' || m_root.back() == '/'))
		m_root.pop_back();
	m_mode = mode;
	const DWORD attributes = GetFileAttributesA((m_root + "\\effects").c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		error = "no effects folder in " + m_root;
		return false;
	}
	m_deployed.clear();
	if (mode == Mode::Prebuilt)
	{
		// The deployed list defines the effects of this folder, whatever else an older deployment left in it.
		eastl::string text;
		if (!readText(m_root + "\\effects\\" + C_DEPLOYED_EFFECTS, text))
		{
			error = eastl::string("no ") + C_DEPLOYED_EFFECTS + " in " + m_root + "\\effects (deploy the shader set again)";
			return false;
		}
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			if (end == eastl::string::npos)
				end = text.size();
			eastl::string name = text.substr(start, end - start);
			while (!name.empty() && (name.back() == '\r' || name.back() == ' '))
				name.pop_back();
			if (!name.empty())
				m_deployed.push_back(name);
			start = end + 1;
		}
		eastl::sort(m_deployed.begin(), m_deployed.end());
		return true;
	}
	return m_compiler.load(dxcPath, error);
}

bool ShaderCache::deployedEffect(const eastl::string &effect) const
{
	for (const eastl::string &name : m_deployed)
		if (_stricmp(name.c_str(), effect.c_str()) == 0)
			return true;
	return false;
}

bool ShaderCache::loadDescription(EffectDesc &desc, const eastl::string &path, eastl::string &error) const
{
	if (!desc.load(path.c_str(), error))
		return false;
	// Older renderer-record packs have no policy hash. They still require an explicit record-mode consumer.
	if (desc.entryModel == m_policy.entries &&
		(desc.policyHash == m_policyHash || (desc.policyHash == 0 && desc.entryModel == EntryModel::RendererRecords)))
		return true;
	error = path + ": the shader description was compiled with a different source/entry policy";
	return false;
}

bool ShaderCache::acceptDescription(const EffectDesc &desc, eastl::string &messages) const
{
	for (size_t i = 0; i < m_layoutCount; ++i)
		if (desc.name == m_layouts[i].effect && desc.layoutHash != m_layouts[i].hash)
		{
			char text[320];
			snprintf(text, sizeof text,
					 "%s: the layout of its records or registers (hash %016llx) differs from the one the host was "
					 "built against (%016llx): regenerate the layout headers and rebuild the host",
					 desc.name.c_str(), static_cast<unsigned long long>(desc.layoutHash),
					 static_cast<unsigned long long>(m_layouts[i].hash));
			messages += text;
			return false;
		}
	if (m_mode != Mode::Develop || m_autos == nullptr)
		return true;
	// A parameter that names a registered auto-constant must have its type and rate. Notes (a semantic nobody
	// registered, a semantic several fields share) go with the report.
	eastl::vector<eastl::string> errors, notes;
	const bool					 ok = m_autos->check(desc, errors, &notes);
	for (const eastl::string &n : notes)
		messages += n + "\n";
	for (const eastl::string &e : errors)
		messages += e + "\n";
	return ok;
}

void ShaderCache::expectLayouts(const EffectLayout *layouts, size_t count)
{
	ExclusiveLock lock(m_lock);
	m_layouts = layouts;
	m_layoutCount = count;
}

//----------------------------------------------------------------------------------------------------------------------
void ShaderCache::readAliases(eastl::vector<eastl::pair<eastl::string, eastl::string>> &aliases) const
{
	// "<alias> = <effect>" lines, '#' comments
	aliases.clear();
	eastl::string text;
	if (!readText(m_root + "\\effects\\aliases.txt", text))
		return;
	size_t start = 0;
	while (start < text.size())
	{
		size_t eol = text.find('\n', start);
		if (eol == eastl::string::npos)
			eol = text.size();
		eastl::string line = text.substr(start, eol - start);
		start = eol + 1;
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == eastl::string::npos)
			continue;
		auto trim = [](eastl::string s)
		{
			while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t'))
				s.pop_back();
			while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
				s.erase(0, 1);
			return s;
		};
		aliases.push_back(eastl::make_pair(trim(line.substr(0, eq)), trim(line.substr(eq + 1))));
	}
}

eastl::string ShaderCache::effectOf(const char *name) const
{
	PSRWLOCK lock = reinterpret_cast<PSRWLOCK>(&m_aliasLock);
	AcquireSRWLockExclusive(lock);
	if (!m_aliasesValid)
	{
		readAliases(m_aliases);
		m_aliasesValid = true;
	}
	eastl::string effect = name;
	for (const auto &a : m_aliases)
		if (a.first == name)
		{
			effect = a.second;
			break;
		}
	ReleaseSRWLockExclusive(lock);
	return effect;
}

void ShaderCache::listEffects(eastl::vector<eastl::string> &names) const
{
	names.clear();
	if (m_mode == Mode::Prebuilt)
	{
		names = m_deployed;
		eastl::vector<eastl::pair<eastl::string, eastl::string>> aliases;
		readAliases(aliases);
		for (const auto &a : aliases)
			names.push_back(a.first);
		eastl::sort(names.begin(), names.end());
		return;
	}
	const char		*extension = ".hlsl";
	WIN32_FIND_DATAA found;
	HANDLE			 search = FindFirstFileA((m_root + "\\effects\\*" + extension).c_str(), &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			// the pattern also matches a long name whose 8.3 alias ends in the extension: the full name decides
			eastl::string name = found.cFileName;
			const size_t  n = strlen(extension);
			if (name.size() <= n || _stricmp(name.c_str() + name.size() - n, extension) != 0)
				continue;
			name.erase(name.size() - n);
			if (name.find('.') == eastl::string::npos)
				names.push_back(name);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	eastl::vector<eastl::pair<eastl::string, eastl::string>> aliases;
	readAliases(aliases);
	for (const auto &a : aliases)
		names.push_back(a.first);
	eastl::sort(names.begin(), names.end());
}

//----------------------------------------------------------------------------------------------------------------------
bool ShaderCache::updateDescription(const eastl::string &name, bool checkOnly, Round &round, BuildEntry &report,
									EffectDesc &desc)
{
	report = BuildEntry();
	report.effect = name;
	report.description = true;
	report.variant = "desc";
	report.entry = "description";
	const eastl::string path = compiledPath(m_root, name.c_str(), "desc", "");
	const eastl::string dep = path + ".dep";
	const eastl::string sourceRel = "effects/" + name + ".hlsl";
	eastl::string		error;
	if (m_mode == Mode::Prebuilt)
	{
		if (!deployedEffect(name))
			error = "effect " + name + " is not in the deployed set (" + C_DEPLOYED_EFFECTS + ")";
		else if (loadDescription(desc, path, error) && acceptDescription(desc, error))
			return true;
		report.result = BuildResult::Failed;
		report.messages = error;
		return false;
	}
	Manifest manifest;
	if (!manifest.read(dep))
		report.reason = BuildReason::NoManifest;
	else if (manifest.compiler != m_compiler.version() || manifest.frontend != front::C_FRONTEND_VERSION)
		report.reason = BuildReason::CompilerChanged;
	else if (manifest.policy != m_policyHash || _stricmp(manifest.files[0].c_str(), sourceRel.c_str()) != 0)
		report.reason = BuildReason::OptionsChanged;
	else
	{
		eastl::vector<uint8_t> bytes;
		if (!readFile(path.c_str(), bytes) || hashOf(bytes.data(), bytes.size()) != manifest.output)
			report.reason = BuildReason::NoOutput;
		for (size_t i = 0; i < manifest.files.size() && report.reason == BuildReason::UpToDate; ++i)
			if (round.get(manifest.files[i]) != manifest.hashes[i])
			{
				report.reason = i == 0 ? BuildReason::SourceChanged : BuildReason::IncludeChanged;
				report.file = manifest.files[i];
			}
	}
	if (report.reason == BuildReason::UpToDate || checkOnly)
	{
		if (checkOnly && report.reason != BuildReason::UpToDate)
			report.result = BuildResult::Stale;
		if (loadDescription(desc, path, error))
		{
			// A stale description is only reported (checkOnly); an up-to-date one is served and must match the host.
			eastl::string why;
			if (report.reason != BuildReason::UpToDate || acceptDescription(desc, why))
				return true;
			report.result = BuildResult::Failed;
			report.messages = why;
			return false;
		}
		if (checkOnly)
			return false; // nothing to check the variants against
	}
	Round::Parsed &parsed = round.parse(m_compiler, name);
	if (!parsed.ok)
	{
		// the old description (if any) stays
		report.result = BuildResult::Failed;
		report.messages = parsed.error;
		return false;
	}
	const EffectDesc &fresh = parsed.effect.desc;
	if (!acceptDescription(fresh, report.messages))
	{
		report.result = BuildResult::Failed;
		report.messages += "\n(the old description and shaders stay)";
		return false;
	}
	// The renderer may hold a copy of the old description; drop its auto-constant plan so the next fill rebuilds it.
	if (m_autos)
		m_autos->forget(fresh);
	const eastl::string text = fresh.text();
	eastl::string		old;
	report.changed = !readText(path, old) || old != text;
	Manifest written;
	written.compiler = m_compiler.version();
	written.frontend = front::C_FRONTEND_VERSION;
	written.policy = m_policyHash;
	written.entry = "description";
	written.variant = "-";
	written.profile = "-";
	written.options = "-";
	written.output = hashOf(text.data(), text.size());
	written.files = parsed.files;
	written.hashes = parsed.hashes;
	const eastl::string manifestText = written.text();
	if ((report.changed && !writeFile(path, text.data(), text.size())) ||
		!writeFile(dep, manifestText.data(), manifestText.size()))
	{
		report.result = BuildResult::Failed;
		report.messages = "cannot write " + path;
		return false;
	}
	report.result = report.changed ? BuildResult::Compiled : BuildResult::UpToDate;
	desc = fresh;
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool ShaderCache::updateVariant(const EffectDesc &desc, const TechniqueDesc &technique, const PassDesc &pass,
								Stage stage, const SelectionValues &values, Path path, bool checkOnly, Round &round,
								BuildEntry &report)
{
	report = BuildEntry();
	report.effect = desc.name;
	report.technique = technique.name;
	report.pass = pass.name;
	report.stage = stage;
	report.values = values;
	report.path = stage == Stage::Mesh || stage == Stage::Amplification ? Path::Meshlets : path;
	if (report.values.size() != desc.selections.size())
		desc.defaults(report.values);
	if (!front::variantOf(desc, pass, stage, report.values, path, report.variant, report.entry, report.messages,
						  m_policy.recordAbi))
	{
		report.result = BuildResult::Failed;
		return false;
	}
	const eastl::string dxil = compiledPath(m_root, desc.name.c_str(), report.variant.c_str(), ".dxil");
	const eastl::string dep = compiledPath(m_root, desc.name.c_str(), report.variant.c_str(), ".dep");
	if (m_mode == Mode::Prebuilt)
		return true;
	Manifest manifest;
	if (!manifest.read(dep))
		report.reason = BuildReason::NoManifest;
	else if (manifest.compiler != m_compiler.version() || manifest.frontend != front::C_FRONTEND_VERSION)
		report.reason = BuildReason::CompilerChanged;
	else if (manifest.policy != m_policyHash || manifest.entry != report.entry ||
			 manifest.profile != profileOf(stage) || manifest.options != C_COMPILE_OPTIONS ||
			 manifest.layout != desc.layoutHash || _stricmp(manifest.files[0].c_str(), desc.source.c_str()) != 0)
		report.reason = BuildReason::OptionsChanged;
	else
	{
		eastl::vector<uint8_t> bytes;
		if (!readFile(dxil.c_str(), bytes) || hashOf(bytes.data(), bytes.size()) != manifest.output)
			report.reason = BuildReason::NoOutput;
		for (size_t i = 0; i < manifest.files.size() && report.reason == BuildReason::UpToDate; ++i)
			if (round.get(manifest.files[i]) != manifest.hashes[i])
			{
				report.reason = i == 0 ? BuildReason::SourceChanged : BuildReason::IncludeChanged;
				report.file = manifest.files[i];
			}
	}
	if (report.reason == BuildReason::UpToDate)
		return true;
	if (checkOnly)
	{
		report.result = BuildResult::Stale;
		return true;
	}
	Round::Parsed &parsed = round.parse(m_compiler, desc.name);
	if (!parsed.ok)
	{
		report.result = BuildResult::Failed;
		report.messages = parsed.error;
		return false;
	}
	front::Entry entry;
	// the pass as the fresh parse has it (the description on disk is the same text when this round updated it)
	const TechniqueDesc *t = parsed.effect.desc.findTechnique(technique.name.c_str());
	const PassDesc		*p = nullptr;
	if (t)
		for (const PassDesc &candidate : t->passes)
			if (candidate.name == pass.name)
				p = &candidate;
	if (!p || !parsed.effect.entry(*p, stage, report.values, path, entry, report.messages))
	{
		if (!p)
			report.messages = desc.source + ": the pass " + technique.name + "/" + pass.name + " is gone";
		report.result = BuildResult::Failed;
		return false;
	}
	Compiler::Result compiled;
	m_compiler.compile(desc.source.c_str(), entry.prefix + parsed.effect.body + entry.text, entry.name.c_str(), stage,
					   compiled);
	report.messages = compiled.messages;
	if (!compiled.ok)
	{
		report.result = BuildResult::Failed;
		return false;
	}
	Manifest written;
	written.compiler = m_compiler.version();
	written.frontend = front::C_FRONTEND_VERSION;
	written.policy = m_policyHash;
	written.entry = report.entry;
	written.variant = variantLine(report);
	written.profile = profileOf(stage);
	written.options = C_COMPILE_OPTIONS;
	written.output = hashOf(compiled.bytecode.data(), compiled.bytecode.size());
	written.layout = parsed.effect.desc.layoutHash;
	written.files = parsed.files;
	written.hashes = parsed.hashes;
	const eastl::string text = written.text();
	if (!writeFile(dxil, compiled.bytecode.data(), compiled.bytecode.size()) ||
		!writeFile(dep, text.data(), text.size()))
	{
		report.result = BuildResult::Failed;
		report.messages = "cannot write " + dxil;
		return false;
	}
	report.result = BuildResult::Compiled;
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
bool ShaderCache::loadEffect(const char *name, EffectDesc &desc, eastl::string &error)
{
	ExclusiveLock		lock(m_lock);
	const eastl::string effect = effectOf(name);
	if (m_mode == Mode::Prebuilt && !deployedEffect(effect))
	{
		error = "effect " + effect + " is not in the deployed set (" + C_DEPLOYED_EFFECTS + ")";
		return false;
	}
	Round				round(m_policy, m_policyHash);
	round.root = m_root;
	BuildEntry report;
	EffectDesc candidate;
	if (updateDescription(effect, false, round, report, candidate))
	{
		if (report.result == BuildResult::Compiled)
			m_compiledOutside.push_back(report);
		desc = eastl::move(candidate);
		return true;
	}
	error = report.messages;
	// An invalid Develop edit can leave a usable cached descriptor. Only publish an intact output from a
	// successful build. The parser may write fields before finding an error, so parse into a temporary.
	if (m_mode == Mode::Develop)
	{
		const eastl::string path = compiledPath(m_root, effect.c_str(), "desc", "");
		Manifest			manifest;
		eastl::string		text, ignored;
		EffectDesc			previous;
		if (manifest.read(path + ".dep") && readText(path, text) &&
			hashOf(text.data(), text.size()) == manifest.output && previous.parse(text.data(), text.size(), ignored) &&
			previous.name == effect && previous.entryModel == m_policy.entries &&
			(previous.policyHash == m_policyHash ||
			 (previous.policyHash == 0 && previous.entryModel == EntryModel::RendererRecords)) &&
			acceptDescription(previous, ignored))
			desc = eastl::move(previous);
	}
	return false;
}

//----------------------------------------------------------------------------------------------------------------------
bool ShaderCache::getShader(const char *effect, const char *technique, const char *pass, Stage stage,
							const SelectionValues &values, eastl::vector<uint8_t> &bytecode, BuildEntry &report,
							Path path)
{
	ExclusiveLock lock(m_lock);
	report = BuildEntry();
	const eastl::string name = effectOf(effect);
	report.effect = name;
	report.technique = technique;
	report.pass = pass;
	report.stage = stage;
	Round round(m_policy, m_policyHash);
	round.root = m_root;
	EffectDesc desc;
	BuildEntry descReport;
	const bool fresh = updateDescription(name, false, round, descReport, desc);
	if (!fresh)
	{
		// Develop: after a broken source or a refused layout, serve the variant the old description names. A Prebuilt
		// cache has nothing else to fall back to.
		report.result = BuildResult::Failed;
		report.messages = descReport.messages;
		eastl::string ignored;
		if (m_mode != Mode::Develop ||
			!loadDescription(desc, compiledPath(m_root, name.c_str(), "desc", ""), ignored) ||
			!acceptDescription(desc, ignored))
			return false;
	}
	else if (descReport.result == BuildResult::Compiled)
		m_compiledOutside.push_back(descReport);
	const TechniqueDesc *t = desc.findTechnique(technique);
	const PassDesc		*p = nullptr;
	if (t)
		for (const PassDesc &candidate : t->passes)
			if (candidate.name == pass)
				p = &candidate;
	if (!p)
	{
		report.result = BuildResult::Failed;
		report.messages = name + ": no pass " + technique + "/" + pass;
		return false;
	}
	// Vertices (the default) for a pass not drawn from its own vertices: its one geometry path
	if (path == Path::Vertices && !p->vertices && p->shader(Stage::Compute).empty())
	{
		eastl::vector<Path> paths;
		front::geometryPaths(*p, paths);
		if (!paths.empty())
			path = paths[0];
	}
	// A combination the pass pins out has no pipeline. The error quotes the pins so the author can widen them.
	{
		SelectionValues full = values;
		if (full.size() != desc.selections.size())
			desc.defaults(full);
		eastl::string which;
		if (!front::pinned(desc, *p, full, which))
		{
			report.result = BuildResult::Failed;
			report.messages = desc.source + ": " + technique + "/" + pass + ": " + which +
							  " is not among the pass's pipelines (" + front::pinText(desc, *p) +
							  "); to draw it, widen the pin in the pass";
			return false;
		}
	}
	BuildEntry variant;
	if (fresh)
	{
		updateVariant(desc, *t, *p, stage, values, path, false, round, variant);
		if (variant.result == BuildResult::Compiled)
		{
			m_compiledOutside.push_back(variant); // reported by the next updateAll
			++m_compiledCount;
		}
		report = variant;
	}
	else
	{
		report.values = values;
		if (report.values.size() != desc.selections.size())
			desc.defaults(report.values);
		eastl::string ignored;
		front::variantOf(desc, *p, stage, report.values, path, report.variant, report.entry, ignored,
						 m_policy.recordAbi);
	}
	if (report.variant.empty())
		return false;
	// The bytecode must match the record layout of the description it is served with. A variant built for another
	// layout (left behind when a rebuild after a field move failed) is not served.
	Manifest manifest;
	if (m_mode == Mode::Develop && manifest.read(compiledPath(m_root, name.c_str(), report.variant.c_str(), ".dep")) &&
		(manifest.layout != desc.layoutHash || manifest.policy != desc.policyHash))
	{
		report.result = BuildResult::Failed;
		if (report.messages.empty())
			report.messages = name + "." + report.variant + ": built for another layout of the records";
		report.messages += "\n(the compiled variant uses another record layout or shader policy: not served)";
		return false;
	}
	const eastl::string bytecodePath = compiledPath(m_root, name.c_str(), report.variant.c_str(), ".dxil");
	if (!readFile(bytecodePath.c_str(), bytecode))
	{
		report.result = BuildResult::Failed;
		if (!report.messages.empty())
			report.messages += '\n';
		report.messages += "no compiled file " + bytecodePath + "; rebuild and deploy shaders for this renderer ABI";
		return false;
	}
	return true;
}

bool ShaderCache::getShader(const char *effect, const char *function, Stage stage, eastl::vector<uint8_t> &bytecode,
							BuildEntry &report)
{
	EffectDesc	  desc;
	eastl::string error;
	// On failure loadEffect leaves desc empty, or in Develop the last intact description, which still names the passes.
	if (!loadEffect(effect, desc, error) && desc.name.empty())
	{
		report = BuildEntry();
		report.effect = effect;
		report.result = BuildResult::Failed;
		report.messages = error;
		return false;
	}
	for (const TechniqueDesc &t : desc.techniques)
		for (const PassDesc &p : t.passes)
			if (p.shader(stage).function == function)
				return getShader(effect, t.name.c_str(), p.name.c_str(), stage, SelectionValues(), bytecode, report);
	report = BuildEntry();
	report.effect = effect;
	report.result = BuildResult::Failed;
	report.messages = eastl::string(effect) + ": no pass with the function " + function;
	return false;
}

//----------------------------------------------------------------------------------------------------------------------
void ShaderCache::updateAll(bool checkOnly, eastl::vector<BuildEntry> &report, Variants variants)
{
	report.clear();
	{
		PSRWLOCK lock = reinterpret_cast<PSRWLOCK>(&m_aliasLock);
		AcquireSRWLockExclusive(lock);
		m_aliasesValid = false; // re-read aliases.txt: it may have changed since the last round
		ReleaseSRWLockExclusive(lock);
	}
	if (!checkOnly)
	{
		ExclusiveLock lock(m_lock);
		report.swap(m_compiledOutside); // built by getShader/loadEffect since the last round
	}
	Round round(m_policy, m_policyHash);
	round.root = m_root;
	eastl::vector<eastl::string> names;
	listEffects(names);
	for (const eastl::string &name : names)
	{
		if (effectOf(name.c_str()) != name)
			continue; // an alias: built under its effect's name
		EffectDesc desc;
		bool	   ok;
		{
			// the lock per step, not per round: the renderer's getShader waits for one build at most
			ExclusiveLock lock(m_lock);
			ok = updateDescription(name, checkOnly, round, report.push_back(), desc);
		}
		if (!ok)
			continue;
		eastl::vector<eastl::string> done;
		auto						 build =
			[&](const TechniqueDesc &t, const PassDesc &p, Stage stage, const SelectionValues &values, Path path)
		{
			eastl::string variant, entry, error;
			if (!front::variantOf(desc, p, stage, values, path, variant, entry, error, m_policy.recordAbi) ||
				eastl::find(done.begin(), done.end(), variant) != done.end())
				return;
			// Variants::Existing (the watcher) rebuilds only variants built before, i.e. those the application asked
			// for. They are found by recomputing the ids of every pass, so a renamed technique or pass still finds a
			// file that another pass shares.
			if (variants == Variants::Existing &&
				GetFileAttributesA(compiledPath(m_root, desc.name.c_str(), variant.c_str(), ".dep").c_str()) ==
					INVALID_FILE_ATTRIBUTES)
				return;
			done.push_back(variant);
			ExclusiveLock lock(m_lock);
			updateVariant(desc, t, p, stage, values, path, checkOnly, round, report.push_back());
		};
		// the stages of each geometry path the pass has (front::geometryPaths: what pipelines() lists), each at the
		// values of the selections it reads, within the pass's pins (front::combinations)
		for (const TechniqueDesc &t : desc.techniques)
			for (const PassDesc &p : t.passes)
			{
				eastl::vector<Path> paths;
				front::geometryPaths(p, paths);
				for (Path path : paths)
				{
					Stage	 stages[3];
					uint32_t count = 0;
					if (!p.shader(Stage::Compute).empty())
						stages[count++] = Stage::Compute;
					else if (path == Path::Meshlets)
					{
						stages[count++] = Stage::Mesh;
						stages[count++] = Stage::Amplification;
						stages[count++] = Stage::Pixel;
					}
					else
					{
						stages[count++] = Stage::Vertex;
						stages[count++] = Stage::Pixel;
					}
					for (uint32_t i = 0; i < count; ++i)
					{
						const Stage stage = stages[i];
						// the meshlet path's mesh and amplification entries are generated from the vertex function
						const bool generated = p.shader(stage).empty() && p.meshlets &&
											   (stage == Stage::Mesh || stage == Stage::Amplification);
						if (p.shader(stage).empty() && !generated)
							continue;
						eastl::vector<SelectionValues> all;
						front::combinations(desc, p, stage, all);
						for (const SelectionValues &values : all)
							build(t, p, stage, values, path);
					}
				}
			}
	}
}

//----------------------------------------------------------------------------------------------------------------------
bool ShaderCache::emit(const char *effect, eastl::string &out, eastl::string &error)
{
	ExclusiveLock lock(m_lock);
	out.clear();
	Round round(m_policy, m_policyHash);
	round.root = m_root;
	Round::Parsed &parsed = round.parse(m_compiler, effectOf(effect));
	if (!parsed.ok)
	{
		error = parsed.error;
		return false;
	}
	const EffectDesc &desc = parsed.effect.desc;
	for (const TechniqueDesc &t : desc.techniques)
		for (const PassDesc &p : t.passes)
		{
			if (p.shader(Stage::Vertex).empty())
				continue;
			// the entry of the pass's geometry path that runs its vertex function (the mesh entry of a meshlet pass):
			// the same link from the vertex function's outputs (rbVertex) to the pixel stage's inputs (rbLink)
			eastl::vector<Path> paths;
			front::geometryPaths(p, paths);
			const Path	 path = paths.empty() ? Path::Vertices : paths[0];
			front::Entry entry;
			if (!parsed.effect.entry(p, path == Path::Meshlets ? Stage::Mesh : Stage::Vertex, SelectionValues(), path,
									 entry, error))
				return false;
			out += "pass " + t.name + " " + p.name + " vs=" + entry.name + "\n" + entry.text + "\n";
		}
	return true;
}

bool ShaderCache::pipelines(const char *effect, eastl::vector<PipelineDesc> &out, eastl::string &error)
{
	EffectDesc desc;
	if (!loadEffect(effect, desc, error))
		return false;
	front::pipelines(desc, out, m_policy.recordAbi);
	return true;
}

namespace
{
// a file of the folder by its name under the root (forward slashes)
eastl::string underRoot(const eastl::string &root, const eastl::string &file)
{
	eastl::string path = root + "\\" + file;
	for (char &c : path)
		if (c == '/')
			c = '\\';
	return path;
}
} // namespace

void ShaderCache::setRemapTable(const char *table, const char *extraEffects)
{
	ExclusiveLock lock(m_lock);
	m_remapTable = table != nullptr ? table : "";
	m_remapNames = extraEffects != nullptr ? extraEffects : "";
	m_remapLoaded = false; // read at the next remap or remapChanged
}

eastl::string ShaderCache::remapTable() const
{
	ExclusiveLock lock(m_lock);
	return m_remapTable;
}

bool ShaderCache::validateRemap(const eastl::vector<RemapRow> &rows, const eastl::vector<eastl::string> &passthrough,
								const eastl::string &table, const eastl::string &extraNamesFile, eastl::string &error)
{
	// Every name the table uses must exist: the originals (effects or aliases of the tree, or names from the extra
	// effect names file), the target effects and techniques, and the targets' parameters and selections with the
	// right number of values. No original may have two rows.
	eastl::vector<eastl::string> names;
	listEffects(names);
	auto known = [&](const eastl::string &n) { return eastl::find(names.begin(), names.end(), n) != names.end(); };
	// The extra effect names file holds words separated by blanks; '#' starts a comment. Without the file (none set,
	// or a deployed folder without it) the originals are not checked.
	eastl::vector<eastl::string> extraNames;
	eastl::vector<uint8_t>		 listBytes;
	const bool haveList = !extraNamesFile.empty() && readFile(underRoot(m_root, extraNamesFile).c_str(), listBytes);
	if (haveList)
	{
		eastl::string word;
		bool		  comment = false;
		for (uint8_t c : listBytes)
		{
			if (c == '#')
				comment = true;
			if (c == '\n')
				comment = false;
			if (!comment && c > ' ')
				word.push_back(char(c));
			else if (!word.empty())
			{
				extraNames.push_back(word);
				word.clear();
			}
		}
		if (!word.empty())
			extraNames.push_back(word);
	}
	for (const eastl::string &n : passthrough)
		if (!known(n))
			return error = table + ": passthrough names " + n + ", which is no effect", false;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const RemapRow	   &row = rows[i];
		const eastl::string at = row.where + ": error: ";
		if (haveList && row.original != "*" && !known(row.original) &&
			eastl::find(extraNames.begin(), extraNames.end(), row.original) == extraNames.end())
			return error = at + row.original + " is no effect of the tree and not in " + extraNamesFile, false;
		for (size_t j = 0; j < i; ++j)
			if (rows[j].original == row.original && rows[j].originalTechnique == row.originalTechnique)
				return error = at + row.original + " " + row.originalTechnique + " has a row already (" +
							   rows[j].where + ")",
					   false;
		EffectDesc	  desc;
		eastl::string why;
		if (!loadEffect(row.effect.c_str(), desc, why))
			return error = at + "the effect " + row.effect + " does not load: " + why, false;
		if (!desc.findTechnique(row.technique.c_str()))
			return error = at + row.effect + " has no technique " + row.technique, false;
		for (const auto &sel : row.selections)
			if (desc.findSelection(sel.first.c_str()) < 0)
				return error = at + row.effect + " has no selection " + sel.first, false;
		for (const RemapRow::Value &v : row.values)
		{
			const ParamDesc *param = desc.findParam(v.param.c_str());
			if (!param)
				return error = at + row.effect + " has no parameter " + v.param, false;
			const uint32_t components = param->size / 4 / (param->count ? param->count : 1);
			if (param->offset >= 0 && components && v.values.size() != components)
			{
				char text[160];
				snprintf(text, sizeof text, "%s is %s: %u values, the row gives %u", v.param.c_str(),
						 param->type.c_str(), components, static_cast<uint32_t>(v.values.size()));
				return error = at + text, false;
			}
		}
		for (const auto &m : row.params)
			if (!desc.findParam(m.second.c_str()))
				return error = at + row.effect + " has no parameter " + m.second + " (fed by " + m.first + ")", false;
	}
	return true;
}

bool ShaderCache::loadRemap(eastl::string &error, bool retryRejected)
{
	// The table is read and checked outside the lock (the check loads descriptions, which takes it) and stored under
	// it. The file names are copied under the lock because setRemapTable may change them.
	eastl::string table, extraNamesFile;
	{
		ExclusiveLock lock(m_lock);
		table = m_remapTable;
		extraNamesFile = m_remapNames;
	}
	eastl::vector<uint8_t> bytes;
	const bool			   read = !table.empty() && readFile(underRoot(m_root, table).c_str(), bytes);
	const uint64_t		   hash = read ? hashOf(bytes.data(), bytes.size()) : 0;
	{
		ExclusiveLock lock(m_lock);
		if (m_remapLoaded && hash == m_remapHash && (!retryRejected || m_remapError.empty()))
		{
			error = m_remapError;
			return error.empty();
		}
	}
	eastl::vector<RemapRow>		 rows;
	eastl::vector<eastl::string> passthrough;
	eastl::string				 problem;
	if (!read)
		problem = table.empty() ? eastl::string("no remap table set (ShaderCache::setRemapTable)")
								: table + ": no remap table";
	else
	{
		// Skip a UTF-8 byte order mark.
		size_t start = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
		const eastl::string text(reinterpret_cast<const char *>(bytes.data()) + start, bytes.size() - start);
		if (parseRemap(table.c_str(), text, rows, passthrough, problem))
			validateRemap(rows, passthrough, table, extraNamesFile, problem);
	}
	ExclusiveLock lock(m_lock);
	m_remapLoaded = true;
	m_remapHash = hash;
	m_remapError = problem;
	if (problem.empty())
	{
		m_remap.swap(rows);
		m_passthrough.swap(passthrough);
		m_remapLogged.clear();
	}
	// If the table fails to read or validate, the rows read before stay in use.
	error = problem;
	return problem.empty();
}

bool ShaderCache::hasEffect(const char *name)
{
	if (name == nullptr || *name == 0)
		return false;
	eastl::string error;
	bool		  readRemap;
	{
		ExclusiveLock lock(m_lock);
		readRemap = !m_remapLoaded && !m_remapTable.empty();
	}
	if (readRemap)
		loadRemap(error);
	const eastl::string target = effectOf(name);
	{
		ExclusiveLock lock(m_lock);
		// A bad hot edit leaves the last validated rows active for rendering and for availability.
		for (const RemapRow &row : m_remap)
			if (row.original != "*" && (row.original == name || row.original == target))
				return true;
	}
	EffectDesc desc;
	loadEffect(name, desc, error);
	return !desc.name.empty() && desc.name == target;
}

bool ShaderCache::remap(const char *effect, const char *technique, RemapRow &out, eastl::string &error)
{
	bool loaded;
	{
		ExclusiveLock lock(m_lock);
		loaded = m_remapLoaded;
	}
	// The table is read once here. Later edits arrive through remapChanged (the watcher), not a file check per lookup.
	eastl::string ignored;
	if (!loaded)
		loadRemap(ignored);
	eastl::vector<eastl::string> names;
	listEffects(names);
	const eastl::string target = effectOf(effect);
	ExclusiveLock		lock(m_lock);
	if (m_remap.empty())
	{
		error = m_remapError;
		return false;
	}
	out = RemapRow();
	const eastl::string name = effect;
	auto				said = [&](const char *what)
	{
		// Logs what an unknown name gets, once per name.
		if (eastl::find(m_remapLogged.begin(), m_remapLogged.end(), name) != m_remapLogged.end())
			return;
		m_remapLogged.push_back(name);
		char line[320];
		snprintf(line, sizeof line, "render_backend shaders: %s %s is in no row of %s: %s\n", effect, technique,
				 m_remapTable.c_str(), what);
		if (m_log)
			m_log(line, m_logUser);
		else
			OutputDebugStringA(line);
	};
	auto passThrough = [&]()
	{
		out.original = out.effect = name;
		out.originalTechnique = out.technique = technique;
		out.passthrough = true;
		return true;
	};
	// A passthrough name is drawn as it is.
	if (eastl::find(m_passthrough.begin(), m_passthrough.end(), name) != m_passthrough.end())
		return passThrough();
	// Search the effect's own rows by technique, then '*' (any technique), then DEFAULT (D3D9 drew an unknown
	// technique with the effect's first valid one). Then repeat for the effect an alias names.
	auto find = [&](const eastl::string &e, const char *t) -> const RemapRow *
	{
		for (const RemapRow &row : m_remap)
			if (row.original == e && row.originalTechnique == t)
				return &row;
		return nullptr;
	};
	const RemapRow *found = nullptr;
	for (const eastl::string &e : {name, target})
		for (const char *t : {technique, "*", "DEFAULT"})
			if (!found)
				found = find(e, t);
	if (!found)
	{
		// An effect of the tree that no row names is drawn as it is; the log says so once.
		if (eastl::find(names.begin(), names.end(), name) != names.end())
		{
			said("drawn as it is (an effect of the tree; add it to the passthrough line or give it a row)");
			return passThrough();
		}
		found = find("*", technique);
		if (!found)
			found = find("*", "*");
		if (!found)
		{
			error = eastl::string(effect) + " " + technique + ": no row and no default row in " + m_remapTable;
			return false;
		}
		said("no such effect, drawn by the default row");
		out = *found;
		out.fallback = true;
		return true;
	}
	out = *found;
	return true;
}

bool ShaderCache::remapChanged(eastl::string &error)
{
	error.clear();
	if (remapTable().empty())
		return false; // no table: nothing to watch
	loadRemap(error, true);
	ExclusiveLock lock(m_lock);
	const bool	  valid = m_remapError.empty();
	if (m_remapHash == m_remapSeen && valid == m_remapSeenValid)
		return false;
	m_remapSeen = m_remapHash;
	m_remapSeenValid = valid;
	return true;
}

bool ShaderCache::findPipeline(const EffectDesc &desc, const eastl::vector<PipelineDesc> &list, const char *technique,
							   const char *pass, Path path, const SelectionValues &values, const PipelineDesc *&out,
							   eastl::string &error)
{
	out = nullptr;
	const TechniqueDesc *t = desc.findTechnique(technique);
	const PassDesc		*p = nullptr;
	if (t)
		for (const PassDesc &candidate : t->passes)
			if (candidate.name == pass)
				p = &candidate;
	if (!p)
	{
		error = desc.name + ": no pass " + technique + "/" + pass;
		return false;
	}
	SelectionValues full = values;
	if (full.size() != desc.selections.size())
		desc.defaults(full);
	eastl::string which;
	if (!front::pinned(desc, *p, full, which))
	{
		error = desc.source + ": " + technique + "/" + pass + ": " + which + " is not among the pass's pipelines (" +
				front::pinText(desc, *p) + "); to draw it, widen the pin in the pass";
		return false;
	}
	// the selections the pass reads decide; pipelines() keeps the others at their defaults
	eastl::vector<int32_t> used;
	front::passSelections(desc, *p, used);
	SelectionValues reduced;
	desc.defaults(reduced);
	for (int32_t s : used)
		reduced[s] = full[s];
	for (const PipelineDesc &pd : list)
		if (pd.technique == technique && pd.pass == pass && pd.path == path && pd.values == reduced)
		{
			if (out)
			{
				// a pass drawn into several target sets: which one is the caller's (pick from the list by targets)
				error = desc.name + ": " + technique + "/" + pass + " has a pipeline per target set (" +
						out->targets.name + ", " + pd.targets.name + "...): choose by PipelineDesc::targets";
				out = nullptr;
				return false;
			}
			out = &pd;
		}
	if (out)
		return true;
	error = desc.name + ": " + technique + "/" + pass + " is not drawn " +
			(path == Path::Meshlets ? "as meshlets"
			 : path == Path::Pool	? "from the pools"
									: "from its own vertices") +
			" (front::geometryPaths: one path per pass; its own vertices besides it with \"Vertices = true;\")";
	return false;
}

bool ShaderCache::pipelineShaders(const char *effect, const PipelineDesc					  &pd,
								  eastl::vector<uint8_t> (&bytecode)[C_STAGES], eastl::string &error)
{
	for (uint32_t st = 0; st < C_STAGES; ++st)
	{
		bytecode[st].clear();
		if (pd.shaders[st].empty())
			continue;
		BuildEntry report;
		if (!getShader(effect, pd.technique.c_str(), pd.pass.c_str(), static_cast<Stage>(st), pd.values, bytecode[st],
					   report, pd.path) ||
			report.variant != pd.shaders[st])
		{
			error = eastl::string(effect) + ": " + pd.technique + "/" + pd.pass + ": the variant " + pd.shaders[st] +
					(report.messages.empty() ? eastl::string(" is not the one served (") + report.variant + ")"
											 : ": " + report.messages);
			return false;
		}
	}
	return true;
}

bool ShaderCache::deploy(const char *dir, uint32_t &files, eastl::string &error) const
{
	files = 0;
	const eastl::string to = eastl::string(dir) + "\\effects";
	CreateDirectoryA(dir, nullptr);
	CreateDirectoryA(to.c_str(), nullptr);
	const eastl::string bytecodePattern = eastl::string("*") + C_BYTECODE_ABI_SUFFIX + ".dxil";
	for (const char *pattern : {"*.desc", bytecodePattern.c_str(), "aliases.txt", "*.remap"})
	{
		WIN32_FIND_DATAA found;
		HANDLE			 search = FindFirstFileA((m_root + "\\effects\\" + pattern).c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			const eastl::string from = m_root + "\\effects\\" + found.cFileName;
			if (!CopyFileA(from.c_str(), (to + "\\" + found.cFileName).c_str(), FALSE))
			{
				error = "cannot copy " + from + " to " + to;
				FindClose(search);
				return false;
			}
			++files;
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	// The deployed list names the effects of the source tree (listEffects in Develop mode) without the aliases, which
	// aliases.txt carries.
	eastl::vector<eastl::string> effects;
	listEffects(effects);
	eastl::vector<eastl::pair<eastl::string, eastl::string>> aliases;
	readAliases(aliases);
	eastl::string list;
	for (const eastl::string &effect : effects)
	{
		bool alias = false;
		for (const auto &a : aliases)
			alias = alias || a.first == effect;
		if (!alias)
			list += effect + "\n";
	}
	const eastl::string listPath = to + "\\" + C_DEPLOYED_EFFECTS;
	if (!writeFile(listPath, list.data(), list.size()))
	{
		error = "cannot write " + listPath;
		return false;
	}
	++files;
	return true;
}

uint64_t ShaderCache::shaderSetHash() const
{
	eastl::vector<eastl::string> files;
	WIN32_FIND_DATAA			 found;
	const eastl::string			 pattern = m_root + "\\effects\\*" + C_BYTECODE_ABI_SUFFIX + ".dxil";
	HANDLE						 search = FindFirstFileA(pattern.c_str(), &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			// Prebuilt: only files of deployed effects (<effect>.<variant>...dxil) count, so files left by an older
			// deployment do not change the hash.
			const eastl::string name = found.cFileName;
			if (m_mode == Mode::Prebuilt && !deployedEffect(name.substr(0, name.find('.'))))
				continue;
			files.push_back(name);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	eastl::sort(files.begin(), files.end());
	uint64_t hash = 14695981039346656037ull;
	for (const eastl::string &f : files)
	{
		eastl::vector<uint8_t> bytes;
		readFile((m_root + "\\effects\\" + f).c_str(), bytes);
		const uint64_t h = hashOf(bytes.data(), bytes.size());
		for (char c : f)
			hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ull;
		for (int i = 0; i < 8; ++i)
			hash = (hash ^ static_cast<uint8_t>(h >> (i * 8))) * 1099511628211ull;
	}
	return hash;
}

bool ShaderCache::layoutHeaders(const LayoutHeaderPolicy &headers, const eastl::vector<eastl::string> &layerEffects,
								eastl::string &layer, eastl::string &host, eastl::string &error)
{
	eastl::vector<eastl::string> names;
	listEffects(names);
	eastl::vector<EffectDesc> descs;
	for (const eastl::string &name : names)
	{
		if (effectOf(name.c_str()) != name)
			continue;
		EffectDesc &desc = descs.push_back();
		if (!loadEffect(name.c_str(), desc, error))
			return false;
	}
	// The shared records come from the prelude; parsing any one effect yields them.
	eastl::vector<StructDesc> standard;
	{
		ExclusiveLock lock(m_lock);
		Round		  round(m_policy, m_policyHash);
		round.root = m_root;
		Round::Parsed &parsed = round.parse(m_compiler, descs.empty() ? eastl::string() : descs[0].name);
		if (!parsed.ok)
		{
			error = parsed.error;
			return false;
		}
		parsed.effect.standardRecords(headers.sharedRecords, headers.sharedRecordCount, standard);
	}
	front::layoutHeaders(headers, m_policy.recordAbi, descs, standard, layerEffects, layer, host);
	return true;
}

} // namespace render_backend::shader
