/**
 * @file src/amf/amf_encoded_frame.h
 * @brief Declarations for AMF encoded frame.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace amf {

  /**
   * @brief Encoded frame from AMF encoder.
   */
  struct amf_encoded_frame {
    std::vector<uint8_t> data;
    uint64_t frame_index = 0;
    bool idr = false;
    bool after_ref_frame_invalidation = false;
  };

}  // namespace amf
