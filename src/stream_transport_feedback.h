#pragma once

#include "stream_quality_controller.h"

#include <alkaidlab/transport/transport_module.h>

namespace stream {
  void
  apply_transport_stats_to_quality_feedback(const AlkTransportStats &stats,
                                            stream_quality::feedback_t &feedback);
}
