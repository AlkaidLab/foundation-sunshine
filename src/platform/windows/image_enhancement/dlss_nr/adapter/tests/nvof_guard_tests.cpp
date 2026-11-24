// SPDX-License-Identifier: GPL-3.0-only
// The driver boundary must catch SEH faults and prevent subsequent reentry.
#include "../src/nvof_provider.cpp"
#include <cstdio>

int
main() {
  unsigned calls = 0;
  auto fail = [&]() noexcept -> NV_OF_STATUS {
    ++calls;
    RaiseException(0xE0424242, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return NV_OF_SUCCESS;
  };
  if (invoke([&]() noexcept { return fail(); }) != NV_OF_ERR_GENERIC ||
      !faulted.load() || calls != 1) {
    std::puts("FAIL: NVOF SEH fault was not caught and latched");
    return 1;
  }
  if (invoke([&]() noexcept { return fail(); }) != NV_OF_ERR_GENERIC || calls != 1) {
    std::puts("FAIL: a faulted NVOF runtime was called again");
    return 1;
  }
  std::puts("NVOF runtime fault guard passed");
  return 0;
}
