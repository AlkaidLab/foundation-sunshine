/**
 * @file tests/tools/ds5_matrix/cases_common.h
 * @brief Helpers shared by the case tables.
 */
#pragma once

#include <set>

#include "tests/tools/ds5_matrix/probes_misc.h"

namespace ds5::matrix {
  namespace cases_detail {
    inline bool
    near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

    inline std::string
    want(const char *what, double got, double wanted) { return std::string(what) + " " + fmt(got) + " (want " + fmt(wanted) + "); "; }

    /** @brief Checks that only the listed indexes moved from @p base. */
    inline std::string
    only(const sig_t &base, const sig_t &value, const std::function<bool(std::size_t)> &allowed, const char *what) {
      for (std::size_t i = 0; i < base.size() && i < value.size(); ++i) {
        if (!allowed(i) && std::fabs(base[i] - value[i]) > 1e-6) return std::string(what) + " moved index " + std::to_string(i) + " (" + fmt(base[i]) + " -> " + fmt(value[i]) + "); ";
      }
      return {};
    }

    inline value_case_t
    val(const json &patch, std::string label, bool must_differ = true) { return { patch, std::move(label), must_differ }; }

    inline json
    kv(int key, const json &value) { return { { key_name(key), value } }; }
  }  // namespace cases_detail

  namespace cases_detail {
    using vecs = std::vector<sig_t>;

    /** @brief Appends cases to a table; `simple` covers the common shape of one probe and a few values. */
    struct builder_t {
      std::vector<case_t> &out;

      void
      add(case_t c) { out.push_back(std::move(c)); }

      void
      simple(int key, std::function<sig_t(rig_t &)> probe, std::vector<value_case_t> values,
        std::function<std::string(const sig_t &, const vecs &)> expect, json setup = json::object()) {
        case_t c;
        c.key = key;
        c.probe = std::move(probe);
        c.values = std::move(values);
        c.expect = std::move(expect);
        c.setup = std::move(setup);
        add(std::move(c));
      }
    };
  }  // namespace cases_detail
}  // namespace ds5::matrix
