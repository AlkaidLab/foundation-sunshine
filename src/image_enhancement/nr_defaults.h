#pragma once

#include <optional>

#include "src/platform/frame_contract.h"

namespace image_enhancement {
  struct nr_defaults_t {
    bool enabled = false;
    platf::pre_encode_filter_config_t filter;
  };

  // An absent file preserves the original per-app/default-off behavior.
  std::optional<nr_defaults_t> load_nr_defaults();
  bool save_nr_defaults(const nr_defaults_t &defaults);
}  // namespace image_enhancement
