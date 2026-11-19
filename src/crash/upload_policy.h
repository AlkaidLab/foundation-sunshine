/**
 * @file src/crash/upload_policy.h
 * @brief What the crash uploader does with an answer, and how long it waits after a failure. Pure code (std only).
 *
 * A crash report is kept on disk until the service has it. Only an answer that says "this body will never be accepted" drops it (400, 415, 422, and 413 once the dump is left out); everything else, also the answers
 * a proxy, a firewall or an older service may give for other reasons, keeps it and waits: Retry-After when the service gave one, else 15 minutes doubling to four hours, always spread by a random share so the
 * computers that failed together do not return together.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace crash::upload_policy {

  enum class verdict_e {
    delivered,  ///< 2xx: the service has it
    drop,  ///< the service will never accept this body
    send_without_dump,  ///< 413 with a dump: the dump is too large for the service, the report is not
    wait,  ///< keep it and try again later (the failure counts: the next wait is longer)
    keep_going,  ///< keep it, but the answer says nothing about the service being unwell (a 408 of a slow line, a redirect): try the next report
  };

  inline constexpr long MIN_WAIT_S = 30;
  inline constexpr long MAX_WAIT_S = 4 * 3600;
  inline constexpr long BASE_WAIT_S = 15 * 60;
  /** The most a wait is stretched by the random share: a quarter. */
  inline constexpr double SPREAD = 0.25;

  /** The verdict for an HTTP status (0: nothing answered). `has_dump`: the request carried a dump. */
  inline verdict_e
  judge(long status, bool has_dump) {
    if (status >= 200 && status < 300) return verdict_e::delivered;
    if (status == 413) return has_dump ? verdict_e::send_without_dump : verdict_e::drop;
    if (status == 400 || status == 415 || status == 422) return verdict_e::drop;
    if (status == 408) return verdict_e::keep_going;
    return verdict_e::wait;  // no answer, 429, 5xx, a proxy's 401, 403, 404, 405, 410, 421, a redirect, anything else: the report stays
  }

  /**
   * Seconds to wait after the `failures`-th failure in a row (1 for the first). `retry_after_s` is the service's own word (0: none): it is believed, kept inside [30 s, 4 h]. Without it 15 minutes, doubling,
   * to four hours. `jitter` in [0, 1) stretches the wait by up to a quarter.
   */
  inline long
  wait_seconds(long failures, long retry_after_s, double jitter) {
    const double spread = 1.0 + SPREAD * std::clamp(jitter, 0.0, 0.999);
    long base = 0;
    if (retry_after_s > 0)
      base = std::clamp(retry_after_s, MIN_WAIT_S, MAX_WAIT_S);
    else
      base = std::min(MAX_WAIT_S, BASE_WAIT_S << std::clamp(failures - 1, 0L, 5L));
    return std::min(MAX_WAIT_S + MAX_WAIT_S / 4, static_cast<long>(std::lround(static_cast<double>(base) * spread)));
  }

}  // namespace crash::upload_policy
