#pragma once
#include <atomic>

namespace cpp_bridge {
enum class QueuePoll { item, wait, finished };

// A failed pop can race the producer's final push + release-store(done).
// After observing done, pop again before declaring the queue drained.
template<class Queue, class Value>
QueuePoll pollUntilDrained(Queue& queue, const std::atomic<bool>& done, Value& value) {
    if (queue.pop(value)) return QueuePoll::item;
    if (!done.load(std::memory_order_acquire)) return QueuePoll::wait;
    return queue.pop(value) ? QueuePoll::item : QueuePoll::finished;
}
}
