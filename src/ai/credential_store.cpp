/**
 * @file src/ai/credential_store.cpp
 * @brief DPAPI on Windows, the Secret Service on Linux, environment-only elsewhere.
 */

#include "credential_store.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
  #include <sddl.h>
  #include <wincrypt.h>
#elif defined(SUNSHINE_BUILD_LIBSECRET)
  #include <libsecret/secret.h>
#endif

namespace credential_store {
  namespace {
#if defined(_WIN32)
    DATA_BLOB
    entropy_blob() {
      static constexpr char entropy[] = "Sunshine/LLM/APIKey/v1";
      return {
        static_cast<DWORD>(sizeof(entropy) - 1),
        reinterpret_cast<BYTE *>(const_cast<char *>(entropy))
      };
    }

    std::string
    windows_error(const char *operation, DWORD error) {
      return std::string { operation } + " failed (Windows error " + std::to_string(error) + ")";
    }

    DWORD
    restrict_file_acl(const std::filesystem::path &path) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      constexpr auto sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)";
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        return GetLastError();
      }

      const BOOL ok = SetFileSecurityW(
        path.c_str(),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        descriptor);
      const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
      LocalFree(descriptor);
      return error;
    }

    mutation_result_t
    write_blob_atomically(const std::filesystem::path &path, const BYTE *data, DWORD size) {
      auto temp = path;
      temp += L".tmp";
      {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
          return { false, "Could not open the protected credential file" };
        }
        file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        file.flush();
        if (!file.good()) {
          file.close();
          std::error_code ignored;
          std::filesystem::remove(temp, ignored);
          return { false, "Could not write the protected credential file" };
        }
      }

      if (const DWORD error = restrict_file_acl(temp); error != ERROR_SUCCESS) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return { false, windows_error("Restricting credential file access", error) };
      }
      if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return { false, windows_error("Replacing credential file", error) };
      }
      return { true, {} };
    }
