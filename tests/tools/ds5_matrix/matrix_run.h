/**
 * @file tests/tools/ds5_matrix/matrix_run.h
 * @brief Runs the case table on a rig and prints one tab-separated row per key.
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "tests/tools/ds5_matrix/cases_device.h"
#include "tests/tools/ds5_matrix/cases_host.h"
#include "tests/tools/ds5_matrix/cases_input.h"
#include "tests/tools/ds5_matrix/live_checks.h"
#include "tests/tools/ds5_matrix/rumble_styles.h"

namespace ds5::matrix {
  inline std::string
  one_line(std::string text) {
    for (auto &c : text) {
      if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return text;
  }

  inline void
  print_row(const result_t &row) {
    std::cout << "ROW\t" << row.key << '\t' << key_name(row.key) << '\t' << row.status << '\t' << row.layer << '\t' << one_line(row.evidence) << std::endl;
  }

  /** @brief Runs every case (or only the keys listed in @p only) and returns the number of FAIL rows. */
  inline int
  run_matrix(rig_t &rig, const std::set<int> &only_keys) {
    std::vector<case_t> table;
    append_host_cases(table);
    append_device_cases(table);
    append_input_cases(table);
    std::sort(table.begin(), table.end(), [](const case_t &a, const case_t &b) { return a.key < b.key; });
    int failures = 0, passes = 0;
    for (const auto &c : table) {
      if (!only_keys.empty() && !only_keys.contains(c.key)) continue;
      const auto row = run_case(rig, c);
      print_row(row);
      (row.status == "PASS" ? passes : failures) += 1;
    }
    for (const auto &u : unavailable_keys()) {
      if (!only_keys.empty() && !only_keys.contains(u.key)) continue;
      const auto row = run_unavailable(rig, u.key, u.valid, u.probe, u.where);
      print_row(row);
      (row.status == "PASS" ? passes : failures) += 1;
    }
    std::cout << "SUMMARY\t" << rig.name() << "\tpass=" << passes << "\tfail=" << failures << std::endl;
    if (only_keys.empty()) {
      for (const auto &row : run_live_checks(rig)) {
        std::cout << "LIVE\t" << rig.name() << '\t' << row.name << '\t' << (row.pass ? "PASS" : "FAIL") << '\t' << one_line(row.evidence) << std::endl;
        if (!row.pass) ++failures;
      }
      run_rumble_styles(rig);
    }
    return failures;
  }

  inline std::set<int>
  parse_keys(int argc, char **argv) {
    std::set<int> keys;
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], "--key") == 0 && i + 1 < argc) keys.insert(std::atoi(argv[++i]));
    }
    return keys;
  }
}  // namespace ds5::matrix
