#pragma once
#include "protocol.h"
#include <map>
#include <tuple>
#include <cmath>
#include <limits>
#include <algorithm>
// Disk v2: 112-byte header, followed by frames mono float32 samples.
// audio.xyz/volume are reconstructed; rawXYZ preserves the property payload.
// Unknown coordinates/volume are quiet NaNs, never fabricated zeroes.
#pragma pack(push, 1)
struct StateRecord {
  Record audio;
  uint32_t version, propertyFlags, positionState, volumeKnown;
  uint64_t lossEpoch, generation;
  float rawX, rawY, rawZ;
  uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(StateRecord) == 112);
namespace position_state {
  enum : uint32_t { Unknown = 0,
    Updated = 1,
    Cached = 2,
    DefaultOrigin = 3,
    StaticChannel = 4 };
  class PositionCache {
    using Key = std::tuple<uint64_t, uint32_t, uint32_t>;
    struct Entry {
      float x = 0, y = 0, z = 0, volume = 1;
      bool known = false, volumeKnown = false;
      uint64_t generation = 0, touched = 0;
    };
    std::map<Key, Entry> entries;
    uint64_t epoch = 0, generation = 0;

  public:
    uint64_t updates = 0, cached = 0, unknown = 0, defaults = 0, statics = 0, creates = 0, ends = 0;
    uint64_t gapInvalidations = 0, evictions = 0, peak = 0, recovered = 0;
    StateRecord
    Apply(const Record &raw, uint32_t flags, uint64_t loss) {
      if (loss != epoch) {
        // A lost create/end can also hide ID reuse: split identity, not just XYZ.
        entries.clear();
        epoch = loss;
        ++gapInvalidations;
      }
      Key key { raw.host, raw.objectId, raw.type };
      auto it = entries.find(key);
      if (it == entries.end()) {
        if (entries.size() >= 4096) {
          auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.second.touched < b.second.touched; });
          entries.erase(oldest);
          ++evictions;
        }
        it = entries.emplace(key, Entry {}).first;
        it->second.generation = ++generation;
      }
      else if (flags & 1) {
        it->second = Entry {};
        it->second.generation = ++generation;
      }
      Entry &e = it->second;
      e.touched = raw.sequence;
      peak = std::max<uint64_t>(peak, entries.size());
      StateRecord out {};
      out.audio = raw;
      out.audio.magic = 0x324f424a;
      out.version = 2;
      out.propertyFlags = flags;
      out.lossEpoch = loss;
      out.generation = e.generation;
      out.rawX = raw.x;
      out.rawY = raw.y;
      out.rawZ = raw.z;
      float nan = std::numeric_limits<float>::quiet_NaN();
      if (flags & 1) {
        ++creates;
        e.volume = 1;
        e.volumeKnown = true;
      }
      if ((flags & 2) && std::isfinite(raw.volume)) {
        e.volume = raw.volume;
        e.volumeKnown = true;
      }
      out.volumeKnown = e.volumeKnown;
      out.audio.volume = e.volumeKnown ? e.volume : nan;
      if (raw.type != 1) {
        out.positionState = StaticChannel;
        ++statics;
        out.audio.x = out.audio.y = out.audio.z = nan;
      }
      else {
        if ((flags & 4) && std::isfinite(raw.x) && std::isfinite(raw.y) && std::isfinite(raw.z)) {
          if (!e.known && !(flags & 1)) ++recovered;
          e.x = raw.x;
          e.y = raw.y;
          e.z = raw.z;
          e.known = true;
          out.positionState = Updated;
          ++updates;
        }
        else if (flags & 4) {
          e.known = false;
          out.positionState = Unknown;
          ++unknown;
        }
        else if (flags & 1) {
          e.x = e.y = e.z = 0;
          e.known = true;
          out.positionState = DefaultOrigin;
          ++defaults;
        }
        else if (e.known) {
          out.positionState = Cached;
          ++cached;
        }
        else {
          out.positionState = Unknown;
          ++unknown;
        }
        out.audio.x = e.known ? e.x : nan;
        out.audio.y = e.known ? e.y : nan;
        out.audio.z = e.known ? e.z : nan;
      }
      if (flags & 8) {
        entries.erase(it);
        ++ends;
      }
      return out;
    }
  };
}  // namespace position_state
using PositionCache = position_state::PositionCache;
