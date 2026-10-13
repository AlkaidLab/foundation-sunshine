/**
 * @file src/ai/credential_store.cpp
 * @brief DPAPI storage on Windows and environment-only credentials elsewhere.
 */

#include "credential_store.h"

#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
  #include <sddl.h>
  #include <wincrypt.h>
#endif

namespace credential_store {
  namespace {
#if defined(_WIN32)
    DATA_BLOB
    entropy_blob(const char *entropy) {
      return {
        static_cast<DWORD>(std::char_traits<char>::length(entropy)),
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

    read_result_t
    read_dpapi_secret(const std::filesystem::path &path, const char *entropy_text) {
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
      auto entropy = entropy_blob(entropy_text);
      if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return { read_status_e::error, {}, windows_error("Decrypting credential", GetLastError()) };
      }
      std::string secret(reinterpret_cast<const char *>(output.pbData), output.cbData);
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
      return { read_status_e::success, std::move(secret), {} };
    }

    mutation_result_t
    write_dpapi_secret(const std::filesystem::path &path, const std::string &secret,
                       const char *entropy_text, const wchar_t *description, DWORD flags) {
      DATA_BLOB input {
        static_cast<DWORD>(secret.size()),
        reinterpret_cast<BYTE *>(const_cast<char *>(secret.data()))
      };
      DATA_BLOB output {};
      auto entropy = entropy_blob(entropy_text);
      if (!CryptProtectData(&input, description, &entropy, nullptr, nullptr,
                            flags | CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return { false, windows_error("Encrypting credential", GetLastError()) };
      }
      auto result = write_blob_atomically(path, output.pbData, output.cbData);
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
      return result;
    }
#endif
    mutation_result_t
    erase_credential_file(const std::filesystem::path &path) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
      if (ec) return { false, "Could not remove the protected credential file: " + ec.message() };
      return { true, {} };
    }
  }  // namespace

  read_result_t
  read_llm_api_key(const std::filesystem::path &path) {
#if defined(_WIN32)
    return read_dpapi_secret(path, "Sunshine/LLM/APIKey/v1");
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
    return write_dpapi_secret(path, secret, "Sunshine/LLM/APIKey/v1",
                              L"Sunshine LLM API key", CRYPTPROTECT_LOCAL_MACHINE);
#else
    (void) path;
    const char *configured = std::getenv("SUNSHINE_LLM_API_KEY");
    if (configured && secret == configured) {
      // Allows a matching legacy plaintext value to be removed safely.
      return { true, {} };
    }
    return {
      false,
      "Secure API key persistence is only supported on Windows; set SUNSHINE_LLM_API_KEY in the service environment"
    };
#endif
  }

  mutation_result_t
  erase_llm_api_key(const std::filesystem::path &path) {
#if defined(_WIN32)
    return erase_credential_file(path);
#else
    (void) path;
    const char *configured = std::getenv("SUNSHINE_LLM_API_KEY");
    if (configured && *configured) {
      return { false, "Remove SUNSHINE_LLM_API_KEY from the service environment to clear the API key" };
    }
    return { true, {} };
#endif
  }

  read_result_t
  read_codex_credential(const std::filesystem::path &path) {
#if defined(_WIN32)
    return read_dpapi_secret(path, "Sunshine/AI/CodexAccount/v1");
#else
    (void) path;
    return { read_status_e::error, {}, "Secure OpenAI account persistence is only supported on Windows" };
#endif
  }

  mutation_result_t
  write_codex_credential(const std::filesystem::path &path, const std::string &secret) {
    if (secret.empty() || secret.size() > 32 * 1024) {
      return { false, "OpenAI account credential has an invalid size" };
    }
#if defined(_WIN32)
    return write_dpapi_secret(path, secret, "Sunshine/AI/CodexAccount/v1",
                              L"Sunshine OpenAI account", 0);
#else
    (void) path;
    return { false, "Secure OpenAI account persistence is only supported on Windows" };
#endif
  }

  mutation_result_t
  erase_codex_credential(const std::filesystem::path &path) {
    return erase_credential_file(path);
  }

}  // namespace credential_store
