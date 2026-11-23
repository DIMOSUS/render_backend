// Worker thread of the shader watcher (watcher.hpp). It watches the source tree with ReadDirectoryChangesW, waits
// for a short quiet period after the last change (editors often write a file in several steps), runs
// ShaderCache::updateAll and queues the results for poll().
#include <render_backend/shader/shader_cache.hpp>
#include <render_backend/shader/watcher.hpp>
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace render_backend::shader
{
namespace
{
// A change is acted on once the tree has been quiet this long.
constexpr DWORD C_QUIET_MS = 150;

// A pointer-sized member viewed as the Win32 lock (zero = unlocked).
PSRWLOCK srwOf(void *&member)
{
	static_assert(sizeof(SRWLOCK) == sizeof(void *), "SRWLOCK is one pointer");
	return reinterpret_cast<PSRWLOCK>(&member);
}

bool endsWith(const eastl::string &text, const char *suffix)
{
	const size_t n = strlen(suffix);
	return text.size() >= n && _stricmp(text.c_str() + text.size() - n, suffix) == 0;
}

// The file name of a FILE_NOTIFY_INFORMATION record, relative to the watched folder, with '/' separators.
eastl::string recordName(const FILE_NOTIFY_INFORMATION &record)
{
	const int	  chars = static_cast<int>(record.FileNameLength / sizeof(WCHAR));
	const int	  n = WideCharToMultiByte(CP_ACP, 0, record.FileName, chars, nullptr, 0, nullptr, nullptr);
	eastl::string name(n > 0 ? n : 0, '\0');
	if (n > 0)
		WideCharToMultiByte(CP_ACP, 0, record.FileName, chars, name.data(), n, nullptr, nullptr);
	for (char &c : name)
		if (c == '\\')
			c = '/';
	return name;
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
Watcher::~Watcher()
{
	stop();
}

//----------------------------------------------------------------------------------------------------------------------
bool Watcher::start(ShaderCache &cache, eastl::string &error)
{
	stop();
	if (cache.mode() != ShaderCache::Mode::Develop)
	{
		error = "the watcher needs a source tree (ShaderCache::Mode::Develop)";
		return false;
	}
	m_cache = &cache;
	{
		// Record the current state of the remap table, so only later edits produce a Remap change.
		eastl::string ignored;
		cache.remapChanged(ignored);
	}
	m_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	m_readyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (m_stopEvent && m_readyEvent)
		m_thread = CreateThread(nullptr, 0, &Watcher::threadMain, this, 0, nullptr);
	if (!m_thread)
	{
		error = "cannot start the shader watcher thread";
		stop();
		return false;
	}
	// Wait until the worker watches, so a change made right after start() returns is seen.
	WaitForSingleObject(m_readyEvent, 5000);
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
void Watcher::stop()
{
	if (m_thread)
	{
		SetEvent(m_stopEvent);
		WaitForSingleObject(m_thread, INFINITE);
		CloseHandle(m_thread);
		m_thread = nullptr;
	}
	if (m_stopEvent)
	{
		CloseHandle(m_stopEvent);
		m_stopEvent = nullptr;
	}
	if (m_readyEvent)
	{
		CloseHandle(m_readyEvent);
		m_readyEvent = nullptr;
	}
}

//----------------------------------------------------------------------------------------------------------------------
uint32_t Watcher::poll(eastl::vector<ShaderChange> &changes)
{
	changes.clear();
	AcquireSRWLockExclusive(srwOf(m_lock));
	changes.swap(m_pending);
	const uint32_t completed = rounds();
	ReleaseSRWLockExclusive(srwOf(m_lock));
	return completed;
}

//----------------------------------------------------------------------------------------------------------------------
uint32_t Watcher::rounds() const
{
	return static_cast<uint32_t>(InterlockedCompareExchange(const_cast<volatile long *>(&m_rounds), 0, 0));
}

//----------------------------------------------------------------------------------------------------------------------
unsigned long __stdcall Watcher::threadMain(void *self)
{
	static_cast<Watcher *>(self)->run();
	return 0;
}

//----------------------------------------------------------------------------------------------------------------------
void Watcher::run()
{
	auto fail = [this](const eastl::string &message)
	{
		ShaderChange failure;
		failure.kind = ShaderChange::Kind::Error;
		failure.message = message;
		AcquireSRWLockExclusive(srwOf(m_lock));
		m_pending.push_back(failure);
		ReleaseSRWLockExclusive(srwOf(m_lock));
		SetEvent(m_readyEvent);
	};
	HANDLE folder = CreateFileA(m_cache->root().c_str(), FILE_LIST_DIRECTORY,
								FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
								FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	if (folder == INVALID_HANDLE_VALUE)
	{
		fail("cannot watch " + m_cache->root());
		return;
	}
	auto isFolder = [this](const eastl::string &name)
	{
		const DWORD attributes = GetFileAttributesA((m_cache->root() + "/" + name).c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	};
	alignas(DWORD) uint8_t buffer[16384];
	OVERLAPPED			   overlapped{};
	overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!overlapped.hEvent)
	{
		CloseHandle(folder);
		fail("cannot watch " + m_cache->root() + ": no event for the directory read");
		return;
	}
	bool dirty = false;
	bool pending = false;
	bool aliasesChanged = false;
	for (;;)
	{
		if (!pending)
		{
			ResetEvent(overlapped.hEvent);
			pending = ReadDirectoryChangesW(folder, buffer, sizeof buffer, TRUE,
											FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
												FILE_NOTIFY_CHANGE_SIZE,
											nullptr, &overlapped, nullptr) != 0;
			SetEvent(m_readyEvent);
		}
		HANDLE waits[2] = {m_stopEvent, overlapped.hEvent};
		// If the read could not be issued, retry after a quiet period instead of waiting forever.
		const DWORD woke =
			WaitForMultipleObjects(pending ? 2 : 1, waits, FALSE, dirty || !pending ? C_QUIET_MS : INFINITE);
		if (woke == WAIT_OBJECT_0)
			break;
		if (woke == WAIT_TIMEOUT)
		{
			if (!dirty)
				continue; // only a retry of an unissued read
			rebuild(aliasesChanged);
			dirty = false;
			aliasesChanged = false;
			continue;
		}
		if (woke != WAIT_OBJECT_0 + 1)
		{
			char text[96];
			snprintf(text, sizeof text, "the shader watcher stopped: wait failed (error %lu)", GetLastError());
			fail(text);
			break;
		}
		DWORD got = 0;
		pending = false;
		if (!GetOverlappedResult(folder, &overlapped, &got, FALSE) || got == 0)
		{
			// Buffer overflow or error: the file names are lost. Rebuild anyway; the manifests find what is stale.
			dirty = true;
			aliasesChanged = true;
			continue;
		}
		for (DWORD offset = 0;;)
		{
			// FILE_NOTIFY_INFORMATION records are DWORD-aligned and chained by NextEntryOffset.
			const auto	 &record = *reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(buffer + offset);
			eastl::string name = recordName(record);
			// Any file except the cache's own outputs counts: a manifest may name an include of any extension.
			// Folders are ignored. A folder's last-write time changes when the cache writes its outputs into it, and
			// that record arrives after the round that wrote them; acting on it would run an extra round and deliver
			// the next edit's changes one round late. A file's own change has its own record.
			const bool output =
				endsWith(name, ".dxil") || endsWith(name, ".dep") || endsWith(name, ".tmp") || endsWith(name, ".desc");
			if (!output && !isFolder(name))
				dirty = true;
			if (_stricmp(name.c_str(), "effects/aliases.txt") == 0)
				aliasesChanged = true;
			if (!record.NextEntryOffset)
				break;
			offset += record.NextEntryOffset;
		}
	}
	if (pending)
	{
		CancelIoEx(folder, &overlapped);
		DWORD got = 0;
		GetOverlappedResult(folder, &overlapped, &got, TRUE);
	}
	CloseHandle(overlapped.hEvent);
	CloseHandle(folder);
}

//----------------------------------------------------------------------------------------------------------------------
void Watcher::rebuild(bool aliasesChanged)
{
	// Rebuild the descriptions, then the variants the application has asked for so far.
	eastl::vector<BuildEntry> report;
	m_cache->updateAll(false, report, ShaderCache::Variants::Existing);
	eastl::vector<ShaderChange> changes;
	if (aliasesChanged)
		changes.push_back().kind = ShaderChange::Kind::Aliases;
	// The remap table (none set: no remap changes). Its file name keys its error state; its stem is the effect name
	// that its changes carry (variant "remap").
	const eastl::string table = m_cache->remapTable();
	const eastl::string remapKey = table.substr(table.find_last_of('/') + 1);
	const eastl::string remapName = remapKey.substr(0, remapKey.find_last_of('.'));
	// Effects whose description failed this round: their variants were not examined, so their errors stay.
	eastl::vector<eastl::string> broken;
	for (const BuildEntry &entry : report)
		if (entry.description && entry.result == BuildResult::Failed)
			broken.push_back(entry.effect);
	// Clear errors of variants the report no longer names (the pass is gone or no longer built).
	for (size_t i = 0; i < m_errors.size();)
	{
		if (!table.empty() && m_errors[i].first == remapKey)
		{
			++i; // handled below
			continue;
		}
		const size_t		dot = m_errors[i].first.find('.');
		const eastl::string effect = m_errors[i].first.substr(0, dot);
		bool				named = eastl::find(broken.begin(), broken.end(), effect) != broken.end() &&
									m_errors[i].first.substr(dot + 1) != "desc";
		for (const BuildEntry &entry : report)
			named = named || m_errors[i].first == entry.effect + "." + entry.variant;
		if (named)
		{
			++i;
			continue;
		}
		ShaderChange &cleared = changes.push_back();
		cleared.kind = ShaderChange::Kind::Cleared;
		cleared.effect = effect;
		cleared.variant = m_errors[i].first.substr(dot + 1);
		m_errors.erase(m_errors.begin() + i);
	}
	for (const BuildEntry &entry : report)
	{
		// Error state per "effect.variant" ("effect.desc" for the description). An error is sent once per distinct
		// message; Cleared is sent when the entry builds again or is up to date again (a source restored to the
		// recorded text).
		const eastl::string key = entry.effect + "." + entry.variant;
		auto				known = m_errors.begin();
		while (known != m_errors.end() && known->first != key)
			++known;
		if (entry.result == BuildResult::Failed)
		{
			if (known != m_errors.end() && known->second == entry.messages)
				continue;
			if (known != m_errors.end())
				known->second = entry.messages;
			else
				m_errors.push_back(eastl::make_pair(key, entry.messages));
		}
		else if (known != m_errors.end())
		{
			m_errors.erase(known);
			ShaderChange &cleared = changes.push_back();
			cleared.kind = ShaderChange::Kind::Cleared;
			cleared.effect = entry.effect;
			cleared.technique = entry.technique;
			cleared.pass = entry.pass;
			cleared.stage = entry.stage;
			cleared.values = entry.values;
			cleared.entry = entry.entry;
			cleared.variant = entry.variant;
		}
		if (entry.result != BuildResult::Compiled && entry.result != BuildResult::Failed)
			continue;
		ShaderChange &change = changes.push_back();
		change.effect = entry.effect;
		change.technique = entry.technique;
		change.pass = entry.pass;
		change.stage = entry.stage;
		change.values = entry.values;
		change.path = entry.path;
		change.entry = entry.entry;
		change.variant = entry.variant;
		change.message = entry.messages;
		if (entry.result == BuildResult::Failed)
		{
			change.kind = ShaderChange::Kind::Error;
			continue;
		}
		if (entry.description)
		{
			change.kind = ShaderChange::Kind::Description;
			continue;
		}
		BuildEntry again;
		if (!m_cache->getShader(entry.effect.c_str(), entry.technique.c_str(), entry.pass.c_str(), entry.stage,
								entry.values, change.bytecode, again, entry.path))
		{
			change.kind = ShaderChange::Kind::Error;
			change.message = again.messages;
		}
	}
	// Re-read the remap table if it changed; on a Remap change the renderer looks its materials up again.
	if (!table.empty())
	{
		// Same error protocol as above, keyed by the table's file name.
		eastl::string remapError;
		const bool	  changed = m_cache->remapChanged(remapError);
		auto		  known = m_errors.begin();
		while (known != m_errors.end() && known->first != remapKey)
			++known;
		if (!remapError.empty() && (known == m_errors.end() || known->second != remapError))
		{
			if (known != m_errors.end())
				known->second = remapError;
			else
				m_errors.push_back(eastl::make_pair(remapKey, remapError));
			ShaderChange &change = changes.push_back();
			change.kind = ShaderChange::Kind::Error;
			change.effect = remapName;
			change.variant = "remap";
			change.message = remapError;
		}
		else if (remapError.empty() && known != m_errors.end())
		{
			m_errors.erase(known);
			ShaderChange &cleared = changes.push_back();
			cleared.kind = ShaderChange::Kind::Cleared;
			cleared.effect = remapName;
			cleared.variant = "remap";
		}
		if (changed && remapError.empty())
		{
			ShaderChange &change = changes.push_back();
			change.kind = ShaderChange::Kind::Remap;
			change.effect = remapName;
			change.variant = "remap";
		}
	}
	AcquireSRWLockExclusive(srwOf(m_lock));
	for (ShaderChange &change : changes)
		m_pending.push_back(eastl::move(change));
	InterlockedIncrement(&m_rounds);
	ReleaseSRWLockExclusive(srwOf(m_lock));
}

} // namespace render_backend::shader
