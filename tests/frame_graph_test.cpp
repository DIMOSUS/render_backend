#include <render_backend/gpu/frame_graph.hpp>
#include <cstdio>
#include <initializer_list>

using namespace render_backend::gpu::graph;

namespace
{
bool check(bool ok, const char *what)
{
	if (!ok)
		std::fprintf(stderr, "frame graph: %s\n", what);
	return ok;
}

bool sequence(const Order &order, std::initializer_list<uint32_t> expected)
{
	if (order.count != expected.size())
		return false;
	uint32_t index = 0;
	for (const uint32_t pass : expected)
		if (order.sequence[index++] != pass || order.rank[pass] != index)
			return false;
	return true;
}

bool ordering()
{
	constexpr Mask A = bit(0), B = bit(1), output = bit(2);
	Frame		   laterWriters{};
	laterWriters.persistent = output;
	const auto readFirst = laterWriters.pass(1, A, output);
	const auto writeFirst = laterWriters.pass(2, 0, A);
	const auto writeLast = laterWriters.pass(3, 0, A);
	Order	   order;
	compile(laterWriters, order);
	bool ok = check(sequence(order, {writeFirst, writeLast, readFirst}) && order.cycles == 0,
					"a reader without an earlier writer did not wait for both ordered writers");
	ok &= check(order.life[0].first == 1 && order.life[0].last == 3 && order.life[2].first == 3 &&
					order.life[2].last == 3,
				"read/write lifetimes do not follow the compiled order");

	Frame betweenWriters{};
	betweenWriters.persistent = A | output;
	const auto first = betweenWriters.pass(4, B, A);
	const auto reader = betweenWriters.pass(5, A, output);
	const auto overwrite = betweenWriters.pass(6, 0, A);
	const auto prerequisite = betweenWriters.pass(7, 0, B);
	compile(betweenWriters, order);
	ok &= check(sequence(order, {prerequisite, first, reader, overwrite}) && order.cycles == 0,
				"a reader of an earlier write was not kept ahead of the overwrite");
	return ok;
}

bool pruningAndCycles()
{
	constexpr Mask A = bit(3), B = bit(5), output = bit(7), branch = bit(9), unused = bit(11);
	Frame		   frame{};
	frame.persistent = output;
	const auto producer = frame.pass(8, 0, A);
	const auto deadBranch = frame.pass(9, A, branch);
	const auto orphan = frame.pass(10, 0, unused);
	const auto middle = frame.pass(11, A, B);
	const auto sink = frame.pass(12, B, output);
	Order	   order;
	compile(frame, order);
	bool ok = check(sequence(order, {producer, middle, sink}) && order.dead == 2 && order.rank[deadBranch] == C_DEAD &&
						order.rank[orphan] == C_DEAD,
					"a dead reader or orphan writer survived pruning");
	ok &= check(order.life[9].first == 0 && order.life[9].last == 0 && order.life[11].first == 0 &&
					order.life[11].last == 0 && order.life[3].first == 1 && order.life[3].last == 2,
				"a pruned pass contributed to a resource lifetime");

	Frame cycle{};
	cycle.persistent = A | B;
	const auto left = cycle.pass(13, B, A);
	const auto right = cycle.pass(14, A, B);
	compile(cycle, order);
	ok &= check(order.cycles == 1 && sequence(order, {left, right}), "a cycle was not detected and broken in order");
	return ok;
}

bool coalescing()
{
	constexpr Mask A = bit(14), B = bit(16), C = bit(18), input = bit(20), other = bit(22);
	Frame		   frame{};
	frame.persistent = A | B | C;
	const auto first = frame.pass(15, input, A);
	const auto separate = frame.pass(16, 0, B);
	const auto repeated = frame.pass(15, other, C);
	bool	   ok = check(first == repeated && frame.passCount == 2 && frame.declarationCount == 3 &&
							  frame.passes[first].reads == (input | other) && frame.passes[first].writes == (A | C),
						  "independent declarations of one pass did not merge their masks");
	uint32_t   cursor = 0;
	ok &= check(frame.find(15, cursor) == first && cursor == 0, "find lost the first merged declaration");
	++cursor;
	ok &= check(frame.find(15, cursor) == first && cursor == 2, "find lost the later merged declaration");
	Order order;
	compile(frame, order);
	ok &= check(sequence(order, {first, separate}), "independent merged passes changed declaration order");

	Frame dependent{};
	dependent.persistent = A | B | C;
	const auto earlierWrite = dependent.pass(17, 0, input);
	const auto firstRead = dependent.pass(18, input, A);
	const auto laterWrite = dependent.pass(19, 0, input);
	const auto secondRead = dependent.pass(18, input, B);
	const auto secondAgain = dependent.pass(18, 0, C);
	compile(dependent, order);
	ok &= check(firstRead != secondRead && secondRead == secondAgain && dependent.passCount == 4 &&
					sequence(order, {earlierWrite, firstRead, laterWrite, secondRead}),
				"coalescing did not respect an intervening overwrite of a preceding input");

	Frame newRead{};
	newRead.persistent = A | B | input;
	const auto beforeInput = newRead.pass(20, 0, A);
	const auto inputWriter = newRead.pass(21, 0, input);
	const auto readBetween = newRead.pass(20, input, B);
	const auto inputOverwrite = newRead.pass(22, 0, input);
	compile(newRead, order);
	ok &= check(beforeInput != readBetween && sequence(order, {beforeInput, inputWriter, readBetween, inputOverwrite}),
				"a merged read moved past the writer of the next input version");

	Frame waiting{};
	waiting.persistent = A | C;
	const auto alreadyWaiting = waiting.pass(23, input, A);
	const auto suppliesInput = waiting.pass(24, 0, input);
	const auto laterWrites = waiting.pass(23, 0, C);
	compile(waiting, order);
	ok &= check(alreadyWaiting == laterWrites && sequence(order, {suppliesInput, alreadyWaiting}),
				"an independent continuation broke an existing dependency on a later writer");
	return ok;
}

bool limits()
{
	constexpr Mask topResource = bit(C_RESOURCES - 1);
	Frame		   frame{};
	frame.persistent = topResource;
	bool ok = true;
	for (uint32_t i = 0; i < C_PASSES; ++i)
		ok &= frame.pass(i + 100, 0, topResource) == i;
	if (!check(ok, "valid declarations did not fit within capacity"))
		return false;
	const auto rejected = frame.pass(200, 0, topResource);
	const auto mergedRejected = frame.pass(100, 0, topResource);
	ok = check(rejected == C_NONE && mergedRejected == C_NONE && frame.overflow == 2 &&
				   frame.declarationCount == C_PASSES && frame.passCount == C_PASSES,
			   "overflow changed graph capacity or accepted a repeated declaration");
	Order order;
	compile(frame, order);
	ok &= check(order.count == C_PASSES && order.dead == 0 && order.cycles == 0 &&
					order.life[C_RESOURCES - 1].first == 1 && order.life[C_RESOURCES - 1].last == C_PASSES,
				"the highest resource or a pass above the first dependency word was lost");
	for (uint32_t i = 0; i < C_PASSES; ++i)
		if (!check(order.sequence[i] == i && order.rank[i] == i + 1, "WAW order changed at the pass limit"))
			return false;
	uint32_t cursor = C_PASSES - 1;
	ok &= check(frame.find(100 + C_PASSES - 1, cursor) == C_PASSES - 1, "find missed the last legal declaration");
	++cursor;
	ok &= check(frame.find(100, cursor) == C_NONE, "find searched beyond declaration bounds");
	Frame merged{};
	for (uint32_t i = 0; i < C_PASSES; ++i)
		ok &= merged.pass(1, 0, topResource) == 0;
	return check(ok && merged.pass(1, 0, topResource) == C_NONE && merged.passCount == 1 && merged.overflow == 1,
				 "a frame with one merged pass escaped the declaration limit");
}
} // namespace

int main()
{
	const bool orderOk = ordering();
	const bool pruningOk = pruningAndCycles();
	const bool coalescingOk = coalescing();
	return limits() && orderOk && pruningOk && coalescingOk ? 0 : 1;
}
