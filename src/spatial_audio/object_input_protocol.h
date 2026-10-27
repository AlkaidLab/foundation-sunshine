#pragma once
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace receiver {
  constexpr uint32_t magic = 0x3149504d, version = 1, capacity = 32, frameMagic = 0x3146504d;
  constexpr uint32_t callCapacity = 64, frames = 480, request = 30720;
  // Windows AudioObjectType bits: FL/FR/FC/LFE/SL/SR/BL/BR/TFL/TFR/TBL/TBR.
  // This is an input bed, not the eight-channel IEC carrier layout.
  constexpr uint32_t bed714Mask = 0x1ffe;
  inline bool
  objectType(uint32_t type) { return type == 1 || (type && (type & bed714Mask) == type && !(type & (type - 1))); }
  inline uint32_t
  createFlags(uint32_t type) { return type == 1 ? 7u : 3u; }
  inline uint32_t
  updateFlags(uint32_t type) { return type == 1 ? 6u : 2u; }
  enum Error : LONG { none,
    badContract,
    identity,
    badFrame,
    checksumError,
    sequence,
    overflow,
    cancelled,
    producerExited,
    consumerExited,
    producerStale,
    consumerStale,
    missingEOF,
    codec,
    counters,
    timeout };
  struct alignas(64) Contract {
    uint32_t magic, version, headerBytes, mapBytes, slotBytes, capacity, packetBytes, callBytes;
    uint64_t domain, epoch, nonce[2];
    uint32_t producerPid, rate, frames, reserved;
    int64_t qpcHz;
  };
  struct alignas(64) Common {
    volatile LONG fault, cancel, producerStatus, consumerStatus, consumerPid, producerDone;
  };
  struct alignas(64) Writer {
    volatile LONG64 write, heartbeat;
  };
  struct alignas(64) Reader {
    volatile LONG64 read, heartbeat;
  };
#pragma pack(push, 1)
  struct Message {
    uint32_t magic, bytes;
    uint64_t sequence, domain, epoch;
    uint32_t index, frames, calls, kind;
    int64_t firstQpc, lastQpc, enqueuedQpc;
    uint32_t checksum, reserved;
  };
#pragma pack(pop)
  struct Call {
    uint32_t id, slot, type, flags;
    float xyz[3];
    uint32_t frames;
    float pcm[480];
  };
  struct alignas(64) Slot {
    Message header;
    Call calls[callCapacity];
  };
  struct alignas(64) Shared {
    Contract contract;
    Common common;
    Writer writer;
    Reader reader;
    Slot slots[capacity];
  };
  static_assert(sizeof(Contract) == 128 && offsetof(Shared, slots) == 320 && sizeof(Message) == 80 && sizeof(Call) == 1952 && sizeof(Slot) == 125056 && sizeof(Shared) == 4002112);
  static_assert(std::is_trivial_v<Shared>);
  inline LONG
  get(volatile LONG &v) { return InterlockedCompareExchange(&v, 0, 0); }
  inline LONG64
  get(volatile LONG64 &v) { return InterlockedCompareExchange64(&v, 0, 0); }
  inline void
  put(volatile LONG &v, LONG n) { InterlockedExchange(&v, n); }
  inline void
  put(volatile LONG64 &v, LONG64 n) { InterlockedExchange64(&v, n); }
  inline int64_t
  qpc() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
  }
  inline int64_t
  hz() {
    LARGE_INTEGER n;
    QueryPerformanceFrequency(&n);
    return n.QuadPart;
  }
  inline void
  fail(Shared &s, LONG why) {
    InterlockedCompareExchange(&s.common.fault, why, 0);
    put(s.common.cancel, 1);
  }
  inline uint32_t
  fnv(uint32_t n, const void *data, size_t size) {
    auto p = static_cast<const BYTE *>(data);
    for (size_t i = 0; i < size; ++i) {
      n ^= p[i];
      n *= 16777619u;
    }
    return n;
  }
  inline uint32_t
  checksum(Message m, const Call *calls) {
    m.checksum = 0;
    return fnv(fnv(2166136261u, &m, sizeof(m)), calls, m.calls * sizeof(Call));
  }
}  // namespace receiver
