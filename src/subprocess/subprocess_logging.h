/**
 * @file src/subprocess/subprocess_logging.h
 * @brief Simple logging wrapper for subprocess module.
 *
 * This header provides a unified logging interface that works in both:
 * - Main Sunshine process (uses full Boost.Log infrastructure)
 * - Subprocess sender (uses simple stderr logging)
 */
#pragma once

#ifdef SUBPROCESS_STANDALONE
  // Standalone subprocess - use simple stderr logging
  #include <iostream>
  #include <sstream>

  #define SUBPROCESS_LOG(level) subprocess::log_stream(#level)

namespace subprocess {
  class log_stream {
  public:
    explicit log_stream(const char *level):
        level_(level) {}

    ~log_stream() {
      std::cerr << "[" << level_ << "] " << stream_.str() << std::endl;
    }

    template <typename T>
    log_stream &
    operator<<(const T &value) {
      stream_ << value;
      return *this;
    }

  private:
    const char *level_;
    std::ostringstream stream_;
  };
}  // namespace subprocess

#else
  // Main process - use Boost.Log
  #include "src/logging.h"

  #define SUBPROCESS_LOG(level) BOOST_LOG(level)

#endif
