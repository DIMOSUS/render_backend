// Frame graph compilation. The ordering rules are documented in frame_graph.hpp.
#include <render_backend/gpu/frame_graph.hpp>

using namespace render_backend::gpu::graph;

uint32_t Frame::pass(uint32_t name, Mask reads, Mask writes)
{
	if (declarationCount >= C_PASSES)
	{
		++overflow;
		return C_NONE;
	}
	// Extend the latest pass with this name only if moving this declaration back to it keeps every read seeing the
	// same resource versions. A writer declared in between precedes the new read; after merging, that read could end
	// up waiting for later writers instead.
	uint32_t at = passCount;
	for (uint32_t e = passCount; e-- > 0 && at == passCount;)
		if (passes[e].name == name)
		{
			Mask before = 0;
			for (uint32_t i = 0; i < e; ++i)
				before |= passes[i].writes;
			const Mask orderedReads = reads | (passes[e].reads & before);
			const Mask joinedWrites = passes[e].writes | writes;
			bool	   independent = true;
			for (uint32_t b = e + 1; b < passCount && independent; ++b)
				independent = ((passes[b].reads | passes[b].writes) & joinedWrites) == 0 &&
							  (passes[b].writes & orderedReads) == 0;
			if (!independent)
				break;
			at = e;
		}
	if (at == passCount)
		passes[passCount++] = Pass{name, 0, 0};
	passes[at].reads |= reads;
	passes[at].writes |= writes;
	declared[declarationCount++] = uint8_t(at);
	return at;
}

uint32_t Frame::find(uint32_t name, uint32_t &cursor) const
{
	for (uint32_t d = cursor; d < declarationCount; ++d)
		if (passes[declared[d]].name == name)
		{
			cursor = d;
			return declared[d];
		}
	return C_NONE;
}

void render_backend::gpu::graph::compile(const Frame &frame, Order &order)
{
	order = Order{};
	const uint32_t n = frame.passCount < C_PASSES ? frame.passCount : C_PASSES;
	const Pass	  *p = frame.passes;
	// Bit set over pass indices. after[i] holds the passes that must execute before pass i.
	struct Set
	{
		uint64_t bits[(C_PASSES + 63) / 64] = {};
		void	 add(uint32_t i)
		{
			bits[i >> 6] |= uint64_t(1) << (i & 63);
		}
		bool has(uint32_t i) const
		{
			return (bits[i >> 6] >> (i & 63) & 1) != 0;
		}
	};
	Set	 after[C_PASSES], done;
	Mask before = 0; // Resources written by passes declared before i.
	for (uint32_t i = 0; i < n; ++i)
	{
		for (uint32_t j = i + 1; j < n; ++j)
		{
			// j after i: j uses something i wrote, or overwrites something i read from an earlier writer.
			if ((p[i].writes & (p[j].reads | p[j].writes)) != 0 || (p[i].reads & p[j].writes & before) != 0)
				after[j].add(i);
			// i after j: i reads a resource with no earlier writer, and j writes it.
			if ((p[i].reads & ~p[i].writes & p[j].writes & ~before) != 0)
				after[i].add(j);
		}
		before |= p[i].writes;
	}
	// Repeatedly pick the first declared pass whose dependencies are done. If a cycle leaves no such pass, pick the
	// first remaining pass in declaration order and count the cycle.
	for (uint32_t k = 0; k < n; ++k)
	{
		uint32_t pick = C_NONE, first = C_NONE;
		for (uint32_t i = 0; i < n && pick == C_NONE; ++i)
		{
			if (done.has(i))
				continue;
			first = first == C_NONE ? i : first;
			bool ready = true;
			for (uint32_t w = 0; w < sizeof done.bits / sizeof done.bits[0] && ready; ++w)
				ready = (after[i].bits[w] & ~done.bits[w]) == 0;
			if (ready)
				pick = i;
		}
		if (pick == C_NONE)
		{
			pick = first;
			++order.cycles;
		}
		done.add(pick);
		order.sequence[k] = uint8_t(pick);
	}
	// Cull passes, walking backwards from the end of the frame. A pass is live if it writes a persistent resource or a
	// resource used by a later live pass.
	Mask needed = frame.persistent;
	bool live[C_PASSES] = {};
	for (uint32_t k = n; k-- > 0;)
	{
		const Pass &q = p[order.sequence[k]];
		live[k] = (q.writes & needed) != 0;
		if (live[k])
			needed |= q.reads | q.writes;
	}
	for (uint32_t k = 0; k < n; ++k)
	{
		const uint32_t i = order.sequence[k];
		if (!live[k])
		{
			order.rank[i] = C_DEAD;
			++order.dead;
			continue;
		}
		order.sequence[order.count++] = uint8_t(i);
		order.rank[i] = uint16_t(order.count);
		for (uint32_t r = 0; r < C_RESOURCES; ++r)
			if (((p[i].reads | p[i].writes) >> r & 1) != 0)
			{
				if (order.life[r].first == 0)
					order.life[r].first = order.rank[i];
				order.life[r].last = order.rank[i];
			}
	}
}
