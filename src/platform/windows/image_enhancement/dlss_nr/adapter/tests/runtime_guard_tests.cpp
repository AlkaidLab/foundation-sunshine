/**
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 Foundation Sunshine contributors
 * @brief Runtime lifecycle regressions; no NVIDIA runtime or GPU required.
 */
#include "../src/dlssnr_adapter.cpp"

#include <cstdio>

namespace {
  std::atomic<unsigned> destroy_calls { 0 };

  __declspec(noinline) NVSDK_NGX_Result NVSDK_CONV
  faulting_destroy(NVSDK_NGX_Parameter *) {
    ++destroy_calls;
    RaiseException(0xE0424242, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return NVSDK_NGX_Result_Success;
  }

  instance_t *
  make_faulting_instance() {
    auto *instance = new instance_t();
    // The callback deliberately faults without dereferencing the opaque value.
    instance->parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(1);
    instance->destroy_parameters = faulting_destroy;
    return instance;
  }
}

int
main() {
  if (!ngx_succeeded(NVSDK_NGX_Result_Success) ||
      ngx_succeeded(NVSDK_NGX_Result_FAIL_NotInitialized)) {
    std::puts("FAIL: NGX result classification");
    return 1;
  }
  adapter_destroy(make_faulting_instance());
  if (!g_faulted.load() || destroy_calls != 1) {
    std::printf("FAIL: parameter teardown must catch and latch an SEH fault (latched=%d, calls=%u)\n",
      g_faulted.load() ? 1 : 0, destroy_calls.load());
    return 1;
  }
  adapter_destroy(make_faulting_instance());
  if (destroy_calls != 1) {
    std::puts("FAIL: teardown must not reenter a faulted runtime");
    return 1;
  }

  const auto module = GetModuleHandleW(nullptr);
  wchar_t original_path[MAX_PATH] {};
  if (!GetModuleFileNameW(nullptr, original_path, MAX_PATH)) return 1;
  // Patch this executable's real import table to exercise the same mechanism
  // used for a loaded snippet, including calls that must forward to Windows.
  if (!install_caller_compatibility(module) || !install_caller_compatibility(module)) {
    std::puts("FAIL: repeated hook installation");
    return 1;
  }
  wchar_t forwarded_path[MAX_PATH] {};
  if (!GetModuleFileNameW(nullptr, forwarded_path, MAX_PATH) ||
      std::wcscmp(original_path, forwarded_path) != 0) {
    std::puts("FAIL: original API forwarding after repeated installation");
    return 1;
  }
  wchar_t caller[MAX_PATH] {};
  if (!GetModuleFileNameW(module, caller, MAX_PATH) ||
      std::wcscmp(caller, L"nvngx.dll") != 0) {
    std::puts("FAIL: caller compatibility after repeated installation");
    return 1;
  }
  std::puts("Runtime lifecycle regressions passed");
  return 0;
}
