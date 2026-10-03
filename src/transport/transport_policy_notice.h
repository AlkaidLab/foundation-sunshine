#pragma once
#include "transport_policy.h"
#include "third-party/moonlight-common-c/src/TransportPolicyStatus.h"
#include <array>
#include <optional>

namespace transport {
  // Pure latest-state projection. It neither grants a lease nor marks media delivered.
  std::optional<TPS_STATUS_NOTICE>
  policy_notice(const policy_snapshot_t &snapshot, std::uint32_t session_id, std::uint64_t sequence);

  class policy_notice_sender_t {
  public:
    std::optional<std::array<std::uint8_t, TPS_STATUS_BYTES>>
    prepare(const policy_snapshot_t &snapshot, std::uint32_t session_id, std::int64_t now_us) const;
    void
    queued(const std::array<std::uint8_t, TPS_STATUS_BYTES> &body, std::int64_t now_us);

  private:
    std::optional<std::array<std::uint8_t, TPS_STATUS_BYTES>> last_;
    std::int64_t last_queued_us_ = -1;
  };
}  // namespace transport
