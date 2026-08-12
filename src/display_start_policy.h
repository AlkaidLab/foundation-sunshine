/**
 * @file src/display_start_policy.h
 * @brief Pure policy helpers for stream-start display configuration results.
 */
#pragma once

#include "display_device/session.h"

namespace nvhttp::stream_start::policy {

  enum class configure_outcome_e {
    ok,
    retry_later,
    mode_refused,
    topology_refused,
    current_only,
    fatal
  };

  inline configure_outcome_e
  classify_configure_result(display_device::session_t::configure_result_t::result_e result) {
    using result_e = display_device::session_t::configure_result_t::result_e;

    switch (result) {
      case result_e::success:
        return configure_outcome_e::ok;
      case result_e::vdd_prepare_deferred:
      case result_e::deferred_retry:
        return configure_outcome_e::retry_later;
      case result_e::modes_fail:
      case result_e::hdr_states_fail:
        return configure_outcome_e::mode_refused;
      case result_e::topology_fail:
      case result_e::primary_display_fail:
        return configure_outcome_e::topology_refused;
      case result_e::file_save_fail:
      case result_e::revert_fail:
        return configure_outcome_e::current_only;
      case result_e::parse_fail:
      case result_e::vdd_not_installed:
      case result_e::vdd_unavailable:
      case result_e::vdd_create_failed:
      default:
        return configure_outcome_e::fatal;
    }
  }

  enum class deferred_retry_action_e {
    retry_without_probe,
    probe,
    stop_without_probe
  };

  inline bool
  display_retry_is_pending(display_device::session_t::configure_result_t::result_e result) {
    using result_e = display_device::session_t::configure_result_t::result_e;
    return result == result_e::deferred_retry || result == result_e::vdd_prepare_deferred;
  }

  inline deferred_retry_action_e
  deferred_retry_action(display_device::session_t::configure_result_t::result_e result) {
    using result_e = display_device::session_t::configure_result_t::result_e;

    switch (result) {
      case result_e::vdd_prepare_deferred:
      case result_e::deferred_retry:
        return deferred_retry_action_e::retry_without_probe;
      case result_e::success:
        return deferred_retry_action_e::probe;
      default:
        return deferred_retry_action_e::stop_without_probe;
    }
  }

}  // namespace nvhttp::stream_start::policy
