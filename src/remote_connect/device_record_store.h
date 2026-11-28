#pragma once

#include <filesystem>
#include <optional>

#include "credential_transaction.h"

namespace remote_connect::device_record_store {
  enum class load_status_e {
    // No file yet: nothing has been enrolled on this host.
    fresh,
    // A well-formed record.
    loaded,
    // The file exists but is not a well-formed device record.
    invalid,
    // The file exists but could not be read.
    unreadable,
  };

  struct load_result_t {
    load_status_e status;
    // The stored record when the status is `loaded`, otherwise a default one.
    credential_transaction::record_t record;
  };

  // Reads the device record, which is the single source of truth for which
  // remote devices this host authorizes.
  //
  // `invalid` and `unreadable` must stop remote access rather than be treated
  // as an empty record: an empty record would authorize nobody, which is
  // safe, but the next enrollment would then overwrite a file that may still
  // hold this host's devices.
  load_result_t
  load(const std::filesystem::path &path);

  // Replaces the stored record in one step: the new record is written to a
  // temporary file beside the old one, access to that file is restricted on
  // Windows to SYSTEM, administrators and its owner, and it is then moved
  // over the previous record. A failure leaves the previous record as it was.
  //
  // The record holds public keys only, but its integrity matters as much as a
  // secret's: whoever can write it can authorize a device. Returns false for a
  // record that is not well-formed, without writing anything.
  bool
  store(const std::filesystem::path &path, const credential_transaction::record_t &record);
}  // namespace remote_connect::device_record_store
