// Upload queue: staging chunks, requests, the per-frame budget and copy recording.
#include <render_backend/gpu/upload_queue.hpp>
#include <EASTL/algorithm.h>
#include <d3d12.h>
#include <string.h>

using namespace render_backend::gpu::upload;

namespace
{
constexpr uint32_t C_TEXTURE_ALIGNMENT = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; // 512
constexpr uint32_t C_BUFFER_ALIGNMENT = 16;

ID3D12Resource *nativeOf(nvrhi::IResource *resource)
{
	if (resource == nullptr)
		return nullptr;
	return resource->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource);
}
} // namespace

bool Queue::create(nvrhi::IDevice *device, const Config &config)
{
	if (device == nullptr || m_device)
		return false;
	m_d3d = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
	if (m_d3d == nullptr)
		return false;
	m_device = device;
	m_config = config;
	if (m_config.chunkBytes < (1u << 20))
		m_config.chunkBytes = 1u << 20;
	return true;
}

void Queue::destroy()
{
	std::lock_guard<std::mutex> lock(m_lock);
	for (const CopyBatch &batch : m_copyBatches)
	{
		while (batch.completion && !m_device->pollEventQuery(batch.completion) &&
			   SUCCEEDED(m_d3d->GetDeviceRemovedReason()))
		{
			m_device->waitEventQuery(batch.completion);
			// If waiting returned early, keep ownership until completion or device removal.
			if (!m_device->pollEventQuery(batch.completion))
				Sleep(1);
		}
	}
	m_copyBatches.clear();
	m_queued.clear();
	m_requests.clear();
	m_freeRequests.clear();
	for (Chunk &c : m_chunks)
		if (c.resource != nullptr)
			c.resource->Release(); // Releasing the last reference also unmaps it.
	m_chunks.clear();
	m_current = ~0u;
	m_d3d = nullptr;
	m_device.Reset();
}

//----------------------------------------------------------------------------------------------------------------------
// Staging
bool Queue::allocate(uint64_t size, uint32_t alignment, uint32_t &chunk, uint64_t &offset)
{
	const auto fits = [&](uint32_t i)
	{
		const Chunk	  &c = m_chunks[i];
		const uint64_t at = (c.used + alignment - 1) & ~uint64_t(alignment - 1);
		return c.resource != nullptr && size <= c.size && at <= c.size - size;
	};
	uint32_t found = ~0u;
	if (m_current < m_chunks.size() && fits(m_current))
		found = m_current;
	for (uint32_t i = 0; found == ~0u && i < m_chunks.size(); ++i)
		if (fits(i))
			found = i;
	if (found == ~0u)
	{
		const uint64_t		  bytes = size > m_config.chunkBytes ? size : m_config.chunkBytes;
		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *resource = nullptr;
		// Every byte a copy reads is written first, so the memory does not need to be zeroed.
		if (FAILED(m_d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc,
												  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
												  IID_PPV_ARGS(&resource))) &&
			FAILED(m_d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
												  nullptr, IID_PPV_ARGS(&resource))))
			return false;
		resource->SetName(L"upload staging");
		void			 *mapped = nullptr;
		const D3D12_RANGE none = {0, 0}; // The CPU never reads staging memory.
		if (FAILED(resource->Map(0, &none, &mapped)) || mapped == nullptr)
		{
			resource->Release();
			return false;
		}
		for (uint32_t i = 0; found == ~0u && i < m_chunks.size(); ++i)
			if (m_chunks[i].resource == nullptr)
				found = i;
		if (found == ~0u)
		{
			found = uint32_t(m_chunks.size());
			m_chunks.push_back(Chunk{});
		}
		Chunk &c = m_chunks[found];
		c = Chunk{};
		c.resource = resource;
		c.mapped = static_cast<uint8_t *>(mapped);
		c.size = bytes;
		c.idleFrom = m_frame;
		++m_stats.chunks;
		++m_stats.chunksMade;
		m_stats.capacity += bytes;
		m_stats.capacityPeak = m_stats.capacity > m_stats.capacityPeak ? m_stats.capacity : m_stats.capacityPeak;
		m_stats.overMark += m_stats.capacity > m_config.markBytes ? 1u : 0u;
	}
	Chunk &c = m_chunks[found];
	offset = (c.used + alignment - 1) & ~uint64_t(alignment - 1);
	m_stats.used += offset + size - c.used;
	m_stats.usedPeak = m_stats.used > m_stats.usedPeak ? m_stats.used : m_stats.usedPeak;
	c.used = offset + size;
	++c.unissued;
	chunk = found;
	m_current = found;
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
// Budget
bool Queue::spent()
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_admitted >= m_config.budgetBytes;
}

