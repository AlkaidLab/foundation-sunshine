#pragma once
#include <windows.h>
#include <cstdint>
constexpr uint32_t kSlots = 4096, kSamples = 2048, kMagic = 0x52534153;
constexpr DWORD kImageSize = 0x1ef000, kTimestamp = 0x0627570d;
#pragma pack(push, 1)
struct Record {
  uint32_t magic, frames;
  uint64_t host;
  int64_t qpc;
  uint32_t objectId, type;
  float x, y, z, volume, callGain;
  uint32_t tid;
  uint64_t sequence;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 64);
template <class S>
struct alignas(64) RingT {
  uint32_t magic, version, collectorPid, enginePid;
  volatile LONG status, stop, error, writer;
  volatile LONG64 renderCalls;
  uint64_t encoderRva;
  volatile LONG encoderReady;
  uint32_t backend;
  volatile LONG64 extendedCalls;
  alignas(64) volatile LONG64 write;
  alignas(64) volatile LONG64 read;
  alignas(64) volatile LONG64 heartbeat;
  alignas(64) volatile LONG64 calls, droppedFull, droppedBusy, invalid, silent;
  volatile LONG64 frames, copyTicks, maxCopyTicks, highWater;
  int64_t qpcFrequency;
  S slots[kSlots];
};
struct StartArgs {
  HANDLE mapping, collector;
};
