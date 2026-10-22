#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

// In-process C ABI only: caller owns all byte storage; MSVC owns context memory.
// q4::Ring never crosses the compiler boundary as a C++ type.
namespace spatial_capture {
constexpr uint32_t version=1,event_capacity=160+2048*4;
enum Result:uint32_t {event=0,pending=1,ended=2,failed=3};
struct Args {uint32_t bytes,version;HANDLE mapping,owner;uint32_t owner_pid,collector_pid;uint64_t owner_created;};
struct Event {uint32_t bytes,version;uint32_t size,reserved;uint8_t data[event_capacity];};
using Create=uint32_t(WINAPI*)(const Args*,void**);
using Next=uint32_t(WINAPI*)(void*,Event*);
using Destroy=void(WINAPI*)(void*);
}
