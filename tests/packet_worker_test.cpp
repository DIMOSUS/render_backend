#include <render_backend/gpu/render_packets.hpp>
#include <cstdio>
#include <cstring>

using render_backend::gpu::packets::Worker;

namespace
{
struct Record
{
	Worker &worker;
	Worker &other;
	uint32_t count = 0;
	bool correct = true;
};

void execute(void *owner, uint32_t kind, const uint8_t *bytes, uint32_t size)
{
	auto &record = *static_cast<Record *>(owner);
	uint32_t value = 0;
	if (size == sizeof value)
		std::memcpy(&value, bytes, sizeof value);
	record.correct &= record.worker.onWorker() && !record.other.onWorker() && kind == 1 &&
					  size == sizeof value && value == record.count + 1;
	++record.count;
}

void enqueue(Worker &worker, uint32_t value)
{
	std::memcpy(worker.reserve(1, sizeof value), &value, sizeof value);
	worker.commit();
	worker.publish();
}
} // namespace

int main()
{
	Worker first, second;
	Record a{first, second}, b{second, first};
	first.start(&execute, nullptr, &a);
	second.start(&execute, nullptr, &b);
	enqueue(first, 1);
	enqueue(second, 1);
	first.sync();
	second.sync();
	first.stop();

	enqueue(second, 2);
	second.sync();
	const bool ok = a.correct && b.correct && a.count == 1 && b.count == 2 &&
					!first.running() && second.running() && second.stats().packets == 2;
	second.stop();
	std::puts(ok ? "Independent packet workers: PASS" : "Independent packet workers: FAIL");
	return ok ? 0 : 1;
}
