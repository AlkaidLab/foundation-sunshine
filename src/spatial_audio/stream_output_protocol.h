#pragma once
#include "object_input_protocol.h"
#include <cwchar>

namespace spatial_stream {
  using namespace receiver;
  constexpr uint32_t streamMagic = 0x32534f4d, streamVersion = 3, slots = 8, payloadCapacity = 12 + 64 * 1940;
  constexpr uint32_t nativeFormat = 1, fakeFormat = 0x80000001u;
  struct Stats {
    uint32_t magic, version, bytes, kind;
    volatile LONG status, error;
    uint32_t hostGuardPassed, hostPid, workerTid;
    volatile LONG workerFinished;
    uint32_t workerTimedOut;
    uint64_t vendorLoads, activations, initializations, submissions, processes, sourceRounds, warmRounds, drainRounds;
    uint64_t messages, records, outputBytes, localHighWater;
    uint32_t eofSeen, helperPinned;
    int32_t createHr, mapperHr, initializeHr, submitHr, processHr;
    uint64_t sourceSamples, fakeCallsVerified;
    uint32_t endpointCalls, hooksInstalled;
  };
#pragma pack(push, 1)
  struct Block {
    uint32_t magic, bytes, format, phase;
    uint64_t sequence, epoch, domain, sampleStart;
    int64_t completedQpc;
    uint32_t sourceIndex, frames;
    uint64_t reserved;
    uint32_t checksum, reserved2;
  };
#pragma pack(pop)
  static_assert(sizeof(Block) == 80);
  struct alignas(64) OutputSlot {
    Block header;
    BYTE payload[payloadCapacity];
  };
  struct alignas(64) Output {
    uint32_t magic, version, bytes, capacity, payloadBytes, format, ownerPid, reserved;
    uint64_t epoch, domain, nonce[2];
    alignas(64) volatile LONG64 write, read, consumerHeartbeat;
    volatile LONG completed, error, cancel;
    Stats header;
    alignas(64) OutputSlot slots[spatial_stream::slots];
  };
  static_assert(sizeof(Output) < 2 * 1024 * 1024 && offsetof(Output, slots) % 64 == 0);
  struct Args {
    uint32_t bytes, version;
    HANDLE input, data, cancel, owner, output;
    uint64_t domain, epoch, nonce[2], ownerCreationTime;
    // Zero leaves duration/round count unbounded; liveness and native pacing stay on.
    uint32_t ownerPid, maxSourceRounds, maxSeconds, reserved;
    // Native only: the publisher resolves the installed Dolby Access package;
    // the worker re-checks its shape and pinned DLL hashes before loading.
    wchar_t vendorDirectory[260];
  };
  struct FakeConfig {
    uint32_t bytes, version;
    int32_t failInitialize, failSubmitAt, failProcessAt;
    uint32_t oversizedProcess, paceMs;
  };
  using Start = DWORD(WINAPI *)(const Args *);
  using FakeStart = DWORD(WINAPI *)(const Args *, const FakeConfig *);
  inline bool
  argsValid(const Args &a) { return a.bytes == sizeof(a) && a.version == streamVersion && a.input && a.data && a.cancel && a.owner && a.output && a.domain && a.epoch && a.ownerPid && a.ownerCreationTime && !a.reserved && wmemchr(a.vendorDirectory, L'\0', 260) != nullptr; }
  inline bool
  contract(const Shared &s, const Args &a) {
    const auto &c = s.contract;
    return c.magic == receiver::magic && c.version == receiver::version && c.headerBytes == 320 && c.mapBytes == sizeof(Shared) && c.slotBytes == sizeof(Slot) && c.capacity == receiver::capacity && c.packetBytes == 124968 && c.callBytes == sizeof(Call) && c.rate == 48000 && c.frames == 480 && !c.reserved && c.qpcHz == hz() && c.domain == a.domain && c.epoch == a.epoch && c.nonce[0] == a.nonce[0] && c.nonce[1] == a.nonce[1] && c.producerPid == a.ownerPid;
  }
  inline bool
  outputValid(const Output &o, const Args &a, bool fake) { return o.magic == streamMagic && o.version == streamVersion && o.bytes == sizeof(Output) && o.capacity == slots && o.payloadBytes == payloadCapacity && o.format == (fake ? fakeFormat : nativeFormat) && o.ownerPid == a.ownerPid && !o.reserved && o.epoch == a.epoch && o.domain == a.domain && o.nonce[0] == a.nonce[0] && o.nonce[1] == a.nonce[1]; }
  inline uint32_t
  blockChecksum(Block h, const BYTE *p) {
    h.checksum = 0;
    return fnv(fnv(2166136261u, &h, sizeof(h)), p, h.bytes);
  }
  inline bool
  publish(Output &o, Block h, const BYTE *p) {
    if (get(o.cancel)) {
      InterlockedCompareExchange(&o.error, cancelled, 0);
      return false;
    }
    if (get(o.error)) return false;
    auto w = get(o.write), r = get(o.read);
    if (w < 0 || r < 0 || w < r) {
      InterlockedCompareExchange(&o.error, counters, 0);
      return false;
    }
    if (w - r >= slots || h.bytes > payloadCapacity) {
      InterlockedCompareExchange(&o.error, overflow, 0);
      return false;
    }
    h.magic = streamMagic;
    h.sequence = w;
    h.epoch = o.epoch;
    h.domain = o.domain;
    h.format = o.format;
    h.reserved = h.reserved2 = 0;
    h.checksum = blockChecksum(h, p);
    auto &slot = o.slots[w % slots];
    slot.header = h;
    if (h.bytes) memcpy(slot.payload, p, h.bytes);
    put(o.write, w + 1);
    return true;
  }
  inline uint32_t
  drainCount(uint64_t alreadyProcessed, bool fake) { return 30 + (!fake ? uint32_t((alreadyProcessed + 30) % 2) : 0); }
  // Caller-owned copy, then reclaim. No borrowed pointers escape the ring.
  enum class Poll { item,
    pending,
    ended,
    failed };
  inline Poll
  next(Output &o, OutputSlot &dest, uint64_t expected) {
    put(o.consumerHeartbeat, qpc());
    if (get(o.error) || get(o.cancel)) return Poll::failed;
    auto r = get(o.read), w = get(o.write);
    if (r < 0 || w < r || w - r > slots || uint64_t(r) != expected) {
      InterlockedCompareExchange(&o.error, counters, 0);
      return Poll::failed;
    }
    if (r == w) return get(o.completed) ? Poll::ended : Poll::pending;
    auto &s = o.slots[r % slots];
    auto h = s.header;
    if (h.magic != streamMagic || h.bytes > payloadCapacity || h.format != o.format || h.epoch != o.epoch || h.domain != o.domain || h.sequence != expected || h.frames != 480 || h.phase < 1 || h.phase > 3 || h.reserved || h.reserved2 || blockChecksum(h, s.payload) != h.checksum) {
      InterlockedCompareExchange(&o.error, badFrame, 0);
      return Poll::failed;
    }
    dest.header = h;
    if (h.bytes) memcpy(dest.payload, s.payload, h.bytes);
    put(o.read, r + 1);
    return Poll::item;
  }
}  // namespace spatial_stream
