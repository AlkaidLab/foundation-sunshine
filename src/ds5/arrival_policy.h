/**
 * @file src/ds5/arrival_policy.h
 * @brief What a controller arrival packet does to a slot that may already hold a virtual pad.
 *
 * A virtual pad is created by an arrival packet (which carries the pad's type, capabilities and
 * buttons) or, for older clients, by the first multi-controller packet whose active mask has the
 * pad's bit (which carries none of that). The second kind must not block the first: a slot made
 * without metadata is replaced when the real arrival comes, otherwise a pad that connects during a
 * stream would be refused because something else had created the slot first.
 */
#pragma once

#include <cstdint>

namespace ds5::arrival {
  struct meta_t {
    std::uint8_t type {};
    std::uint16_t capabilities {};
    std::uint32_t supported_buttons {};

    friend bool operator==(const meta_t &, const meta_t &) = default;
  };

  enum class action_e {
    allocate,  ///< the slot is free
    replace,  ///< the slot holds a pad made without metadata: free it and allocate with the arrival
    ignore_duplicate,  ///< the same pad announced again
    ignore_conflict,  ///< a pad made by an arrival is announced with other data and was not removed first
  };

  inline action_e
  decide(bool slot_allocated, bool allocated_by_arrival, const meta_t &existing, const meta_t &incoming) {
    if (!slot_allocated) return action_e::allocate;
    if (!allocated_by_arrival) return action_e::replace;
    return existing == incoming ? action_e::ignore_duplicate : action_e::ignore_conflict;
  }

  inline const char *
  to_string(action_e action) {
    switch (action) {
      case action_e::allocate: return "allocate";
      case action_e::replace: return "replace";
      case action_e::ignore_duplicate: return "ignore (same pad announced again)";
      case action_e::ignore_conflict: return "ignore (different data for a pad that was not removed)";
    }
    return "?";
  }
}  // namespace ds5::arrival
