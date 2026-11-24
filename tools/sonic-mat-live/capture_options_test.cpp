#include "capture_options.h"
#include <iostream>
int
main() {
  try {
    using sonic_live::capture_limit;
    if (capture_limit(1, L"--continuous") != 0 || capture_limit(2, L"--max-seconds", L"1") != 1 || capture_limit(2, L"--max-seconds", L"60") != 60) return 1;
    for (auto text : { L"", L"0", L"61", L"-1", L"+8", L"8suffix", L" 8", L"18446744073709551616" }) {
      bool rejected = false;
      try {
        capture_limit(2, L"--max-seconds", text);
      }
      catch (...) {
        rejected = true;
      }
      if (!rejected) return 2;
    }
    for (int count : { 0, 2, 3 }) {
      bool rejected = false;
      try {
        capture_limit(count, L"--continuous", L"8");
      }
      catch (...) {
        rejected = true;
      }
      if (!rejected) return 3;
    }
    bool rejected = false;
    try {
      capture_limit(1, L"--max-seconds");
    }
    catch (...) {
      rejected = true;
    }
    if (!rejected) return 4;
    std::cout << "PASS explicit continuous/bounded capture options, malformed bounds fail closed; nativeCapture=0\n";
    return 0;
  }
  catch (...) {
    return 5;
  }
}
