#include "credential_transaction.h"

#include <algorithm>
#include <limits>
#include <set>

namespace remote_connect::credential_transaction {
  namespace {
    template<std::size_t N>
    bool
    all_zero(const std::array<std::uint8_t, N> &bytes) {
      return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value == 0; });
    }

    bool
    valid_device(const device_t &device) {
      return !device.id.empty() && device.id.size() <= 64 &&
             std::all_of(device.id.begin(), device.id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
             }) &&
             !all_zero(device.easytier_public_key) && !all_zero(device.gamestream_certificate_sha256);
    }

    bool
    valid_record(const record_t &record) {
      switch (record.phase) {
        case phase_t::stable:
        case phase_t::add_prepared:
        case phase_t::add_verified_closed:
        case phase_t::add_committed_closed:
        case phase_t::add_rollback_closed:
        case phase_t::revoke_pending:
        case phase_t::revoke_verified_closed:
        case phase_t::revoke_committed_closed:
        case phase_t::recovery_closed: break;
        default: return false;
      }
      if (record.active.size() > 1024 || record.tombstones.size() > 1024) return false;
      std::set<public_key_t> keys;
      std::set<public_key_t> certificates;
      const auto check = [&keys, &certificates](const auto &entries) {
        for (const auto &[id, device] : entries) {
          if (id != device.id || !valid_device(device) || !keys.insert(device.easytier_public_key).second ||
              !certificates.insert(device.gamestream_certificate_sha256).second) return false;
        }
        return true;
      };
      if (!check(record.active) || !check(record.tombstones)) return false;
      for (const auto &[id, ignored] : record.tombstones) {
        (void) ignored;
        if (record.active.contains(id)) return false;
      }
      const bool is_add_phase = record.phase == phase_t::add_prepared || record.phase == phase_t::add_verified_closed ||
                                record.phase == phase_t::add_committed_closed || record.phase == phase_t::add_rollback_closed;
      if (is_add_phase != record.candidate.has_value()) return false;
      const bool is_revoke_phase = record.phase == phase_t::revoke_pending || record.phase == phase_t::revoke_verified_closed ||
                                   record.phase == phase_t::revoke_committed_closed;
      if (is_revoke_phase != record.revoke_target.has_value()) return false;
      if (record.revoke_target && !record.tombstones.contains(*record.revoke_target)) return false;
      if (record.phase == phase_t::stable && !all_zero(record.transaction_id)) return false;
      if (record.phase != phase_t::stable && (all_zero(record.transaction_id) || record.transaction_id == record.last_transaction_id)) return false;
      if (record.candidate) {
        if (!valid_device(*record.candidate) || record.tombstones.contains(record.candidate->id)) return false;
        const auto active = record.active.find(record.candidate->id);
        if (record.phase == phase_t::add_committed_closed) {
          if (active == record.active.end() || active->second != *record.candidate) return false;
        }
        else if (active != record.active.end() || keys.contains(record.candidate->easytier_public_key) ||
                 certificates.contains(record.candidate->gamestream_certificate_sha256)) return false;
      }
      return true;
    }
  }  // namespace

  std::optional<record_t>
  reduce(const record_t &before, const event_t &event) {
    if (!valid_record(before) || event.expected_revision != before.revision ||
        before.revision == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    if (event.action == action_t::prepare_add) {
      if (!event.candidate || !event.target_id.empty()) return std::nullopt;
    }
    else if (event.action == action_t::prepare_revoke) {
      if (event.candidate || event.target_id.empty()) return std::nullopt;
    }
    else if (event.candidate || !event.target_id.empty()) return std::nullopt;
    auto after = before;
    const bool matches = !all_zero(event.transaction_id) && event.transaction_id == before.transaction_id;
    switch (event.action) {
      case action_t::prepare_add:
        if (before.phase != phase_t::stable || all_zero(event.transaction_id) || event.transaction_id == before.last_transaction_id ||
            !event.candidate || !valid_device(*event.candidate) || before.active.contains(event.candidate->id) ||
            before.tombstones.contains(event.candidate->id)) return std::nullopt;
        for (const auto &[id, device] : before.active) {
          (void) id;
          if (device.easytier_public_key == event.candidate->easytier_public_key ||
              device.gamestream_certificate_sha256 == event.candidate->gamestream_certificate_sha256) return std::nullopt;
        }
        for (const auto &[id, device] : before.tombstones) {
          (void) id;
          if (device.easytier_public_key == event.candidate->easytier_public_key ||
              device.gamestream_certificate_sha256 == event.candidate->gamestream_certificate_sha256) return std::nullopt;
        }
        after.transaction_id = event.transaction_id;
        after.candidate = event.candidate;
        after.phase = phase_t::add_prepared;
        break;
      case action_t::prepare_revoke: {
        if (before.phase != phase_t::stable || all_zero(event.transaction_id) || event.transaction_id == before.last_transaction_id) return std::nullopt;
        const auto found = after.active.find(event.target_id);
        if (found == after.active.end()) return std::nullopt;
        after.tombstones.emplace(found->first, found->second);
        after.active.erase(found);
        after.revoke_target = event.target_id;
        after.transaction_id = event.transaction_id;
        after.phase = phase_t::revoke_pending;
        break;
      }
      case action_t::add_verified:
        if (!matches || before.phase != phase_t::add_prepared) return std::nullopt;
        after.phase = phase_t::add_verified_closed;
        break;
      case action_t::commit_add:
        if (!matches || before.phase != phase_t::add_verified_closed || !before.candidate) return std::nullopt;
        after.active.emplace(before.candidate->id, *before.candidate);
        after.phase = phase_t::add_committed_closed;
        break;
      case action_t::abort_add:
        if (!matches || (before.phase != phase_t::add_prepared && before.phase != phase_t::add_verified_closed)) return std::nullopt;
        after.phase = phase_t::add_rollback_closed;
        break;
      case action_t::revoke_verified:
        if (!matches || before.phase != phase_t::revoke_pending) return std::nullopt;
        after.phase = phase_t::revoke_verified_closed;
        break;
      case action_t::commit_revoke:
        if (!matches || before.phase != phase_t::revoke_verified_closed) return std::nullopt;
        after.phase = phase_t::revoke_committed_closed;
        break;
      case action_t::finish_closed:
        if (!matches || (before.phase != phase_t::add_committed_closed && before.phase != phase_t::add_rollback_closed &&
                         before.phase != phase_t::revoke_committed_closed && before.phase != phase_t::recovery_closed)) return std::nullopt;
        after.last_transaction_id = before.transaction_id;
        after.transaction_id = {};
        after.candidate.reset();
        after.revoke_target.reset();
        after.phase = phase_t::stable;
        break;
      case action_t::recover_closed:
        // Recovery must first close access and stop old children. It preserves
        // only the already committed active map, including all tombstones.
        if (before.phase == phase_t::recovery_closed) return std::nullopt;
        if (before.phase != phase_t::stable && !matches) return std::nullopt;
        if (before.phase == phase_t::stable) {
          if (all_zero(event.transaction_id) || event.transaction_id == before.last_transaction_id) return std::nullopt;
          after.transaction_id = event.transaction_id;
        }
        after.candidate.reset();
        after.revoke_target.reset();
        after.phase = phase_t::recovery_closed;
        break;
      default: return std::nullopt;
    }
    ++after.revision;
    if (!valid_record(after)) return std::nullopt;
    return after;
  }
}  // namespace remote_connect::credential_transaction
