/**
 * @file src/platform/linux/mirror_plan.h
 * @brief Work out the KScreen arguments that turn the current mirroring into the wanted one.
 */
#pragma once

// standard includes
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace platf::mirror {
  /**
   * @brief The parts of a reported output this decision needs.
   * @details `index` and `replication_source` are KScreen's numeric output ids:
   *          a mirrored output reports the id of the output it mirrors, or 0
   *          when it stands alone.
   */
  struct output_t {
    std::string name;
    int index = 0;
    bool enabled = false;
    int replication_source = 0;
  };

  /**
   * @brief Arguments to hand kscreen-doctor so the mirroring matches the groups.
   * @param current What the compositor reports right now.
   * @param wanted_groups The topology being applied; a group with two or more
   *        members mirrors them all onto its first member.
   * @return The arguments to send, empty when nothing has to change, or
   *         std::nullopt when the topology names an output the compositor does
   *         not report.
   * @details Only the relations that differ are emitted, and an output that is
   *          mirrored today but should not be gets an explicit `mirror.none`:
   *          leaving it out of the command keeps the stale relation.
   */
  inline std::optional<std::vector<std::string>>
  plan(const std::vector<output_t> &current, const std::vector<std::vector<std::string>> &wanted_groups) {
    std::vector<std::pair<std::string, std::string>> wanted;  // member -> source
    for (const auto &group : wanted_groups) {
      for (std::size_t i = 1; i < group.size(); ++i) {
        wanted.emplace_back(group[i], group.front());
      }
    }

    const auto find = [&current](const std::string &name) -> const output_t * {
      for (const auto &output : current) {
        if (output.name == name) {
          return &output;
        }
      }
      return nullptr;
    };

    std::vector<std::string> args;

    // Release what is mirrored today but no longer asked for.
    for (const auto &output : current) {
      if (!output.enabled || output.replication_source <= 0) {
        continue;
      }
      const bool still_wanted = std::any_of(wanted.begin(), wanted.end(),
        [&output](const auto &relation) { return relation.first == output.name; });
      if (!still_wanted) {
        args.push_back("output." + output.name + ".mirror.none");
      }
    }

    // Point the rest at their source.
    for (const auto &[member, source] : wanted) {
      const auto *output = find(member);
      const auto *source_output = find(source);
      if (!output || !source_output) {
        return std::nullopt;
      }
      if (output->replication_source == source_output->index) {
        continue;  // Already mirrors that output.
      }
      args.push_back("output." + member + ".mirror." + source);
    }

    return args;
  }

}  // namespace platf::mirror
