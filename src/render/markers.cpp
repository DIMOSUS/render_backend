// GPU marker stack with optional timers and vendor backend.
#include <render_backend/gpu/markers.hpp>
#include <render_backend/gpu/diagnostics.hpp>
#include <nvrhi/d3d12.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void render_backend::gpu::markers::Stack::setup(nvrhi::IDevice *device, diag::Timers *timers, Backend *backend)
{
	m_on = true;
	m_device = device;
	m_timers = timers;
	m_backend = backend;
}

void render_backend::gpu::markers::Stack::release()
{
	delete m_backend;
	m_backend = nullptr;
	m_on = false;
	m_list = nullptr;
	m_device = nullptr;
}

void render_backend::gpu::markers::Stack::begin(Entry &e, uint32_t index)
{
	m_list->beginMarker(e.name);
	if (m_backend != nullptr && m_native != nullptr)
		m_backend->begin(m_native, e.name);
	if (e.kind != Kind::Item && m_timers != nullptr && m_timers->enabled())
	{
		// Extend the path of the nearest enclosing timed range, or of the frame range.
		uint32_t parent = 5; // strlen("frame")
		for (uint32_t i = index; i-- != 0;)
			if (m_entries[i].kind != Kind::Item)
			{
				parent = m_entries[i].pathLength;
				break;
			}
		const size_t length = strlen(e.name);
		if (parent + 1 + length < C_PATH)
		{
			m_path[parent] = '/';
			for (size_t i = 0; i <= length; ++i) // '/' separates path elements, so replace it in names.
				m_path[parent + 1 + i] = e.name[i] == '/' ? '|' : e.name[i];
			e.pathLength = uint32_t(parent + 1 + length);
		}
		else
			e.pathLength = parent; // Path too long: time it under its parent's row.
		m_path[e.pathLength] = 0;
		m_timers->begin(m_device, m_list, m_path, !e.begun);
	}
	e.running = true;
	e.begun = true;
}

void render_backend::gpu::markers::Stack::end(Entry &e)
{
	if (!e.running)
		return;
	if (e.kind != Kind::Item && m_timers != nullptr && m_timers->enabled())
		m_timers->end(m_list);
	if (m_backend != nullptr && m_native != nullptr)
		m_backend->end(m_native);
	m_list->endMarker();
	e.running = false;
}

void render_backend::gpu::markers::Stack::opened(nvrhi::ICommandList *list)
{
	if (!m_on)
		return;
	m_list = list;
	m_native = list->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
	if (!m_frameBegun)
		_snprintf_s(m_frameName, sizeof m_frameName, _TRUNCATE, "frame %u", m_frame);
	m_list->beginMarker(m_frameName);
	if (m_backend != nullptr && m_native != nullptr)
		m_backend->begin(m_native, m_frameName);
	if (m_timers != nullptr && m_timers->enabled())
	{
		memcpy(m_path, "frame", 6);
		m_timers->begin(m_device, m_list, m_path, !m_frameBegun);
	}
	m_frameBegun = true;
	const uint32_t recorded = m_depth < C_DEPTH ? m_depth : C_DEPTH;
	for (uint32_t i = 0; i < recorded; ++i)
		begin(m_entries[i], i);
}

void render_backend::gpu::markers::Stack::closing()
{
	if (!m_on || m_list == nullptr)
		return;
	const uint32_t recorded = m_depth < C_DEPTH ? m_depth : C_DEPTH;
	for (uint32_t i = recorded; i-- != 0;)
		end(m_entries[i]);
	if (m_timers != nullptr && m_timers->enabled())
		m_timers->end(m_list);
	if (m_backend != nullptr && m_native != nullptr)
		m_backend->end(m_native);
	m_list->endMarker();
	m_list = nullptr;
	m_native = nullptr;
}

void render_backend::gpu::markers::Stack::frameDone()
{
	++m_frame;
	m_frameBegun = false;
	for (Entry &e : m_entries)
		e.begun = false;
}

uint32_t render_backend::gpu::markers::Stack::push(Kind kind, const char *name)
{
	const uint32_t index = m_depth++;
	if (index >= C_DEPTH)
	{
		++m_overflow;
		return index;
	}
	Entry &e = m_entries[index];
	strncpy_s(e.name, sizeof e.name, name != nullptr ? name : "", _TRUNCATE);
	e.kind = kind;
	e.running = false;
	e.begun = false;
	e.pathLength = 0;
	if (m_list != nullptr)
		begin(e, index);
	return index;
}

uint32_t render_backend::gpu::markers::Stack::pushf(Kind kind, const char *format, ...)
{
	char	text[C_NAME];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(text, sizeof text, _TRUNCATE, format, args);
	va_end(args);
	return push(kind, text);
}

void render_backend::gpu::markers::Stack::popTo(uint32_t level)
{
	while (m_depth > level)
	{
		const uint32_t index = --m_depth;
		if (index < C_DEPTH && m_list != nullptr)
			end(m_entries[index]);
	}
}
