#include "enrollment_proof.h"
#include "enrollment_proof_internal.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace remote_connect {
  namespace detail {
    bool
    matches_clamped_zero_x25519_private(const enrollment_bytes32_t &private_key) {
      // Compare canonical clamped scalars, not raw encodings: the low three
      // bits and the high two bits do not contribute to the X25519 scalar.
      auto clamped = private_key;
      clamped[0] &= 248;
      clamped[31] &= 127;
      clamped[31] |= 64;
      enrollment_bytes32_t zero_clamped {};
      zero_clamped[31] = 64;
      const bool matches_zero_input = CRYPTO_memcmp(clamped.data(), zero_clamped.data(), clamped.size()) == 0;
      OPENSSL_cleanse(clamped.data(), clamped.size());
      return matches_zero_input;
    }

    bool
    nonzero_x25519_shared_secret(const enrollment_bytes32_t &shared) {
      const enrollment_bytes32_t zero {};
      return CRYPTO_memcmp(shared.data(), zero.data(), zero.size()) != 0;
    }
  }  // namespace detail

  namespace {
    using bytes_t = std::vector<std::uint8_t>;

    struct sensitive_bytes_t {
      enrollment_bytes32_t value {};
      ~sensitive_bytes_t() { OPENSSL_cleanse(value.data(), value.size()); }
    };

    using pkey_t = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using pkey_ctx_t = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

    template<std::size_t N>
    void
    append(bytes_t &out, const std::array<std::uint8_t, N> &input) {
      out.insert(out.end(), input.begin(), input.end());
    }

    template<std::size_t N>
    void
    append_label(bytes_t &out, const char (&label)[N]) {
      // N includes the terminating NUL specified by the v1 draft.
      out.insert(out.end(), label, label + N);
    }

    bool
    sha256(const void *data, std::size_t length, enrollment_bytes32_t &out) {
      unsigned int produced = 0;
      return EVP_Digest(data, length, out.data(), &produced, EVP_sha256(), nullptr) == 1 && produced == out.size();
    }

    bool
    hmac_sha256(const enrollment_bytes32_t &key, const std::uint8_t *data, std::size_t length, enrollment_bytes32_t &out) {
      unsigned int produced = 0;
      return HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data, length, out.data(), &produced) != nullptr && produced == out.size();
    }

    bool
    hmac_sha256(const enrollment_bytes32_t &key, const bytes_t &message, enrollment_bytes32_t &out) {
      return hmac_sha256(key, message.data(), message.size(), out);
    }

    bool
    acceptable_context(const enrollment_proof_context_t &context) {
      const std::string_view peer(context.discovery_peer);
      if (context.version != 1 || context.enrollment_port == 0 || peer.size() <= 6 || peer.size() > 512 || !peer.starts_with("udp://")) return false;
      return std::all_of(peer.begin(), peer.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
    }

    bool
    shared_secret(const enrollment_bytes32_t &private_key, const enrollment_bytes32_t &expected_public,
                  const enrollment_bytes32_t &peer_public, enrollment_bytes32_t &out) {
      if (detail::matches_clamped_zero_x25519_private(private_key)) return false;
      pkey_t host(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), private_key.size()), EVP_PKEY_free);
      pkey_t peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public.data(), peer_public.size()), EVP_PKEY_free);
      if (!host || !peer) return false;

      enrollment_bytes32_t actual_public {};
      std::size_t public_len = actual_public.size();
      if (EVP_PKEY_get_raw_public_key(host.get(), actual_public.data(), &public_len) != 1 || public_len != actual_public.size() ||
          CRYPTO_memcmp(actual_public.data(), expected_public.data(), actual_public.size()) != 0) return false;

      pkey_ctx_t derive(EVP_PKEY_CTX_new(host.get(), nullptr), EVP_PKEY_CTX_free);
      if (!derive || EVP_PKEY_derive_init(derive.get()) != 1 || EVP_PKEY_derive_set_peer(derive.get(), peer.get()) != 1) return false;
      std::size_t produced = out.size();
      if (EVP_PKEY_derive(derive.get(), out.data(), &produced) != 1 || produced != out.size()) return false;
      return detail::nonzero_x25519_shared_secret(out);
    }
  }  // namespace

  std::optional<enrollment_confirmation_t>
  verify_enrollment_proof_v1(const enrollment_proof_context_t &context, const enrollment_bytes32_t &token,
                             const enrollment_bytes32_t &host_private, const enrollment_bytes32_t &proof) {
    if (!acceptable_context(context)) return std::nullopt;
    sensitive_bytes_t shared;
    if (!shared_secret(host_private, context.host_ephemeral_public, context.client_public, shared.value)) return std::nullopt;

    enrollment_bytes32_t token_digest {};
    if (!sha256(token.data(), token.size(), token_digest)) return std::nullopt;
    bytes_t transcript;
    transcript.reserve(241 + context.discovery_peer.size());
    append_label(transcript, "VPLUS-ENROLL-POP-V1");
    transcript.push_back(context.version);
    append(transcript, token_digest);
    append(transcript, context.host_certificate_sha256);
    append(transcript, context.bootstrap_id);
    append(transcript, context.challenge);
    append(transcript, context.host_ephemeral_public);
    append(transcript, context.client_public);
    append(transcript, context.client_certificate_sha256);
    append(transcript, context.host_ipv4);
    append(transcript, context.client_ipv4);
    transcript.push_back(static_cast<std::uint8_t>(context.enrollment_port >> 8));
    transcript.push_back(static_cast<std::uint8_t>(context.enrollment_port));
    transcript.push_back(static_cast<std::uint8_t>(context.discovery_peer.size() >> 8));
    transcript.push_back(static_cast<std::uint8_t>(context.discovery_peer.size()));
    transcript.insert(transcript.end(), context.discovery_peer.begin(), context.discovery_peer.end());

    bytes_t input;
    append_label(input, "VPLUS-ENROLL-HKDF-SALT-V1");
    input.insert(input.end(), transcript.begin(), transcript.end());
    enrollment_bytes32_t salt {};
    if (!sha256(input.data(), input.size(), salt)) return std::nullopt;

    sensitive_bytes_t prk;
    if (!hmac_sha256(salt, shared.value.data(), shared.value.size(), prk.value)) return std::nullopt;
    input.clear();
    append_label(input, "VPLUS-ENROLL-POP-KEY-V1");
    input.push_back(1);
    sensitive_bytes_t proof_key;
    if (!hmac_sha256(prk.value, input, proof_key.value)) return std::nullopt;
    input.clear();
    append_label(input, "VPLUS-ENROLL-POP-PROOF-V1");
    input.insert(input.end(), transcript.begin(), transcript.end());
    sensitive_bytes_t expected_proof;
    if (!hmac_sha256(proof_key.value, input, expected_proof.value) ||
        CRYPTO_memcmp(expected_proof.value.data(), proof.data(), proof.size()) != 0) return std::nullopt;

    input.clear();
    append_label(input, "VPLUS-ENROLL-SAS-V1");
    input.insert(input.end(), transcript.begin(), transcript.end());
    append(input, proof);
    enrollment_bytes32_t confirmation_hash {};
    if (!sha256(input.data(), input.size(), confirmation_hash)) return std::nullopt;
    enrollment_confirmation_t confirmation {};
    std::copy_n(confirmation_hash.begin(), confirmation.size(), confirmation.begin());
    return confirmation;
  }
}  // namespace remote_connect