bool Queue::admit(uint64_t bytes)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (m_admitted >= m_config.budgetBytes)
	{
		++m_stats.refused;
		return false;
	}
	m_admitted += bytes != 0 ? bytes : 1;
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
// Requests. A ticket is the request index plus one, so 0 is never a valid ticket.
Queue::Request *Queue::find(uint32_t ticket)
{
	if (ticket == 0 || ticket > m_requests.size() || !m_requests[ticket - 1].used)
		return nullptr;
	return &m_requests[ticket - 1];
}

void Queue::release(Request &r)
{
	r.parts.clear();
	r.used = false;
	m_freeRequests.push_back(uint32_t(&r - m_requests.data()));
}

uint32_t Queue::begin(bool placed)
{
	if (m_d3d == nullptr)
		return 0;
	std::lock_guard<std::mutex> lock(m_lock);
	uint32_t					index;
	if (!m_freeRequests.empty())
	{
		index = m_freeRequests.back();
		m_freeRequests.pop_back();
	}
	else
	{
		index = uint32_t(m_requests.size());
		m_requests.push_back(Request{});
	}
	Request &r = m_requests[index];
	r.used = true;
	r.placed = placed;
	++m_stats.requests;
	m_stats.placed += placed ? 1u : 0u;
	return index + 1;
}

bool Queue::texture(uint32_t ticket, nvrhi::ITexture *texture, uint32_t arraySlice, uint32_t mipLevel, Room &room,
					nvrhi::ResourceStates after)
{
	// Staging layout of the subresource as CopyTextureRegion reads it.
	ID3D12Resource *native = nativeOf(texture);
	if (native == nullptr || m_d3d == nullptr)
		return false;
	const D3D12_RESOURCE_DESC desc = native->GetDesc();
	const uint32_t arraySize = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
	if (mipLevel >= desc.MipLevels || arraySlice >= arraySize)
		return false;
	const uint32_t					   subresource = mipLevel + arraySlice * desc.MipLevels;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;
	UINT							   rows = 0;
	UINT64							   rowBytes = 0, total = 0;
	m_d3d->GetCopyableFootprints(&desc, subresource, 1, 0, &foot, &rows, &rowBytes, &total);
	if (total == UINT64_MAX)
		return false;
	std::lock_guard<std::mutex> lock(m_lock);
	Request					   *r = find(ticket);
	Part						part;
	if (r == nullptr || !allocate(total, C_TEXTURE_ALIGNMENT, part.chunk, part.offset))
		return false;
	part.texture = texture;
	part.after = after;
	part.bytes = total;
	part.subresource = subresource;
	part.format = uint32_t(foot.Footprint.Format);
	part.footWidth = foot.Footprint.Width;
	part.footHeight = foot.Footprint.Height;
	part.rowPitch = foot.Footprint.RowPitch;
	r->parts.push_back(part);
	room.bytes = m_chunks[part.chunk].mapped + part.offset;
	room.rowPitch = part.rowPitch;
	room.rows = rows;
	room.rowBytes = uint32_t(rowBytes);
	return true;
}

bool Queue::writeTexture(uint32_t ticket, nvrhi::ITexture *texture, uint32_t arraySlice, uint32_t mipLevel,
						 const void *data, size_t rowPitch, nvrhi::ResourceStates after)
{
	Room room;
	if (!this->texture(ticket, texture, arraySlice, mipLevel, room, after))
		return false;
	const size_t bytes = rowPitch < room.rowBytes ? rowPitch : room.rowBytes;
	if (rowPitch == room.rowPitch)
		memcpy(room.bytes, data, size_t(room.rows - 1) * rowPitch + bytes);
	else
		for (uint32_t row = 0; row < room.rows; ++row)
			memcpy(room.bytes + size_t(row) * room.rowPitch, static_cast<const uint8_t *>(data) + row * rowPitch,
				   bytes);
	return true;
}

