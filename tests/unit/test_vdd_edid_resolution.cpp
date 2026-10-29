#include "src/display_device/vdd_edid_resolution.h"

#include <iostream>
#include <stdexcept>

int
main() {
  using namespace display_device::vdd_edid;
  struct selection_case {
    const char *value;
    const char *resolutions;
    const char *fps;
    bool expected;
  };
  const selection_case cases[] = {
    { "default", "[]", "[]", true },
    { "3440x1440", "[1920x1080,3440x1440,3840x2160]", "[60,90,120]", true },
    { "2037x1109", "[2037X1109]", "[59.94]", true },
    { "3840x2160", "[3840x2160]", "[60]", true },
    { "3840x2160", "[3840x2160]", "[120,60]", true },
    { "4095x4095", "[4095x4095]", "[30]", true },
    { "3440x1440", "[1920x1080,3840x2160]", "[60]", false },
    { "3840x2160", "[3840x2160]", "[120,144]", false },
    { "3840x2160", "[3840x2160]", "[90]", false },
    { "5120x1440", "[5120x1440]", "[60]", false },
    { "4096x2160", "[4096x2160]", "[60]", false },
    { "0x1080", "[0x1080]", "[60]", false },
    { "1x1", "[1x1]", "[1]", false },
    { "3440x1440", "[3440x1440]", "[bad,0,481,nan,inf]", false },
    { "999999999999999999999999x1440", "[]", "[60]", false },
    { "3440x1440junk", "[3440x1440]", "[60]", false },
    { "3440x1440</EdidNativeResolution>", "[3440x1440]", "[60]", false },
    { "", "[1920x1080]", "[60]", false },
    { "1920x1080", "[]", "[60]", false },
  };
  try {
    for (const auto &test : cases) {
      if (supported_selection(test.value, test.resolutions, test.fps) != test.expected) {
        throw std::runtime_error(std::string("Incorrect EDID validation for ") + test.value);
      }
    }
    const std::map<std::string, std::string> existing {
      { "vdd_edid_native_resolution", "3840x2160" },
      { "resolutions", "[1920x1080,3840x2160]" }, { "fps", "[60,120]" }
    };
    unsigned retained_cases = 0;
    auto check_retained = [&](const std::map<std::string, std::string> &incoming,
                            const std::map<std::string, std::string> &saved, bool expected) {
      ++retained_cases;
      if (retained_selection_supported(incoming, saved) != expected)
        throw std::runtime_error("Incorrect retained EDID validation, case " + std::to_string(retained_cases));
    };
    check_retained({}, existing, true);
    check_retained({ { "sunshine_name", "legacy" } }, existing, true);
    check_retained({ { "resolutions", "[1920x1080,3840x2160]" } }, existing, true);
    check_retained({ { "resolutions", "[2560x1440,3840x2160]" } }, existing, true);
    check_retained({ { "resolutions", "[1920x1080]" } }, existing, false);
    check_retained({ { "resolutions", "[]" } }, existing, false);
    check_retained({ { "fps", "[120,144]" } }, existing, false);
    check_retained({ { "fps", "[]" } }, existing, false);
    check_retained({ { "fps", "[120,60]" } }, existing, true);
    check_retained({ { "resolutions", "[1920x1080]" }, { "vdd_edid_native_resolution", "default" } }, existing, true);
    check_retained({ { "resolutions", "[]" } }, { { "vdd_edid_native_resolution", "default" } }, true);
    check_retained({ { "resolutions", "[]" } }, {}, true);
    std::cout << "PASS: 19 EDID selection cases and " << retained_cases << " legacy mode-list update cases\n";
    return 0;
  }
  catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
