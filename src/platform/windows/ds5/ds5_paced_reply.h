/**
 * @file src/platform/windows/ds5/ds5_paced_reply.h
 * @brief The reply pacing of the virtual DualSense: the shared queue.
 *
 * The queue itself is device independent and lives in
 * src/platform/windows/paced_reply_queue.h so that other virtual devices share
 * one implementation.
 */
#pragma once

#include "src/platform/windows/paced_reply_queue.h"

namespace platf::ds5 {
  using paced_reply_queue = ::platf::paced_reply_queue;
}  // namespace platf::ds5