bool Queue::stageTexture(uint32_t ticket, uint32_t dxgiFormat, uint32_t width, uint32_t height, uint32_t arraySlice,
						 uint32_t mipLevel, const void *data, size_t rowPitch, nvrhi::ResourceStates after)
{
	if (m_d3d == nullptr)
		return false;
	// A subresource footprint depends only on its own size and format, so compute it for a single-subresource texture
	// of that size. Block-compressed sizes are rounded up to whole 4x4 blocks, as they are for the mip levels of a full
	// texture.
	const bool			block = (dxgiFormat >= DXGI_FORMAT_BC1_TYPELESS && dxgiFormat <= DXGI_FORMAT_BC5_SNORM) ||
								(dxgiFormat >= DXGI_FORMAT_BC6H_TYPELESS && dxgiFormat <= DXGI_FORMAT_BC7_UNORM_SRGB);
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = block ? (width + 3) & ~3u : width;
	desc.Height = block ? (height + 3) & ~3u : height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT(dxgiFormat);
	desc.SampleDesc.Count = 1;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;
	UINT							   rows = 0;
	UINT64							   rowBytes = 0, total = 0;
	m_d3d->GetCopyableFootprints(&desc, 0, 1, 0, &foot, &rows, &rowBytes, &total);
	uint8_t *room = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Request					   *r = find(ticket);
		Part						part;
		if (r == nullptr || !r->placed || total == UINT64_MAX ||
			!allocate(total, C_TEXTURE_ALIGNMENT, part.chunk, part.offset))
			return false;
		part.after = after;
		part.bytes = total;
		part.arraySlice = arraySlice;
		part.mipLevel = mipLevel;
		part.format = uint32_t(foot.Footprint.Format);
		part.footWidth = foot.Footprint.Width;
		part.footHeight = foot.Footprint.Height;
		part.rowPitch = foot.Footprint.RowPitch;
		r->parts.push_back(part);
		room = m_chunks[part.chunk].mapped + part.offset;
	}
	const size_t bytes = rowPitch < rowBytes ? rowPitch : size_t(rowBytes);
	for (uint32_t row = 0; row < rows; ++row)
		memcpy(room + size_t(row) * foot.Footprint.RowPitch, static_cast<const uint8_t *>(data) + row * rowPitch,
			   bytes);
	return true;
}

void Queue::bind(uint32_t ticket, nvrhi::ITexture *texture)
{
	ID3D12Resource *native = texture != nullptr ? nativeOf(texture) : nullptr;
	if (native == nullptr)
		return;
	const uint32_t				levels = native->GetDesc().MipLevels;
	std::lock_guard<std::mutex> lock(m_lock);
	Request					   *r = find(ticket);
	if (r == nullptr)
		return;
	for (Part &p : r->parts)
		if (!p.texture && !p.buffer)
		{
			p.texture = texture;
			p.subresource = p.mipLevel + p.arraySlice * levels;
		}
}

void Queue::cancel(uint32_t ticket)
{
	std::lock_guard<std::mutex> lock(m_lock);
	Request					   *r = find(ticket);
	if (r == nullptr)
		return;
	for (const Part &p : r->parts)
		--m_chunks[p.chunk].unissued;
	release(*r);
}

uint8_t *Queue::buffer(uint32_t ticket, nvrhi::IBuffer *buffer, uint64_t offset, uint64_t size,
					   nvrhi::ResourceStates after, bool permanent)
{
	std::lock_guard<std::mutex> lock(m_lock);
	Request					   *r = find(ticket);
	Part						part;
	if (r == nullptr || buffer == nullptr || size == 0)
		return nullptr;
	const uint64_t capacity = buffer->getDesc().byteSize;
	if (size > capacity || offset > capacity - size)
		return nullptr;
	if (!allocate(size, C_BUFFER_ALIGNMENT, part.chunk, part.offset))
		return nullptr;
	part.buffer = buffer;
	part.after = after;
	part.bytes = size;
	part.destOffset = offset;
	part.permanent = permanent;
	r->parts.push_back(part);
	return m_chunks[part.chunk].mapped + part.offset;
}

bool Queue::writeBuffer(uint32_t ticket, nvrhi::IBuffer *buffer, uint64_t offset, const void *data, uint64_t size,
						nvrhi::ResourceStates after, bool permanent)
{
	uint8_t *room = this->buffer(ticket, buffer, offset, size, after, permanent);
	if (room == nullptr)
		return false;
	memcpy(room, data, size_t(size));
	return true;
}

void Queue::end(uint32_t ticket)
{
	std::lock_guard<std::mutex> lock(m_lock);
	Request					   *r = find(ticket);
	if (r == nullptr || r->placed)
		return;
	for (const Part &p : r->parts)
		m_queued.push_back(p);
	release(*r);
}

