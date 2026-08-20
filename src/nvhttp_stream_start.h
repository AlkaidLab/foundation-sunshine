/**
 * @file src/nvhttp_stream_start.h
 * @brief Helpers for GameStream launch/resume display preparation, encoder probing, and startup recovery.
 */
#pragma once

#include "rtsp.h"

#include <boost/property_tree/ptree.hpp>
#include <string>

namespace nvhttp::stream_start {

  void
  set_sunshine_error(
    boost::property_tree::ptree &tree,
    int status_code,
    const std::string &status_message,
    const std::string &error_code,
    const std::string &hint,
    const std::string &recovery_action,
    const std::string &source,
    const std::string &stage,
    bool recoverable);

  bool
  prepare_display_and_probe_encoders(
    boost::property_tree::ptree &tree,
    rtsp_stream::launch_session_t &launch_session,
    bool is_reconfigure);

}  // namespace nvhttp::stream_start
