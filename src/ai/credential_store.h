/**
 * @file src/ai/credential_store.h
 * @brief Platform-backed storage for the LLM API credential.
 */
#pragma once

#include <filesystem>
#include <string>

namespace credential_store {

  enum class read_status_e {
    success,
    not_found,
    error
  };

  struct read_result_t {
    read_status_e status = read_status_e::error;
    std::string secret;
    std::string error;
  };

  struct mutation_result_t {
    bool success = false;
    std::string error;
  };

  /**
   * The path is used for the DPAPI-protected blob on Windows. Other platforms
   * read SUNSHINE_LLM_API_KEY and do not persist credentials.
   */
  read_result_t
  read_llm_api_key(const std::filesystem::path &path);

  mutation_result_t
  write_llm_api_key(const std::filesystem::path &path, const std::string &secret);

  mutation_result_t
  erase_llm_api_key(const std::filesystem::path &path);

}  // namespace credential_store
