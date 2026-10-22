#pragma once
#include <atomic>
#include <cstdint>
namespace spatial_transport {
// Isolated replay only: at most one audio_packets item outstanding. The normal
// queue clears itself at 32 entries; a 60-fragment MAT burst must not use that
// overflow policy. Acknowledge completion of the real broadcaster iteration.
inline std::atomic<uint64_t> replay_ack{0};
}