#elif defined(SUNSHINE_BUILD_LIBSECRET)
    // One keyring entry per user: there is no per-install file path to key on,
    // and the attributes are the whole lookup key.
    constexpr const char *key_application = "sunshine";
    constexpr const char *key_purpose = "llm-api-key";

    const SecretSchema *
    llm_key_schema() {
      static const SecretSchema schema = {
        "org.lizardbyte.sunshine.LlmApiKey",
        SECRET_SCHEMA_NONE,
        {
          { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
          { "purpose", SECRET_SCHEMA_ATTRIBUTE_STRING },
          { nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
      };
      return &schema;
    }

    // Takes ownership of `error`.
    std::string
    secret_error(const char *operation, GError *error) {
      std::string message { operation };
      if (!error) return message + " failed";
      message += ": ";
      message += error->message;
      g_error_free(error);
      return message;
    }

    bool
    secret_object_missing(GError *error) {
      return error && g_error_matches(error, SECRET_ERROR, SECRET_ERROR_NO_SUCH_OBJECT);
    }

    std::string
    environment_api_key() {
      const char *value = std::getenv("SUNSHINE_LLM_API_KEY");
      return value && *value ? std::string { value } : std::string {};
    }
#endif
  }  // namespace

  read_result_t
  read_llm_api_key(const std::filesystem::path &path) {
#if defined(_WIN32)
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      std::error_code ec;
      return std::filesystem::exists(path, ec)
               ? read_result_t { read_status_e::error, {}, "Could not open the protected credential file" }
               : read_result_t { read_status_e::not_found, {}, {} };
    }
    const auto length = file.tellg();
    if (length <= 0 || length > 64 * 1024) {
      return { read_status_e::error, {}, "Protected credential file has an invalid size" };
    }
    std::vector<BYTE> protected_data(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(protected_data.data()), length);
    if (!file.good()) {
      return { read_status_e::error, {}, "Could not read the protected credential file" };
    }

    DATA_BLOB input { static_cast<DWORD>(protected_data.size()), protected_data.data() };
    DATA_BLOB output {};
    auto entropy = entropy_blob();
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
      return { read_status_e::error, {}, windows_error("Decrypting LLM credential", GetLastError()) };
    }
    std::string secret(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return { read_status_e::success, std::move(secret), {} };
#elif defined(SUNSHINE_BUILD_LIBSECRET)
    (void) path;
    GError *error = nullptr;
    gchar *stored = secret_password_lookup_sync(
      llm_key_schema(), nullptr, &error,
      "application", key_application,
      "purpose", key_purpose,
      nullptr);
    if (stored) {
      std::string secret { stored };
      std::memset(stored, 0, std::strlen(stored));
      g_free(stored);
      return { read_status_e::success, std::move(secret), {} };
    }
    // A missing entry is the normal "nothing saved yet" case; anything else
    // (no Secret Service, locked collection) is a real error worth surfacing.
    if (error && !secret_object_missing(error)) {
      return { read_status_e::error, {}, secret_error("Reading the LLM credential from the keyring", error) };
    }
    if (error) g_error_free(error);

    if (auto fallback = environment_api_key(); !fallback.empty()) {
      return { read_status_e::success, std::move(fallback), {} };
    }
    return { read_status_e::not_found, {}, {} };
#else
    (void) path;
    const char *value = std::getenv("SUNSHINE_LLM_API_KEY");
    if (!value || !*value) return { read_status_e::not_found, {}, {} };
    return { read_status_e::success, value, {} };
#endif
  }

  mutation_result_t
  write_llm_api_key(const std::filesystem::path &path, const std::string &secret) {
    if (secret.empty()) return erase_llm_api_key(path);
    if (secret.size() > 16 * 1024) return { false, "The LLM API key is too large" };
#if defined(_WIN32)
    DATA_BLOB input {
      static_cast<DWORD>(secret.size()),
      reinterpret_cast<BYTE *>(const_cast<char *>(secret.data()))
    };
    DATA_BLOB output {};
    auto entropy = entropy_blob();
    if (!CryptProtectData(
          &input, L"Sunshine LLM API key", &entropy, nullptr, nullptr,
          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
          &output)) {
      return { false, windows_error("Encrypting LLM credential", GetLastError()) };
    }
    auto result = write_blob_atomically(path, output.pbData, output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#elif defined(SUNSHINE_BUILD_LIBSECRET)
    (void) path;
    GError *error = nullptr;
    const bool stored = secret_password_store_sync(
      llm_key_schema(), SECRET_COLLECTION_DEFAULT, "Sunshine LLM API key",
      secret.c_str(), nullptr, &error,
      "application", key_application,
      "purpose", key_purpose,
      nullptr);
    if (!stored) {
      return {
        false,
        secret_error("Storing the LLM credential in the keyring", error) +
          " (or set SUNSHINE_LLM_API_KEY in the service environment)"
      };
    }
    return { true, {} };
#else
    (void) path;
    const char *configured = std::getenv("SUNSHINE_LLM_API_KEY");
    if (configured && secret == configured) {
      // Allows a matching legacy plaintext value to be removed safely.
      return { true, {} };
    }
    return {
      false,
      "Secure API key persistence needs a Secret Service provider (libsecret); set SUNSHINE_LLM_API_KEY in the service environment instead"
    };
#endif
  }

  mutation_result_t
  erase_llm_api_key(const std::filesystem::path &path) {
#if defined(_WIN32)
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) return { false, "Could not remove the protected credential file: " + ec.message() };
    (void) removed;
    return { true, {} };
#elif defined(SUNSHINE_BUILD_LIBSECRET)
    (void) path;
    GError *error = nullptr;
    const bool cleared = secret_password_clear_sync(
      llm_key_schema(), nullptr, &error,
      "application", key_application,
      "purpose", key_purpose,
      nullptr);
    // libsecret reports "nothing matched" as FALSE with no GError at all (and,
    // depending on the version, sometimes as NO_SUCH_OBJECT). Erase is meant to
    // be idempotent, so only a real transport/collection failure is an error.
    if (!cleared && error && !secret_object_missing(error)) {
      return { false, secret_error("Clearing the LLM credential from the keyring", error) };
    }
    if (error) g_error_free(error);
    // The keyring entry is gone, but an environment-supplied key still wins on
    // read, so erase is only complete once the service environment is clean.
    const char *configured = std::getenv("SUNSHINE_LLM_API_KEY");
    if (configured && *configured) {
      return { false, "Remove SUNSHINE_LLM_API_KEY from the service environment to clear the API key" };
    }
    return { true, {} };
#else
    (void) path;
    const char *configured = std::getenv("SUNSHINE_LLM_API_KEY");
    if (configured && *configured) {
      return { false, "Remove SUNSHINE_LLM_API_KEY from the service environment to clear the API key" };
    }
    return { true, {} };
#endif
  }

}  // namespace credential_store
