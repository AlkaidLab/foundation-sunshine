/**
 * @file src/system_load_stub.cpp
 * @brief System load sampling for platforms without a backend: reports nothing.
 */
#ifndef _WIN32

  #include "system_load.h"

namespace system_load {

  snapshot_t
  sample() {
    return {};
  }

}  // namespace system_load

#endif
