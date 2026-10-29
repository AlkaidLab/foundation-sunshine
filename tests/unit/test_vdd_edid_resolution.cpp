#include "src/display_device/vdd_edid_resolution.h"

#include <iostream>
#include <stdexcept>

int main() {
  using namespace display_device::vdd_edid;
  struct selection_case {
    const char *value; const char *resolutions; const char *fps; bool expected;
  };
  const selection_case cases[] = {
    {"default", "[]", "[]", true},
    {"3440x1440", "[1920x1080,3440x1440,3840x2160]", "[60,90,120]", true},
    {"2037x1109", "[2037X1109]", "[59.94]", true},
    {"3840x2160", "[3840x2160]", "[60]", true},
    {"3840x2160", "[3840x2160]", "[120,60]", true},
    {"4095x4095", "[4095x4095]", "[30]", true},
    {"3440x1440", "[1920x1080,3840x2160]", "[60]", false},
    {"3840x2160", "[3840x2160]", "[120,144]", false},
    {"3840x2160", "[3840x2160]", "[90]", false},
    {"5120x1440", "[5120x1440]", "[60]", false},
    {"4096x2160", "[4096x2160]", "[60]", false},
    {"0x1080", "[0x1080]", "[60]", false},
    {"1x1", "[1x1]", "[1]", false},
    {"3440x1440", "[3440x1440]", "[bad,0,481,nan,inf]", false},
    {"999999999999999999999999x1440", "[]", "[60]", false},
    {"3440x1440junk", "[3440x1440]", "[60]", false},
    {"3440x1440</EdidNativeResolution>", "[3440x1440]", "[60]", false},
    {"", "[1920x1080]", "[60]", false},
    {"1920x1080", "[]", "[60]", false},
  };
  try {
    for (const auto &test : cases) {
      if (supported_selection(test.value, test.resolutions, test.fps) != test.expected) {
        throw std::runtime_error(std::string("Incorrect EDID validation for ") + test.value);
      }
    }
    std::cout << "PASS: 19 EDID API selection cases (configured/custom sizes, default, removal, dimensions, refresh, clocks, malformed input)\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
