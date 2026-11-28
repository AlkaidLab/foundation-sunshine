#pragma once

#include "enrollment_proof.h"

namespace remote_connect::detail {
  // X25519 clears the lowest three bits and replaces the top two bits during
  // clamping. All encodings of the all-zero input scalar are invalid here.
  bool
  matches_clamped_zero_x25519_private(const enrollment_bytes32_t &private_key);

  // Kept separately testable because some OpenSSL providers reject low-order
  // keys before returning an all-zero shared secret.
  bool
  nonzero_x25519_shared_secret(const enrollment_bytes32_t &shared);
}
