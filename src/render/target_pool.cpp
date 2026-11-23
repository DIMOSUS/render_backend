// Render target pool.
#include <render_backend/gpu/target_pool.hpp>

using render_backend::gpu::TargetPool;

uint32_t TargetPool::take(nvrhi::IDevice *device, nvrhi::Format format, uint32_t width, uint32_t height, uint8_t owner,
						  uint64_t frame, const char *name)
{
	return takeImpl(device, format, width, height, owner, frame, name, nullptr);
}

bool TargetPool::setExecutionOrder(uint64_t frame, const graph::Frame &declarations, const graph::Order &order)
{
	if (m_orderSet || m_stats.taken != 0)
		return false;

	m_orderSet = true;
	m_orderFrame = frame;
	if (declarations.overflow != 0 || order.cycles != 0 || declarations.passCount > graph::C_PASSES ||
		declarations.declarationCount > graph::C_PASSES || declarations.passCount > declarations.declarationCount ||
		order.count == 0 || order.count > declarations.passCount || order.dead != declarations.passCount - order.count)
		return false;
	// Derive lifetimes from the ordered declarations, even if the caller's Order::life is stale.
	for (auto &life : m_lifetimes)
		life = {};
	for (uint32_t i = 0; i < order.count; ++i)
	{
		const uint16_t rank = uint16_t(i + 1);
		const uint32_t pass = order.sequence[i];
		if (pass >= declarations.passCount || order.rank[pass] != rank)
			return false;
		const graph::Pass &declaration = declarations.passes[pass];
		const graph::Mask  touched = (declaration.reads | declaration.writes) & ~declarations.persistent;
		for (uint32_t resource = 0; resource < graph::C_RESOURCES; ++resource)
			if ((touched & graph::bit(resource)) != 0)
			{
				auto &life = m_lifetimes[resource];
				if (life.first == 0)
					life.first = rank;
				life.last = rank;
			}
	}
	m_orderValid = true;
	return true;
}

uint32_t TargetPool::takeTransient(nvrhi::IDevice *device, nvrhi::Format format, uint32_t width, uint32_t height,
								   uint8_t owner, uint64_t frame, const char *name, uint32_t resource)
{
	const graph::Order::Life *life = nullptr;
	if (m_orderValid && frame == m_orderFrame && resource < graph::C_RESOURCES && m_lifetimes[resource].first != 0)
		life = &m_lifetimes[resource];
	return takeImpl(device, format, width, height, owner, frame, name, life);
}

bool TargetPool::canShare(uint32_t slot, uint64_t frame, const RankMask &ranks) const
{
	const Target	  &target = m_targets[slot];
	const Reservation &reservation = m_reservations[slot];
	if (reservation.exclusive || target.lastFrame != frame || target.framebuffer || target.pairFramebuffer ||
		target.pair != nullptr)
		return false;
	for (size_t word = 0; word < ranks.size(); ++word)
		if ((reservation.occupied[word] & ranks[word]) != 0)
			return false;
	return true;
}

uint32_t TargetPool::takeImpl(nvrhi::IDevice *device, nvrhi::Format format, uint32_t width, uint32_t height,
							  uint8_t owner, uint64_t frame, const char *name, const graph::Order::Life *lifetime)
{
	RankMask ranks{};
	if (lifetime != nullptr)
		for (uint32_t rank = lifetime->first; rank <= lifetime->last; ++rank)
		{
			const uint32_t bitIndex = rank - 1;
			ranks[bitIndex / 64] |= uint64_t(1) << (bitIndex % 64);
		}
	uint32_t found = C_NONE, empty = C_NONE;
	for (uint32_t i = 0; i < C_TARGETS; ++i)
	{
		const Target &t = m_targets[i];
		if (!t.texture)
		{
			if (empty == C_NONE)
				empty = i;
			continue;
		}
		if (t.format != format || t.width != width || t.height != height ||
			(t.taken && (lifetime == nullptr || !canShare(i, frame, ranks))))
			continue;
		// Prefer a compatible reservation already taken this frame, then apply the usual owner preference.
		if (found == C_NONE || (t.taken && !m_targets[found].taken) ||
			(t.taken == m_targets[found].taken && t.owner == owner && m_targets[found].owner != owner))
			found = i;
	}
	// Table full: release the free target unused for longest to make room. Pending commands that name it hold their
	// own references.
	if (found == C_NONE && empty == C_NONE && device != nullptr)
	{
		for (uint32_t i = 0; i < C_TARGETS; ++i)
			if (!m_targets[i].taken && (empty == C_NONE || m_targets[i].lastFrame < m_targets[empty].lastFrame))
				empty = i;
		if (empty != C_NONE)
			letGo(m_targets[empty]);
	}
	if (found == C_NONE && empty != C_NONE && device != nullptr)
	{
		const nvrhi::FormatInfo &info = nvrhi::getFormatInfo(format);
		const bool				 depth = info.hasDepth;
		nvrhi::TextureDesc		 d;
		d.setWidth(width)
			.setHeight(height)
			.setFormat(format)
			.setIsRenderTarget(true)
			.setClearValue(depth ? nvrhi::Color(1.0f) : nvrhi::Color(0.0f))
			.setDebugName(name)
			.setInitialState(depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget)
			.setKeepInitialState(true);
		Target &t = m_targets[empty];
		t.texture = device->createTexture(d);
		if (t.texture)
		{
			t.format = format;
			t.width = width;
			t.height = height;
			t.bytes = uint64_t(width) * height * info.bytesPerBlock;
			++m_stats.made;
			++m_stats.live;
			m_stats.bytes += t.bytes;
			m_stats.peakLive = m_stats.live > m_stats.peakLive ? m_stats.live : m_stats.peakLive;
			m_stats.peakBytes = m_stats.bytes > m_stats.peakBytes ? m_stats.bytes : m_stats.peakBytes;
			found = empty;
		}
	}
	if (found == C_NONE)
	{
		++m_stats.fallbacks;
		return C_NONE;
	}
	Target &t = m_targets[found];
	Reservation &reservation = m_reservations[found];
	if (t.taken)
		++m_stats.sameFrameReuses;
	else
	{
		reservation = {};
		++m_stats.taken;
	}
	if (lifetime == nullptr)
		reservation.exclusive = true;
	else
		for (size_t word = 0; word < ranks.size(); ++word)
			reservation.occupied[word] |= ranks[word];
	t.taken = true;
	t.owner = owner;
	t.lastFrame = frame;
	m_stats.peakTaken = m_stats.taken > m_stats.peakTaken ? m_stats.taken : m_stats.peakTaken;
	return found;
}

void TargetPool::letGo(Target &t)
{
	m_stats.bytes -= t.bytes;
	--m_stats.live;
	++m_stats.letGo;
	t = Target{};
}

void TargetPool::endFrame(uint64_t frame)
{
	m_orderSet = m_orderValid = false;
	for (Target &t : m_targets)
	{
		t.taken = false;
		if (t.texture && t.lastFrame + C_AGE < frame)
			letGo(t);
	}
	m_stats.taken = 0;
}

void TargetPool::release()
{
	m_orderSet = m_orderValid = false;
	for (Target &t : m_targets)
		if (t.texture)
			letGo(t);
	m_stats.taken = 0;
}
