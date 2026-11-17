/**
 * @file src/crash/crash_env.h
 * @brief The machine, for the "env" object of a crash report: CPU, GPUs and their driver, memory, displays, language, how it was installed.
 *
 * Fixed vocabulary, numbers and short model strings only: no computer name, no account name, no path, no address, no serial number. The service keeps only what matches
 * its own format rules (the same ones as sanitize_model / sanitize_version here), so a crash report can say "most crashes are on this GPU driver" and nothing more.
 */
#pragma once
#include <string>
#include <string_view>

namespace crash::env {

  /// A hardware model: letters, digits and . + - _ ( ) / @ and spaces, whitespace collapsed, at most 64 characters; empty when nothing is left.
  std::string
  sanitize_model(std::string_view raw);

  /// A version string: letters, digits and . - _ +, at most 32 characters; empty when nothing is left.
  std::string
  sanitize_version(std::string_view raw);

  /// "nvidia", "amd", "intel", "apple", "qualcomm", "microsoft" or "other".
  std::string
  gpu_vendor_word(std::string_view vendor_or_name);

  /// The JSON text of the machine, to install with crash::set_environment_provider().
  std::string
  describe_machine();

}  // namespace crash::env
