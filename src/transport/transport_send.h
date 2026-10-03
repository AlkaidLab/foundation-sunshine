#pragma once

#include <cstddef>

namespace transport {
  struct submission_result_t {
    std::size_t submitted = 0;
    std::size_t failed = 0;
    bool prefix_valid = true;
  };

  // The batch operation reports its exact successful prefix. Individual
  // retries can have holes; only confirmed successes reach the send ledger.
  template<class Batch, class Single, class Confirm>
  submission_result_t submit_packet_batch(std::size_t count, Batch &&batch, Single &&single, Confirm &&confirm) {
    const auto prefix = batch();
    if (prefix > count) return {0, 0, false};
    submission_result_t result;
    for (std::size_t i = 0; i < prefix; ++i) {
      confirm(i);
      ++result.submitted;
    }
    for (std::size_t i = prefix; i < count; ++i) {
      if (single(i)) {
        confirm(i);
        ++result.submitted;
      }
      else ++result.failed;
    }
    return result;
  }
}
