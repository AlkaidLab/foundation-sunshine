/**
 * @file src/streaming/expected_failures.h
 * @brief Failures that the host meets in normal operation and must not report as errors.
 */
#pragma once

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

namespace streaming {

  /**
   * @brief Whether a finished `async_accept` was cancelled by closing its acceptor.
   * @details Stopping the RTSP server closes the acceptor, which completes the pending accept with
   *          `operation_aborted`. That is the shutdown path, not a failure to accept a connection.
   */
  inline bool
  accept_was_cancelled(const boost::system::error_code &ec) {
    return ec == boost::asio::error::operation_aborted;
  }

  /**
   * @brief Tells a retry loop whether a failure is worth a log line.
   * @details The loop that looks for a UPnP router runs every few minutes for as long as the host runs.
   *          A home without a UPnP router fails the same way every time, so the first failure and every
   *          change of the failure are logged, and the repeats are not. A success starts over.
   */
  class repeated_failure_gate {
  public:
    /**
     * @brief Records a failure.
     * @param status The failure, any value that is equal for the same failure.
     * @return `true` if this failure differs from the one recorded last (or none was recorded).
     */
    bool
    failed(int status) {
      if (has_last && last == status) {
        return false;
      }
      has_last = true;
      last = status;
      return true;
    }

    /// @brief Records a success: the next failure is new again.
    void
    succeeded() {
      has_last = false;
    }

  private:
    bool has_last = false;
    int last = 0;
  };

}  // namespace streaming
