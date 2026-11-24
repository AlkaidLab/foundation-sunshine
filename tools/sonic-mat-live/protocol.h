#pragma once
#include "../mat-capture/protocol.h"
namespace sonic_live {
  constexpr uint32_t objects = 20, frames = 480;
  // Owned object passes shared by the plugin adapter and native worker bridge.
  struct Object {
    uint64_t host;
    uint32_t id, type, offset, frames;
    float xyz[3], volume, gain;
    int32_t result;
    uint32_t ended, nullPcm;
    float pcm[sonic_live::frames];
  };
  struct Pass {
    uint64_t sequence, renderer;
    int64_t firstQpc, lastQpc;
    uint32_t deviceFrames, requestedFrames, rate, count, endCount;
    struct End {
      uint64_t host;
      uint32_t id, reserved;
    } ends[sonic_live::objects];
    Object objects[sonic_live::objects];
  };
}  // namespace sonic_live