//----------------------------------------------------------------------------------------------------------------------
// Frame
void Queue::beginFrame(uint64_t frame)
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_stats.frameBytes = m_frameBytes;
	m_stats.frameCopies = m_frameCopies;
	m_stats.admittedBytes = m_admitted;
	m_stats.frameBytesPeak = m_frameBytes > m_stats.frameBytesPeak ? m_frameBytes : m_stats.frameBytesPeak;
	m_stats.frameCopiesPeak = m_frameCopies > m_stats.frameCopiesPeak ? m_frameCopies : m_stats.frameCopiesPeak;
	m_frameBytes = 0;
	m_frameCopies = 0;
	m_admitted = 0;
	m_frame = frame;
	for (auto i = m_copyBatches.begin(); i != m_copyBatches.end();)
	{
		if (i->completion && m_device->pollEventQuery(i->completion))
		{
			releasePins(*i);
			i = m_copyBatches.erase(i);
		}
		else
			++i;
	}
	// Reuse fully retired chunks. Release oversized chunks immediately and extra standard chunks after they go idle.
	bool kept = false;
	for (uint32_t i = 0; i < m_chunks.size(); ++i)
	{
		Chunk &c = m_chunks[i];
		if (c.resource == nullptr)
			continue;
		if (c.used != 0 && c.unissued == 0 && c.pins == 0)
		{
			m_stats.used -= c.used;
			c.used = 0;
			c.idleFrom = frame;
		}
		const bool standard = c.size <= m_config.chunkBytes;
		if (c.used != 0 || (standard && !kept))
		{
			kept = kept || standard;
			continue;
		}
		if (!standard || frame >= c.idleFrom + m_config.idleFrames)
		{
			c.resource->Release();
			m_stats.capacity -= c.size;
			--m_stats.chunks;
			++m_stats.chunksReleased;
			c = Chunk{};
			if (m_current == i)
				m_current = ~0u;
		}
	}
}

Queue::CopyBatches::iterator Queue::findRecording(nvrhi::ICommandList *list)
{
	return eastl::find_if(m_copyBatches.begin(), m_copyBatches.end(),
						  [list](const CopyBatch &batch) { return batch.list.Get() == list; });
}

void Queue::releasePins(const CopyBatch &batch)
{
	for (uint32_t chunk : batch.chunks)
		--m_chunks[chunk].pins;
}

