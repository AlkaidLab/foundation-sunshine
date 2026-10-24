/**
 * @file vdd_primary_plan.h
 * @brief Pure rules for who keeps the primary role while a virtual display
 *        exists — the Linux counterpart of upstream 0219abe2 (#1175).
 *
 * Windows models the primary display as "the first group of the topology", so
 * upstream records the display that was primary *before* the virtual display
 * was created and rotates it back to the front instead of trusting the
 * enumeration order ("枚举结果不保证首个设备就是原主屏"). Linux has no ordered
 * topology — each compositor exposes a per-output primary flag (kscreen
 * priority 1, Mutter's logical-monitor `primary`, RandR's primary output) — but
 * the same hazard existed in a different shape: the VDD prep handed the primary
 * role to *every* physical output in turn, so the compositor ended up holding
 * whichever one it happened to see last. On a multi-monitor host that can be a
 * screen the user was not looking at.
 *
 * These rules are pure so the ordering contract can be tested without a
 * compositor; the backend keeps only the state they are applied to.
 */
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace platf::vdd_primary {

  /**
   * @brief The role a hint asks a connector to take.
   */
  enum class role_e {
    primary,   ///< kscreen priority 1 / the compositor's primary flag
    secondary, ///< kscreen priority 2 / extended
  };

  /**
   * @brief One connector to hint, and the role to ask for.
   */
  struct hint_t {
    std::string connector;
    role_e role;

    bool operator==(const hint_t &) const = default;
  };

  /**
   * @brief Hints for "physical displays primary, virtual display secondary".
   * @param physicals Connected physical connectors, in enumeration order.
   * @param vdd The virtual display's connector, or empty when only the physical
   *        side is being restored (teardown).
   * @param original_primary The connector that held the primary role before the
   *        virtual display was prepared, or empty when it could not be
   *        determined.
   * @return The hints to emit, in order.
   * @details Only a *known* original primary is ever promoted, and that
   *          promotion is the only hint naming a primary: granting the role to
   *          several outputs in turn is exactly the enumeration-order
   *          dependence this rule removes. When the original primary is unknown
   *          (not among the physicals any more, or never observed) the physical
   *          side is left untouched and only the virtual display is demoted —
   *          the compositor keeps the primary it already had, which is the only
   *          answer that cannot hand the role to the wrong screen. Every
   *          non-primary connector present is demoted explicitly so a leftover
   *          priority from an earlier prep cannot outlive it.
   */
  inline std::vector<hint_t>
  secondary_plan(const std::vector<std::string> &physicals, const std::string &vdd, const std::string &original_primary) {
    const bool known = !original_primary.empty() && original_primary != vdd &&
                       std::find(physicals.begin(), physicals.end(), original_primary) != physicals.end();

    std::vector<hint_t> plan;

    if (known) {
      plan.push_back({ original_primary, role_e::primary });
    }

    for (const auto &physical : physicals) {
      if (known && physical == original_primary) {
        continue;
      }
      if (!known) {
        // Nothing is promoted, so nothing may be demoted either: a demotion
        // here would push the compositor into choosing a new primary.
        continue;
      }
      plan.push_back({ physical, role_e::secondary });
    }

    if (!vdd.empty()) {
      plan.push_back({ vdd, role_e::secondary });
    }

    return plan;
  }

}  // namespace platf::vdd_primary
