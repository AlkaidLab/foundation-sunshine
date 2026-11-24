#pragma once
#include "protocol.h"
namespace q4 {
  constexpr uint32_t version = 5, backend = 5, magic = 0x3451424f, domainCapacity = 8, objectCapacity = 64;
  enum Kind : uint32_t { DomainBegin = 1,
    DomainEnd,
    PassBegin,
    ObjectBefore,
    SubmitResult,
    ProcessResult,
    PassEnd };
  enum Flags : uint32_t { PreCall = 1,
    PostCall = 2,
    ObservedNotCreated = 4,
    NullPcm = 8 };
  enum Reason : uint32_t { Discovery = 1,
    Reset,
    Destroy,
    BindingChanged,
    CaptureStop };
#pragma pack(push, 1)
  struct Header {
    Record audio;
    uint32_t version, kind, properties, flags;
    uint64_t lossEpoch, renderer, encoder, implementation, domain, pass, call;
    uint32_t inputFrames, producedBytes, capacity;
    int32_t result;
    uint32_t reason, reserved;
  };
#pragma pack(pop)
  static_assert(sizeof(Header) == 160);
  // write reserves a ticket; ready publishes the completed header/PCM. A later
  // producer may finish first, but the consumer must never skip an earlier ticket.
  struct Slot {
    Header header;
    float pcm[kSamples];
    alignas(8) volatile LONG64 ready;
  };
#if defined(_MSC_VER)
  // Both IPC ends use the same compiler and this header.
  struct alignas(64) Ring: RingT<Slot> {
    alignas(64) volatile LONG64 ignoredBeforeFirstRender, unboundAfterArming;
    volatile LONG64 domainsOpened, domainsClosed, submitResults, processResults;
  };
  static_assert(offsetof(Ring, write) == 64);
  inline bool
  recordReady(Ring &ring, LONG64 ticket) {
    return InterlockedCompareExchange64(&ring.slots[ticket % kSlots].ready, 0, 0) == ticket + 1;
  }
#endif
}  // namespace q4