bool Queue::record(nvrhi::ICommandList *list, const Part *parts, size_t count)
{
	if (list == nullptr || list->getDesc().queueType != nvrhi::CommandQueue::Graphics)
		return false;
	ID3D12GraphicsCommandList *native = list->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
	if (native == nullptr)
		return false;

	// Pin every distinct source chunk before emitting the copies. A command list can record several requests.
	auto recording = findRecording(list);
	if (recording == m_copyBatches.end())
	{
		m_copyBatches.emplace_back();
		recording = m_copyBatches.end() - 1;
		recording->list = list;
	}
	auto &chunks = recording->chunks;
	for (size_t i = 0; i < count; ++i)
	{
		const uint32_t chunk = parts[i].chunk;
		if (eastl::find(chunks.begin(), chunks.end(), chunk) == chunks.end())
		{
			chunks.push_back(chunk);
			++m_chunks[chunk].pins;
		}
	}

	// Transition every destination to CopyDest in one barrier batch. Textures transition as a whole, since nothing
	// reads a texture while it is being uploaded. A permanent buffer is not tracked yet; it starts in COMMON, the state
	// it was created in. Tracking must begin only once per buffer, even when its parts are not adjacent.
	eastl::vector<nvrhi::IBuffer *> begun;
	nvrhi::IResource			   *last = nullptr;
	for (size_t i = 0; i < count; ++i)
	{
		const Part &p = parts[i];
		if (p.texture)
		{
			if (p.texture.Get() != last)
				list->setTextureState(p.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
			last = p.texture.Get();
		}
		else
		{
			if (p.buffer.Get() != last)
			{
				if (p.permanent && eastl::find(begun.begin(), begun.end(), p.buffer.Get()) == begun.end())
				{
					begun.push_back(p.buffer.Get());
					list->beginTrackingBufferState(p.buffer, nvrhi::ResourceStates::Common);
				}
				list->setBufferState(p.buffer, nvrhi::ResourceStates::CopyDest);
			}
			last = p.buffer.Get();
		}
	}
	list->commitBarriers();
	for (size_t i = 0; i < count; ++i)
	{
		const Part &p = parts[i];
		Chunk	   &chunk = m_chunks[p.chunk];
		if (p.texture)
		{
			D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
			to.pResource = nativeOf(p.texture);
			to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			to.SubresourceIndex = p.subresource;
			from.pResource = chunk.resource;
			from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			from.PlacedFootprint.Offset = p.offset;
			from.PlacedFootprint.Footprint.Format = DXGI_FORMAT(p.format);
			from.PlacedFootprint.Footprint.Width = p.footWidth;
			from.PlacedFootprint.Footprint.Height = p.footHeight;
			from.PlacedFootprint.Footprint.Depth = 1;
			from.PlacedFootprint.Footprint.RowPitch = p.rowPitch;
			native->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		}
		else
			native->CopyBufferRegion(nativeOf(p.buffer), p.destOffset, chunk.resource, p.offset, p.bytes);
		--chunk.unissued;
		m_frameBytes += p.bytes;
		m_stats.runBytes += p.bytes;
	}
	m_frameCopies += uint32_t(count);
	m_stats.runCopies += count;
	// Transition the destinations to the states the requests asked for.
	last = nullptr;
	for (size_t i = 0; i < count; ++i)
	{
		const Part &p = parts[i];
		if (p.texture)
		{
			if (p.texture.Get() != last)
				list->setTextureState(p.texture, nvrhi::AllSubresources, p.after);
			last = p.texture.Get();
		}
		else
		{
			if (p.buffer.Get() != last)
			{
				if (p.permanent)
					list->setPermanentBufferState(p.buffer, p.after);
				else
					list->setBufferState(p.buffer, p.after);
			}
			last = p.buffer.Get();
		}
	}
	list->commitBarriers();
	return true;
}

bool Queue::pending()
{
	std::lock_guard<std::mutex> lock(m_lock);
	return !m_queued.empty();
}

bool Queue::issue(nvrhi::ICommandList *list)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (m_queued.empty() || !record(list, m_queued.data(), m_queued.size()))
		return false;
	m_queued.clear();
	return true;
}

void Queue::issueTicket(nvrhi::ICommandList *list, uint32_t ticket)
{
	std::lock_guard<std::mutex> lock(m_lock);
	Request					   *r = find(ticket);
	if (r == nullptr || !r->placed)
		return;
	// Drop parts that were never bound. They won't be referenced by any command list.
	for (size_t i = r->parts.size(); i-- > 0;)
		if (!r->parts[i].texture && !r->parts[i].buffer)
		{
			--m_chunks[r->parts[i].chunk].unissued;
			r->parts.erase(r->parts.begin() + i);
		}
	if (r->parts.empty() || record(list, r->parts.data(), r->parts.size()))
		release(*r);
}

uint64_t Queue::submit(nvrhi::ICommandList *list, nvrhi::ICommandList *prelude)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if (!m_device || !list || list->getDesc().queueType != nvrhi::CommandQueue::Graphics ||
		(prelude && (prelude == list || prelude->getDesc().queueType != nvrhi::CommandQueue::Graphics)))
		return 0;

	const auto recording = findRecording(list);
	const auto prefixRecording = prelude ? findRecording(prelude) : m_copyBatches.end();
	const bool staged = recording != m_copyBatches.end() || prefixRecording != m_copyBatches.end();
	const auto completion = staged ? m_device->createEventQuery() : nvrhi::EventQueryHandle{};
	if (staged && !completion)
		return 0;

	nvrhi::ICommandList *lists[] = {prelude, list};
	const uint64_t submission = m_device->executeCommandLists(prelude ? lists : lists + 1, prelude ? 2 : 1);
	if (submission != 0 && staged)
	{
		// Both recordings are protected by the fence of the ordered batch.
		m_device->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
		for (const auto batch : {prefixRecording, recording})
			if (batch != m_copyBatches.end())
			{
				batch->completion = completion;
				batch->list.Reset();
			}
	}
	return submission;
}

void Queue::discard(nvrhi::ICommandList *list)
{
	if (list == nullptr)
		return;
	std::lock_guard<std::mutex> lock(m_lock);
	const auto					recording = findRecording(list);
	if (recording != m_copyBatches.end())
	{
		releasePins(*recording);
		m_copyBatches.erase(recording);
	}
}

Stats Queue::stats()
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_stats;
}
